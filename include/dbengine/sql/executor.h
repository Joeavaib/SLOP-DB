#pragma once

// s11-executor: SQL-Executor verdrahtet Parser + KVStore + MvccStore + WAL.
// - TableRegistry: Name (lowercase, PG-Folding) -> Schema (ColumnDefs) + RowID-Counter
// - INSERT: validiert + koerziert gegen Schema, schreibt KV-Key
//     "sql/<table>/<pk>" (pk = valueToString(erste Spalte), Kollision -> "#rowid"-Suffix),
//     WAL-append (1 Record pro Zeile, group-commit via flush pro Statement)
//     + KV-WriteBatch + MVCC-Commit (Single-Writer, blocking).
// - SELECT: KV-Snapshot (Prefix-Scan) fuer Key-Menge + MVCC-Snapshot-Read je Key,
//     sichtbar NUR bei committed MVCC-Version (KV-only Keys ohne MVCC-Commit
//     bleiben unsichtbar: kein KV-Fallback-Dirty-Read),
//     Filter/Projektion/COUNT(*) via In-Memory-Database (parser-kompatibel).
// - recover(): WAL-Replay (CREATE + INSERT-Records) in leere/frische Stores
//     (Restart-Szenario). Idempotent fuer bereits vorhandene Keys (last-wins).
//     Mit aktivem Mirror-Checkpoint (enableMirror) wird nur der WAL-Tail
//     (lsn > mirror_lsn) replayt, danach der Spiegel nachgezogen.
// - Mirror-Checkpoint (optional, Default aus): BTreeKV-Sidecar (<db>.btree)
//     als Latest-State je Key + Spiegel-LSN. Pro WAL-Flush inkrementell via
//     WAL-read_from(mirror_lsn+1) nachgezogen; Start laedt Latest-State in
//     KV/MVCC/Registry/Replika und replayt nur den Tail. WAL bleibt Wahrheit:
//     Spiegel-Fehler -> Voll-Replay, nie Datenverlust durch den Spiegel.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dbengine/columnar/store.h"
#include "dbengine/kv.h"
#include "dbengine/kv/btree.h"
#include "dbengine/sql/parser.h"
#include "dbengine/storage/wal.h"
#include "dbengine/txn/mvcc.h"

namespace dbengine::sql {

struct TableSchema {
  std::vector<ColumnDef> columns;
  std::uint64_t next_rowid = 0;
};

// ---- pg_stat_statements-light (session-lokal, keine Persistenz) --------------
// Pro normalisiertem Query-Text (Literale -> '?') werden gezaehlt: calls,
// total_ms, rows_out, errors. Normalisierung s. normalizeQuery (Zahlen,
// Single-/Double-quoted Strings -> '?', Whitespace kollabiert, kein
// Lowercasing der Keywords). Overhead minimal (eine Map + chrono pro
// execute(), s. executor.cpp). Session-lokal: recover()/Restart setzt NICHT
// zurueck, aber es gibt keine WAL-/Disk-Persistenz (Restart = leer).
// Top-N via queryStats(n) (sortiert total_ms absteigend); Metrics-Render
// (Top-5) s. src/server/metrics.cpp (SetPgStats/RenderPgStats).
struct QueryStat {
  std::string query;  // normalisierter Query-Text
  std::uint64_t calls = 0;
  double total_ms = 0.0;
  std::uint64_t rows_out = 0;  // SELECT: rows.size(), DML: affected (Summe)
  std::uint64_t errors = 0;    // geworfene SqlError/sonstige Exceptions
};

// ---- Slow-Query-Log (session-lokal, keine Persistenz, kein stderr) ----------
// Entscheidung (s. Aufgabe): KEIN stderr-Spam im Lib-Code, KEINE Metrics.cpp-
// Integration (verboten), KEIN Export in den queryStats-Dump (QueryStat-Format
// stabil fuer Metrics-Render). Nur Sammlung + Getter, Executor-seitig:
//   setSlowLogThresholdMs(ms): Schwellwert in ms; <= 0 / NaN = aus (Default).
//   slowQueries(n): Top-N nach max_ms absteigend (limit == 0 -> alle, max Cap).
//   clearSlowLog(): leert nur das Slow-Log (clearQueryStats tastet es nicht an).
// Speicherung: pro normalisiertem Query-Text (identisch zu normalizeQuery) EIN
// Eintrag mit max/last-Zeit (kein unbegrenztes Per-Statement-Log):
//   calls = Anzahl der als-slow gewerteten Ausfuehrungen (Subset von
//           QueryStat.calls, Fehler inkl. mit rows 0), max_ms = Maximum,
//           last_ms = letzte slow-Ausfuehrung, rows_out = Summe rows_out der
//           slow-Ausfuehrungen (SELECT rows.size()+affected, Fehler = 0).
// Schranke: kSlowLogCap = 64 Eintraege. Eviction bei voller Map: neuer Key wird
// nur aufgenommen, wenn sein ms strikt groesser ist als das kleinste max_ms;
// dann wird genau dieser schnellste Eintrag verdraengt (O(Cap) Scan, Cap klein).
// Aktualisierung bestehender Keys immer (max = max, last = ms).
// Aufzeichnung: nur wenn threshold > 0 UND ms >= threshold (Grenze inklusiv).
// Thread-Safety: derselbe pgstat_mu_ wie pgstat_ (ein Lock pro execute()).
// Overhead bei deaktiviertem Log: ein double-Vergleich unter dem bestehenden
// Lock (kein Zusatz-Lock, keine Allok). Session-lokal: recover()/Restart setzt
// NICHT zurueck, keine WAL-/Disk-Persistenz.
struct SlowQueryStat {
  std::string query;  // normalisierter Query-Text
  std::uint64_t calls = 0;  // slow-Ausfuehrungen (Fehler inkl.)
  double max_ms = 0.0;
  double last_ms = 0.0;
  std::uint64_t rows_out = 0;  // Summe rows_out der slow-Ausfuehrungen
};

class Executor {
 public:
  Executor(kv::KVStore& kv, txn::MvccStore& mvcc, storage::Wal* wal = nullptr);
  ~Executor();

  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  Result execute(const std::string& sql);

