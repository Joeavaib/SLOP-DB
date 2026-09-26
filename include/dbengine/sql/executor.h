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

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dbengine/kv.h"
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

  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  Result execute(const std::string& sql);

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

  kv::KVStore& kv_;
  txn::MvccStore& mvcc_;
  storage::Wal* wal_ = nullptr;
  std::map<std::string, TableSchema> tables_;  // norm-name -> schema
  std::size_t recover_skipped_ = 0;  // Skips des letzten recover()-Laufs
  std::size_t recover_applied_ = 0;  // erfolgreich angewendete Records
};

}  // namespace dbengine::sql
