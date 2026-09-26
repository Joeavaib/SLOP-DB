#pragma once

// BackupCoordinator (offline, single-shard, STL/POSIX-only, C++20).
//
// Ablauf Backup(src..., dstDir) — Aufrufer hält ALLE Writer an (offline):
//   1. wal->flush(); durable_lsn = wal->durable_lsn() merken.
//   2. ColumnarStore::Save(dstDir/columnar)
//   3. RaftGroup::SaveLog(dstDir/raft.log) + SaveSnapshot(dstDir/raft.snap)
//   4. WAL-Datei src_wal_path -> dstDir/wal.log kopieren (binaer + fsync).
//   5. MANIFEST (dstDir/MANIFEST) ZULETZT, atomar via tmp+rename+fsync
//      (File + Directory), Muster nach storage/wal.cpp.
//
// Ablauf Restore(backupDir, ...) — umgekehrte Reihenfolge:
//   1. MANIFEST lesen + validieren (Magic, Felder).
//   2. wal.log -> dst_wal_path kopieren; per replay_file() verifizieren,
//      dass max-LSN == manifest.wal_lsn.
//   3. RaftGroup::LoadLog(raft.log), dann LoadSnapshot(raft.snap)
//      (Snapshot overlay kompaktiert; Commit muss manifest.commit matchen).
//   4. ColumnarStore::Load(columnar); TotalRows muss manifest.rows matchen.
//   5. WAL-Tail ab manifest.wal_lsn+1 via replay_file/read_from zaehlen
//      (offline: Erwartung 0). SQL-Schicht kann Tail-Records zusaetzlich
//      via sql::Executor::recover() anwenden (s. executor.h).
//
// MANIFEST-Format (Text, key=value, erste Zeile Magic):
//   DBBACKUP1
//   wal_lsn=<u64>     ... WAL durable_lsn zum Backup-Zeitpunkt (Snapshot-LSN)
//   rows=<u64>        ... ColumnarStore::TotalRows()
//   term=<u64>        ... RaftGroup::term()
//   commit=<u64>      ... RaftGroup::commitIndex()
//
// Layout im Backup-Verzeichnis:
//   backup/
//     wal.log          WAL-Kopie (binaer, WAL1-Format)
//     raft.log         RAFT1-Log (SaveLog)
//     raft.snap        RSNP1-Snapshot (SaveSnapshot)
//     columnar/        ColumnarStore::Save-Verzeichnis (manifest.txt + part-*.col)
//     MANIFEST         s. oben, ZULETZT geschrieben
//
// Einschraenkungen (Grundgeruest, dokumentiert auch in backup.cpp-main):
//   - OFFLINE: keine Writer waehrend Backup/Restore (kein Online-Snapshot,
//     kein LSB/Incremental, kein fs-Lock, kein Checkpoint-Protokoll).
//   - SINGLE-SHARD: genau eine RaftGroup (kein Multi-Shard, kein Split/Merge).
//   - KEIN PITR: nur Voll-Backup auf Stand wal_lsn; Tail-Count ist Diagnose,
//     kein zeitbasiertes Replay (keine LSN->Timestamp-Map, kein Log-Shipping).
//   - WAL-Copy ist Datei-Copy nach flush(); kein hardlink/snapshot-Fallback.
//   - File-CLI (--backup/--restore) kennt term/commit ggf. nicht (0 wenn die
//     Raft-Quellfiles fehlen); Objekt-API (Backup/Restore) schreibt exakte Werte.

#include <cstddef>
#include <cstdint>
#include <string>

namespace dbengine::columnar {
class ColumnarStore;
}
namespace dbengine::raft {
class RaftGroup;
}
namespace dbengine::storage {
class Wal;
}

namespace dbengine::backup {

struct Manifest {
  std::uint64_t wal_lsn = 0;
  std::uint64_t rows = 0;
  std::uint64_t term = 0;
  std::uint64_t commit = 0;
};

inline constexpr const char* kManifestName = "MANIFEST";
inline constexpr const char* kManifestMagic = "DBBACKUP1";
inline constexpr const char* kWalName = "wal.log";
inline constexpr const char* kRaftLogName = "raft.log";
inline constexpr const char* kRaftSnapName = "raft.snap";
inline constexpr const char* kColumnarDir = "columnar";

class BackupCoordinator {
 public:
  // Objekt-API (bevorzugt, exakte Manifest-Werte aus Live-Stores).
  // wal darf nullptr sein, wenn src_wal_path reicht? Nein: flush() braucht
  // die Instanz — wal==nullptr oder col/raft==nullptr => false + err.
  // dst_dir wird erstellt (mkdir -p). Bei Fehler bleibt ein ggf. altes
  // MANIFEST unversehrt (nur vollstaendiges Backup schreibt MANIFEST neu).
  static bool Backup(storage::Wal* wal, const columnar::ColumnarStore* col,
                     const raft::RaftGroup* raft,
                     const std::string& src_wal_path, const std::string& dst_dir,
                     std::string* err);

  // Stellt aus backup_dir wieder her: WAL nach dst_wal_path kopieren,
  // raft/col in die uebergebenen (leeren) Objekte laden. out_tail_records
  // (optional) = Zahl WAL-Records mit lsn > manifest.wal_lsn (offline: 0).
  static bool Restore(const std::string& backup_dir,
                      columnar::ColumnarStore* col, raft::RaftGroup* raft,
                      const std::string& dst_wal_path, std::string* err,
                      std::size_t* out_tail_records = nullptr);

  // Datei-API (CLI-Grundgeruest ohne Live-Objekte): kopiert WAL + optional
  // Columnar-Verzeichnis + optionale Raft-Files, schreibt MANIFEST zuletzt.
  // wal_lsn aus replay_file(src_wal), rows aus ColumnarStore::Load(src_col)
  // (0 wenn src_col leer/nicht gegeben), term/commit 0 (unbekannt file-level).
  static bool BackupFiles(const std::string& src_wal,
                          const std::string& src_col_dir,
                          const std::string& src_raft_log,
                          const std::string& src_raft_snap,
                          const std::string& dst_dir, std::string* err);

  // Datei-Restore: wal.log -> dst_wal, columnar/ -> dst_col_dir (wenn
  // dst_col_dir nicht leer), raft-Files NICHT automatisch zurueckkopiert
  // (Objekt-Restore via Restore() nutzen). Zaehlt WAL-Tail ab wal_lsn+1.
  static bool RestoreFiles(const std::string& backup_dir,
                           const std::string& dst_wal,
                           const std::string& dst_col_dir, std::string* err,
                           std::size_t* out_tail_records = nullptr);

  static bool ReadManifest(const std::string& backup_dir, Manifest* out,
                           std::string* err);

 private:
  static bool WriteManifestAtomic(const std::string& dst_dir,
                                  const Manifest& m, std::string* err);
  static bool CopyFileFsync(const std::string& src, const std::string& dst,
                            std::string* err);
  static bool CopyDirRecursive(const std::string& src, const std::string& dst,
                               std::string* err);
};

}  // namespace dbengine::backup