  // pg_stat_statements-light: Einstiegspunkt ist execute() (alle Statements).
  static std::string normalizeQuery(const std::string& sql);
  // Top-N nach total_ms (Default 5, passend zum Metrics-Top-5-Render).
  // limit == 0 -> alle Eintraege.
  std::vector<QueryStat> queryStats(std::size_t top_n = 5) const;
  void clearQueryStats();

  // Slow-Query-Log: Schwellwert in ms (Default aus/0). ms <= 0 oder NaN = aus.
  // Aufzeichnung in execute(): ms >= threshold (Grenze inklusiv, Erfolg+Fehler).
  static constexpr std::size_t kSlowLogCap = 64;
  void setSlowLogThresholdMs(double ms);
  double slowLogThresholdMs() const;
  // Top-N nach max_ms absteigend (Tie: calls absteigend, dann query aufsteigend).
  // limit == 0 -> alle Eintraege (max kSlowLogCap).
  std::vector<SlowQueryStat> slowQueries(std::size_t top_n = 0) const;
  void clearSlowLog();

  // ---- Mirror-Checkpoint (BTreeKV-Sidecar, Default: aus) --------------------
  // enableMirror(path) oeffnet (ggf. erzeugt) das Sidecar und laedt dessen
  // Latest-State in die (idealerweise frischen) Stores: Schemas -> Registry +
  // KV, Rows -> KV + frische Single-Version-MVCC-Ketten (alle committed, im
  // frischen Prozess ohne aktive Txns daher alle sichtbar) + Scan-Replika.
  // Danach replayt recover() nur den WAL-Tail (lsn > mirror_lsn).
  // Rueckgabe false (mit Warntext in *warn): Sidecar unbrauchbar -> Aufrufer
  // faehrt ohne Spiegel fort (Voll-Replay, WAL bleibt Wahrheit). Korrupte
  // Einzeleintraege degradieren zu mirror_lsn = 0 (Voll-Replay heilt den
  // Stand, last-wins). Wirft nie (Fehler -> false + *warn).
  bool enableMirror(const std::string& path, std::string* warn = nullptr);
  void disableMirror();
  bool mirrorEnabled() const { return mirror_on_; }
  std::uint64_t mirrorLsn() const { return mirror_lsn_; }
  const std::string& mirrorPath() const { return mirror_path_; }

  // WAL-Replay in Registry + KV + MVCC (kein erneutes WAL-Append).
  // Gibt die Anzahl uebersprungener (korrupter/unbekannter) Records zurueck;
  // ein discarding Aufruf `recover();` ohne Verwendung des Rueckgabewerts
  // bleibt kompilierfaehig (bestehende Aufrufer brechen nicht).
  // Detail-Statistik zusaetzlich via recover_skipped()/recover_applied().
  std::size_t recover();
  std::size_t recover_skipped() const { return recover_skipped_; }
  std::size_t recover_applied() const { return recover_applied_; }

