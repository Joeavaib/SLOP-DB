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

class Executor {
 public:
  Executor(kv::KVStore& kv, txn::MvccStore& mvcc, storage::Wal* wal = nullptr);
  ~Executor();

  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  Result execute(const std::string& sql);

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

  kv::KVStore& kv_;
  txn::MvccStore& mvcc_;
  storage::Wal* wal_ = nullptr;
  std::map<std::string, TableSchema> tables_;  // norm-name -> schema
  std::map<std::string, columnar::ColumnarStore> replica_;  // norm-name -> Scan-Replika
  std::size_t recover_skipped_ = 0;  // Skips des letzten recover()-Laufs
  std::size_t recover_applied_ = 0;  // erfolgreich angewendete Records
  // ---- Mirror-Checkpoint-State -------------------------------------------
  std::unique_ptr<kv::BTreeKV> mirror_;
  std::string mirror_path_;
  std::uint64_t mirror_lsn_ = 0;
  bool mirror_on_ = false;
};

}  // namespace dbengine::sql