  // ---- MVCC-Purge / VACUUM (Undo-GC, Executor-seitig) -----------------------
  // MvccStore::Purge() (s. txn/mvcc.h: `std::size_t Purge()`) entfernt pro Key
  // alte Versionen, die fuer kein aktives Snapshot mehr sichtbar sein koennen,
  // und fasst die neueste Version nie an (kein Heap-Bloat, da Single-Version
  // in der Primary-Kette + Undo-Historie). Korrektheit wird VOLL an die
  // Purge-API delegiert: der Executor filtert/waehlt selbst keine Versionen,
  // daher kann Auto-Purge/VACUUM niemals eine Version entfernen, die ein
  // aktives Snapshot noch braucht (Purge prueft die active_-Map intern).
  // Best-effort: Purge wirft nie in den DML-Pfad (Fehler -> 0, kein Throw).
  //
  // vacuum(): globales Purge ueber alle Keys (Rueckgabe = befreite Versionen).
  // vacuum(tabelle): validiert Existenz (unbekannt -> SqlError wie
  // UPDATE/DELETE), ruft danach mangels per-Key-Purge in der MVCC-API
  // ebenfalls das globale Purge (Effekt ggf. tabellenuebergreifend).
  // Tabellenname PG-gefoldet (normalizeTable).
  // SQL: "VACUUM [VERBOSE|ANALYZE] [tabelle][;]" (Prefix in executeInner,
  // ohne Parser-Umbau; Rueckgabe-Message "VACUUM <freed>", affected=freed).
  // Auto-Purge: nach jedem erfolgreichen UPDATE/DELETE/DROP wird die Zahl
  // obsoletierter Versionen (affected) auf auto_purge_pending_ akkumuliert;
  // erreicht sie auto_purge_threshold_ (Default 1000), laeuft genau ein
  // Purge() und der Zaehler wird zurueckgesetzt. Schwelle 0 = aus.
  std::size_t vacuum();
  std::size_t vacuum(const std::string& table);
  void setAutoPurgeThreshold(std::size_t n) { auto_purge_threshold_ = n; }
  std::size_t autoPurgeThreshold() const { return auto_purge_threshold_; }
  std::size_t autoPurgePending() const { return auto_purge_pending_; }
  std::size_t lastPurgeFreed() const { return last_purge_freed_; }

  bool hasTable(const std::string& name) const;
  const TableSchema* schemaOf(const std::string& name) const;

  // Key-/Codec-Helpers (fuer Tests/Debug oeffentlich).
  static std::string normalizeTable(const std::string& table);
  static std::string tablePrefix(const std::string& table);
  static std::string schemaEncode(const std::vector<ColumnDef>& cols);
  static std::vector<ColumnDef> schemaDecode(const std::string& s);
  static std::string encodeRow(const std::vector<Value>& row);
  static std::vector<Value> decodeRow(const std::string& s, std::size_t ncols);

 private:
  Result executeInner(const std::string& sql);
  Result execCreate(const CreateTableStmt& s);
  Result execInsert(const InsertStmt& s);
  Result execSelect(const SelectStmt& s);

  void applyCreateRecord(const std::string& table, const std::string& schemaEnc);
  // true bei erfolgreich replaytem Insert, false wenn ohne Schema oder
  // MVCC-Write fehlschlug (zaehlt in recover() als Skip).
  bool applyInsertRecord(const std::string& table, const std::string& key,
                         const std::string& rowEnc);

  // ---- HTAP Scan-Replika (ein ColumnarStore pro Tabelle) -------------------
  // KV/MVCC bleibt einzige Wahrheit fuer Writes/Point-Reads. INSERT/UPDATE-
  // Commits spiegeln Rows in die Replika (Commit-TS aus der MVCC-Kette, exakt
  // ein TS pro Writer-Commit -> Statement atomar sichtbar). SELECT-Full-Scans
  // lesen die Replika (kein KV-Snapshot, kein MVCC-Read pro Zeile) mit Regel
  // begin_ts <= snapshot < end_ts; Store-Prune via minBegin. UPDATE = Upsert
  // mit Versionskette, DELETE = Tombstone (end_ts), DROP = erase. Replika ist
  // in-memory only; recover()/WAL-Replay baut sie wieder auf. Ohne Tags (leere
  // Replika) gilt immer-sichtbar.
  std::uint64_t commitTsOf(const std::string& key);
  void mirrorUpsertOne(const std::string& norm, const std::string& key,
                       const std::string& enc, std::uint64_t commit_ts);
  void mirrorEraseOne(const std::string& norm, const std::string& key,
                      std::uint64_t commit_ts);
  // Selbstheilung: Latest-State aus KV-Keymenge + neuesten MVCC-Versionen.
  void rebuildReplicaForTable(const std::string& norm);

  // Spiegel-Seiteneffekt EINES WAL-Records (nur Spiegel, kein KV/MVCC):
  // exakt die KV/Registry-Wirkung von recover() (inkl. Skip-Regeln: INSERT/
  // UPDATE/DELETE ohne Schema = No-Op, korrupt/unbekannt = No-Op). true =
  // 1:1 abgebildet (Watermark darf vorruecken), false = Put-Limit o.Ae.
  // (Watermark bleibt stehen -> Tail-Replay holt es nach).
  bool applyMirrorRecord(const std::string& data);
  // Inkrementell: Records seit mirror_lsn via wal_->read_from spiegeln,
  // danach mirror_lsn = durable persistieren (ein BTreeKV-Flush, atomar).
  // Nie werfend, nie Execute-scheiternd: Spiegel-Fehler bleiben still.
  void syncMirrorFromWal();
  // Batch-Variante: wendet gerade geschriebene Payloads direkt an (ohne
  // WAL-Re-Read; O(Batch) statt O(WAL)). Nullptr = Vollscan wie oben.
  void syncMirrorBatch(const std::vector<std::string>* batch);

  // Auto-Purge-Helfer: akkumuliert obsoletierte Versionen und purgt bei
  // erreichter Schwelle genau einmal (best-effort, nie werfend). Muss NACH
  // erfolgreichem MVCC-Commit + Replika-Spiegelung aufgerufen werden.
  void maybeAutoPurge(std::size_t newly_obsoleted) noexcept;

  // Auto-Purge-State: pending = seit letztem Purge akkumulierte
  // obsoletierte Versionen (UPDATE/DELETE/DROP-affected); threshold = 0 aus.
  std::size_t auto_purge_pending_ = 0;
  std::size_t auto_purge_threshold_ = 1000;
  std::size_t last_purge_freed_ = 0;

  kv::KVStore& kv_;
  txn::MvccStore& mvcc_;
  storage::Wal* wal_ = nullptr;
  std::map<std::string, TableSchema> tables_;  // norm-name -> schema
  std::map<std::string, columnar::ColumnarStore> replica_;  // norm-name -> Scan-Replika
  std::size_t recover_skipped_ = 0;  // Skips des letzten recover()-Laufs
  std::size_t recover_applied_ = 0;  // erfolgreich angewendete Records
  // ---- pg_stat_statements-light-State (session-lokal, keine Persistenz) ----
  // pgstat_mu_ schuetzt pgstat_, slowlog_ und slow_threshold_ms_ gemeinsam
  // (genau ein Lock pro execute(), keine Lock-Ordnung noetig).
  mutable std::mutex pgstat_mu_;
  std::map<std::string, QueryStat> pgstat_;  // normalisierter Text -> Stat
  // ---- Slow-Query-Log-State (Schranke kSlowLogCap, s. Doku bei SlowQueryStat)
  double slow_threshold_ms_ = 0.0;  // <= 0 = aus (Default)
  std::map<std::string, SlowQueryStat> slowlog_;  // norm. Text -> max/last
  // Muss mit gehaltenem pgstat_mu_ aufgerufen werden (nur aus execute()).
  void recordSlowLocked(const std::string& key, double ms, std::uint64_t rows);
  // ---- Mirror-Checkpoint-State -------------------------------------------
  std::unique_ptr<kv::BTreeKV> mirror_;
  std::string mirror_path_;
  std::uint64_t mirror_lsn_ = 0;
  bool mirror_on_ = false;
  // Batch-Zaehlung: Mirror-Flush (teuer: B-Tree-Voll-Rewrite) nur alle
  // mirror_interval_ Statements; Watermark rueckt nur mit Flush vor.
  std::uint64_t mirror_pending_ = 0;
  std::uint64_t mirror_interval_ = 1000;

 public:
  void setMirrorInterval(std::uint64_t n) {
    mirror_interval_ = (n == 0) ? 1 : n;
  }
  // Erzwungener Mirror-Checkpoint (Flush + Watermark = durable).
  // Fuer sauberes Herunterfahren (main ruft vor Exit).
  bool mirrorCheckpoint();
};

}  // namespace dbengine::sql
