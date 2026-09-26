// BackupCoordinator (offline, STL/POSIX, C++20) + dbbackup-main.
// Muster tmp+rename+fsync nach src/storage/wal.cpp (fsync File + Directory).
// C++20 -Wall sauber: keine unbenutzten Parameter, keine Sign-Vergleiche.

#include "dbengine/server/backup.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "dbengine/columnar/store.h"
#include "dbengine/raft/shard.h"
#include "dbengine/storage/wal.h"

namespace dbengine::backup {
namespace {

namespace fs = std::filesystem;

void SetErr(std::string* err, const std::string& msg) {
  if (err) *err = msg;
}

bool FsyncFileByPath(const std::string& path, std::string* err) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    SetErr(err, "open fsync: " + path);
    return false;
  }
  bool ok = true;
#ifdef __linux__
  if (::fdatasync(fd) != 0) ok = (::fsync(fd) == 0);
#else
  ok = (::fsync(fd) == 0);
#endif
  ::close(fd);
  if (!ok) SetErr(err, "fsync file: " + path);
  return ok;
}

void FsyncDirBestEffort(const std::string& dir) {
  int dfd = ::open(dir.c_str(), O_RDONLY
#ifdef O_DIRECTORY
                                        | O_DIRECTORY
#endif
  );
  if (dfd < 0) return;
  (void)::fsync(dfd);
  ::close(dfd);
}

bool EnsureDir(const std::string& dir, std::string* err) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    SetErr(err, "mkdir: " + dir + ": " + ec.message());
    return false;
  }
  return true;
}

std::uint64_t ReplayMaxLsn(const std::string& path) {
  auto recs = storage::Wal::replay_file(path);
  std::uint64_t mx = 0;
  for (const auto& r : recs) mx = r.lsn > mx ? r.lsn : mx;
  return mx;
}

}  // namespace

bool BackupCoordinator::CopyFileFsync(const std::string& src,
                                      const std::string& dst,
                                      std::string* err) {
  std::ifstream in(src, std::ios::binary);
  if (!in) {
    SetErr(err, "open src: " + src);
    return false;
  }
  // Via tmp + rename, damit ein Crash keine halbe Zieldatei hinterlaesst.
  const std::string tmp = dst + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      SetErr(err, "open tmp: " + tmp);
      return false;
    }
    char buf[65536];
    while (in) {
      in.read(buf, sizeof(buf));
      std::streamsize n = in.gcount();
      if (n > 0) {
        out.write(buf, n);
        if (!out) {
          SetErr(err, "write tmp: " + tmp);
          return false;
        }
      }
    }
    out.flush();
    if (!out) {
      SetErr(err, "flush tmp: " + tmp);
      return false;
    }
  }
  if (!FsyncFileByPath(tmp, err)) return false;
  std::error_code ec;
  fs::rename(tmp, dst, ec);
  if (ec) {
    std::error_code ec2;
    fs::remove(tmp, ec2);
    SetErr(err, "rename: " + tmp + " -> " + dst + ": " + ec.message());
    return false;
  }
  auto slash = dst.find_last_of('/');
  std::string dir = (slash == std::string::npos) ? "." : dst.substr(0, slash);
  if (dir.empty()) dir = ".";
  FsyncDirBestEffort(dir);
  return true;
}

bool BackupCoordinator::CopyDirRecursive(const std::string& src,
                                         const std::string& dst,
                                         std::string* err) {
  std::error_code ec;
  if (!fs::exists(src, ec) || ec) {
    SetErr(err, "src dir missing: " + src);
    return false;
  }
  fs::create_directories(dst, ec);
  if (ec) {
    SetErr(err, "mkdir: " + dst + ": " + ec.message());
    return false;
  }
  for (fs::recursive_directory_iterator it(src, ec), end; it != end;
       it.increment(ec)) {
    if (ec) {
      SetErr(err, "iter: " + src + ": " + ec.message());
      return false;
    }
    const fs::path p = it->path();
    std::error_code rel_ec;
    fs::path rel = fs::relative(p, src, rel_ec);
    if (rel_ec) {
      SetErr(err, "relative: " + p.string());
      return false;
    }
    fs::path target = fs::path(dst) / rel;
    if (it->is_directory(ec)) {
      fs::create_directories(target, ec);
      if (ec) {
        SetErr(err, "mkdir: " + target.string());
        return false;
      }
    } else if (it->is_regular_file(ec)) {
      if (!CopyFileFsync(p.string(), target.string(), err)) return false;
    }
    // Symlinks/Specials werden bewusst ignoriert (offline Grundgeruest).
  }
  FsyncDirBestEffort(dst);
  return true;
}

bool BackupCoordinator::WriteManifestAtomic(const std::string& dst_dir,
                                            const Manifest& m,
                                            std::string* err) {
  const std::string path = (fs::path(dst_dir) / kManifestName).string();
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      SetErr(err, "open manifest tmp: " + tmp);
      return false;
    }
    out << kManifestMagic << "\n";
    out << "wal_lsn=" << m.wal_lsn << "\n";
    out << "rows=" << m.rows << "\n";
    out << "term=" << m.term << "\n";
    out << "commit=" << m.commit << "\n";
    out.flush();
    if (!out) {
      SetErr(err, "write manifest tmp: " + tmp);
      return false;
    }
  }
  if (!FsyncFileByPath(tmp, err)) return false;
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    std::error_code ec2;
    fs::remove(tmp, ec2);
    SetErr(err, "rename manifest: " + ec.message());
    return false;
  }
  FsyncDirBestEffort(dst_dir);
  return true;
}

bool BackupCoordinator::ReadManifest(const std::string& backup_dir,
                                     Manifest* out, std::string* err) {
  if (!out) {
    SetErr(err, "null manifest out");
    return false;
  }
  const std::string path = (fs::path(backup_dir) / kManifestName).string();
  std::ifstream in(path);
  if (!in) {
    SetErr(err, "open manifest: " + path);
    return false;
  }
  std::string magic;
  if (!std::getline(in, magic)) {
    SetErr(err, "empty manifest: " + path);
    return false;
  }
  if (!magic.empty() && magic.back() == '\r') magic.pop_back();
  if (magic != kManifestMagic) {
    SetErr(err, "bad manifest magic: " + path);
    return false;
  }
  Manifest m;
  bool have_wal = false, have_rows = false, have_term = false,
       have_commit = false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      SetErr(err, "bad manifest line: " + line);
      return false;
    }
    std::string k = line.substr(0, eq);
    std::string v = line.substr(eq + 1);
    try {
      unsigned long long n = std::stoull(v);
      if (k == "wal_lsn") {
        m.wal_lsn = static_cast<std::uint64_t>(n);
        have_wal = true;
      } else if (k == "rows") {
        m.rows = static_cast<std::uint64_t>(n);
        have_rows = true;
      } else if (k == "term") {
        m.term = static_cast<std::uint64_t>(n);
        have_term = true;
      } else if (k == "commit") {
        m.commit = static_cast<std::uint64_t>(n);
        have_commit = true;
      } else {
        SetErr(err, "unknown manifest key: " + k);
        return false;
      }
    } catch (...) {
      SetErr(err, "bad manifest value: " + line);
      return false;
    }
  }
  if (!have_wal || !have_rows || !have_term || !have_commit) {
    SetErr(err, "incomplete manifest: " + path);
    return false;
  }
  *out = m;
  return true;
}

bool BackupCoordinator::Backup(storage::Wal* wal,
                               const columnar::ColumnarStore* col,
                               const raft::RaftGroup* raft,
                               const std::string& src_wal_path,
                               const std::string& dst_dir, std::string* err) {
  if (!wal || !col || !raft) {
    SetErr(err, "null store pointer (offline: wal/col/raft required)");
    return false;
  }
  if (src_wal_path.empty() || dst_dir.empty()) {
    SetErr(err, "empty path");
    return false;
  }
  // 1. WAL flush -> durable_lsn merken (offline: keine Writer aktiv).
  try {
    wal->flush();
  } catch (const std::exception& e) {
    SetErr(err, std::string("wal flush: ") + e.what());
    return false;
  }
  std::uint64_t durable = 0;
  try {
    durable = wal->durable_lsn();
  } catch (const std::exception& e) {
    SetErr(err, std::string("durable_lsn: ") + e.what());
    return false;
  }
  if (!EnsureDir(dst_dir, err)) return false;

  const std::string col_dst = (fs::path(dst_dir) / kColumnarDir).string();
  const std::string raft_log = (fs::path(dst_dir) / kRaftLogName).string();
  const std::string raft_snap = (fs::path(dst_dir) / kRaftSnapName).string();
  const std::string wal_dst = (fs::path(dst_dir) / kWalName).string();

  // 2. ColumnarStore::Save.
  if (!col->Save(col_dst)) {
    SetErr(err, "columnar Save failed: " + col_dst);
    return false;
  }
  // 3. RaftGroup::SaveLog + SaveSnapshot (brauchen Leader; offline ok).
  if (!raft->SaveLog(raft_log)) {
    SetErr(err, "raft SaveLog failed (leader? quorum?): " + raft_log);
    return false;
  }
  if (!raft->SaveSnapshot(raft_snap)) {
    SetErr(err, "raft SaveSnapshot failed: " + raft_snap);
    return false;
  }
  // 4. WAL-Datei kopieren.
  if (!CopyFileFsync(src_wal_path, wal_dst, err)) return false;

  // 5. MANIFEST ZULETZT (atomar). Werte aus Live-Stores.
  Manifest m;
  m.wal_lsn = durable;
  m.rows = static_cast<std::uint64_t>(col->TotalRows());
  m.term = raft->term();
  m.commit = raft->commitIndex();
  if (!WriteManifestAtomic(dst_dir, m, err)) return false;
  return true;
}

bool BackupCoordinator::Restore(const std::string& backup_dir,
                                columnar::ColumnarStore* col,
                                raft::RaftGroup* raft,
                                const std::string& dst_wal_path,
                                std::string* err,
                                std::size_t* out_tail_records) {
  if (!col || !raft) {
    SetErr(err, "null store pointer (col/raft required)");
    return false;
  }
  if (backup_dir.empty() || dst_wal_path.empty()) {
    SetErr(err, "empty path");
    return false;
  }
  if (out_tail_records) *out_tail_records = 0;

  // 1. MANIFEST zuerst (Gueltigkeitsanker).
  Manifest m;
  if (!ReadManifest(backup_dir, &m, err)) return false;

  const std::string wal_src = (fs::path(backup_dir) / kWalName).string();
  const std::string raft_log = (fs::path(backup_dir) / kRaftLogName).string();
  const std::string raft_snap = (fs::path(backup_dir) / kRaftSnapName).string();
  const std::string col_src = (fs::path(backup_dir) / kColumnarDir).string();

  // 2. WAL zurueckkopieren + per replay_file verifizieren (max-LSN == wal_lsn).
  if (!CopyFileFsync(wal_src, dst_wal_path, err)) return false;
  std::vector<storage::WalRecord> recs;
  try {
    recs = storage::Wal::replay_file(dst_wal_path);
  } catch (const std::exception& e) {
    SetErr(err, std::string("wal replay: ") + e.what());
    return false;
  }
  std::uint64_t mx = 0;
  std::size_t tail = 0;
  for (const auto& r : recs) {
    if (r.lsn > mx) mx = r.lsn;
    if (r.lsn > m.wal_lsn) ++tail;
  }
  if (mx != m.wal_lsn) {
    SetErr(err, "wal lsn mismatch: file=" + std::to_string(mx) +
                    " manifest=" + std::to_string(m.wal_lsn));
    return false;
  }

  // 3. Raft: LoadLog, dann LoadSnapshot (Snapshot-Overlay kompaktiert;
  //    umgekehrte Reihenfolge zum Backup: Log vor Snapshot).
  if (!raft->LoadLog(raft_log)) {
    SetErr(err, "raft LoadLog failed: " + raft_log);
    return false;
  }
  if (!raft->LoadSnapshot(raft_snap)) {
    SetErr(err, "raft LoadSnapshot failed: " + raft_snap);
    return false;
  }
  if (raft->commitIndex() != m.commit) {
    SetErr(err, "raft commit mismatch: got=" +
                    std::to_string(raft->commitIndex()) +
                    " manifest=" + std::to_string(m.commit));
    return false;
  }

  // 4. Columnar laden + Rows pruefen.
  if (!col->Load(col_src)) {
    SetErr(err, "columnar Load failed: " + col_src);
    return false;
  }
  if (static_cast<std::uint64_t>(col->TotalRows()) != m.rows) {
    SetErr(err, "columnar rows mismatch: got=" +
                    std::to_string(col->TotalRows()) +
                    " manifest=" + std::to_string(m.rows));
    return false;
  }

  // 5. WAL-Tail ab wal_lsn+1 (offline: 0). SQL-Layer kann Records zusaetzlich
  //    via sql::Executor::recover() anwenden; hier nur Diagnose-Count.
  //    Hinweis: read_from() braucht offene Wal-Instanz; replay_file() oben
  //    liefert identische Menge (scan ab Dateianfang, torn-tail-tolerant).
  if (out_tail_records) *out_tail_records = tail;
  if (tail != 0) {
    SetErr(err, "wal tail non-empty (online write during backup?): tail=" +
                    std::to_string(tail));
    return false;
  }
  return true;
}

bool BackupCoordinator::BackupFiles(const std::string& src_wal,
                                    const std::string& src_col_dir,
                                    const std::string& src_raft_log,
                                    const std::string& src_raft_snap,
                                    const std::string& dst_dir,
                                    std::string* err) {
  if (src_wal.empty() || dst_dir.empty()) {
    SetErr(err, "src wal / dst required");
    return false;
  }
  if (!EnsureDir(dst_dir, err)) return false;
  const std::string wal_dst = (fs::path(dst_dir) / kWalName).string();
  if (!CopyFileFsync(src_wal, wal_dst, err)) return false;
  std::uint64_t wal_lsn = 0;
  try {
    wal_lsn = ReplayMaxLsn(wal_dst);
  } catch (const std::exception& e) {
    SetErr(err, std::string("wal replay: ") + e.what());
    return false;
  }
  std::uint64_t rows = 0;
  if (!src_col_dir.empty()) {
    const std::string col_dst = (fs::path(dst_dir) / kColumnarDir).string();
    if (!CopyDirRecursive(src_col_dir, col_dst, err)) return false;
    // Rows best-effort aus kopiertem Store lesen (0 bei unlesbar = leeres Backup).
    try {
      columnar::ColumnarStore tmp;
      if (tmp.Load(col_dst))
        rows = static_cast<std::uint64_t>(tmp.TotalRows());
    } catch (...) {
      rows = 0;
    }
  }
  if (!src_raft_log.empty()) {
    const std::string dst = (fs::path(dst_dir) / kRaftLogName).string();
    if (!CopyFileFsync(src_raft_log, dst, err)) return false;
  }
  if (!src_raft_snap.empty()) {
    const std::string dst = (fs::path(dst_dir) / kRaftSnapName).string();
    if (!CopyFileFsync(src_raft_snap, dst, err)) return false;
  }
  Manifest m;
  m.wal_lsn = wal_lsn;
  m.rows = rows;
  m.term = 0;    // file-level unbekannt (Objekt-API liefert exakte Werte)
  m.commit = 0;  // dto.
  if (!WriteManifestAtomic(dst_dir, m, err)) return false;
  return true;
}

bool BackupCoordinator::RestoreFiles(const std::string& backup_dir,
                                     const std::string& dst_wal,
                                     const std::string& dst_col_dir,
                                     std::string* err,
                                     std::size_t* out_tail_records) {
  if (backup_dir.empty() || dst_wal.empty()) {
    SetErr(err, "backup dir / dst wal required");
    return false;
  }
  if (out_tail_records) *out_tail_records = 0;
  Manifest m;
  if (!ReadManifest(backup_dir, &m, err)) return false;
  const std::string wal_src = (fs::path(backup_dir) / kWalName).string();
  if (!CopyFileFsync(wal_src, dst_wal, err)) return false;
  std::vector<storage::WalRecord> recs;
  try {
    recs = storage::Wal::replay_file(dst_wal);
  } catch (const std::exception& e) {
    SetErr(err, std::string("wal replay: ") + e.what());
    return false;
  }
  std::uint64_t mx = 0;
  std::size_t tail = 0;
  for (const auto& r : recs) {
    if (r.lsn > mx) mx = r.lsn;
    if (r.lsn > m.wal_lsn) ++tail;
  }
  if (mx != m.wal_lsn) {
    SetErr(err, "wal lsn mismatch: file=" + std::to_string(mx) +
                    " manifest=" + std::to_string(m.wal_lsn));
    return false;
  }
  if (!dst_col_dir.empty()) {
    const std::string col_src = (fs::path(backup_dir) / kColumnarDir).string();
    std::error_code ec;
    if (fs::exists(col_src, ec)) {
      // Ziel ggf. leeren (nur Grundgeruest: remove + copy).
      std::error_code ec2;
      fs::remove_all(dst_col_dir, ec2);
      if (!CopyDirRecursive(col_src, dst_col_dir, err)) return false;
    }
  }
  if (out_tail_records) *out_tail_records = tail;
  return true;
}

}  // namespace dbengine::backup

// ---- dbbackup-main (klein, im gleichen TU, Vorbild dbmetrics-main) ---------
namespace {

using dbengine::backup::BackupCoordinator;

void Usage(const char* prog) {
  std::fprintf(stderr,
               "usage:\n"
               "  %s --selfcheck\n"
               "  %s --backup --wal <src.wal> --out <backupdir>"
               " [--col <coldir>] [--raft-log <f>] [--raft-snap <f>]\n"
               "  %s --restore --in <backupdir> --wal <dst.wal>"
               " [--col <dstdir>]\n"
               "Limitation: offline, single-shard, kein PITR (s. backup.h).\n",
               prog, prog, prog);
}

int SelfCheck() {
  try {
    const std::string base =
        "/tmp/dbbackup_selfcheck_" + std::to_string((long long)::getpid());
    const std::string src_wal = base + ".src.wal";
    const std::string backup_dir = base + ".backup";
    const std::string restored_wal = base + ".restored.wal";
    std::error_code ec;
    std::filesystem::remove_all(backup_dir, ec);
    ::unlink(src_wal.c_str());
    ::unlink(restored_wal.c_str());

    // Mini-Stores befuellen.
    dbengine::storage::Wal wal(src_wal);
    wal.open();
    wal.append("put k1 v1");
    wal.append("put k2 v2");
    wal.append("put k3 v3");
    wal.flush();
    const std::uint64_t src_lsn = wal.durable_lsn();

    dbengine::columnar::ColumnarStore col;
    col.Append(1, "a");
    col.Append(2, "b");
    col.Append(3, "c");
    col.Append(4, "d");
    col.SealActive();
    const std::size_t src_rows = col.TotalRows();

    dbengine::raft::RaftGroup raft(0, "", "");
    if (raft.electLeader() < 0) {
      std::fprintf(stderr, "dbbackup selfcheck: FAIL no leader\n");
      return 1;
    }
    raft.append("put rk1 rv1");
    raft.append("put rk2 rv2");
    const std::uint64_t src_commit = raft.commitIndex();
    const auto src_rk1 = raft.get("rk1");
    const auto src_rk2 = raft.get("rk2");

    // Backup (Objekt-API).
    std::string err;
    if (!BackupCoordinator::Backup(&wal, &col, &raft, src_wal, backup_dir,
                                   &err)) {
      std::fprintf(stderr, "dbbackup selfcheck: FAIL backup: %s\n",
                   err.c_str());
      return 1;
    }
    wal.close();

    // Restore in frische Objekte + zweites WAL-Ziel.
    dbengine::columnar::ColumnarStore col2;
    dbengine::raft::RaftGroup raft2(0, "", "");
    std::size_t tail = 0;
    if (!BackupCoordinator::Restore(backup_dir, &col2, &raft2, restored_wal,
                                    &err, &tail)) {
      std::fprintf(stderr, "dbbackup selfcheck: FAIL restore: %s\n",
                   err.c_str());
      return 1;
    }

    bool ok = true;
    if (col2.TotalRows() != src_rows) {
      std::fprintf(stderr, "dbbackup selfcheck: FAIL rows %zu != %zu\n",
                   col2.TotalRows(), src_rows);
      ok = false;
    }
    if (raft2.get("rk1") != src_rk1 || raft2.get("rk2") != src_rk2) {
      std::fprintf(stderr, "dbbackup selfcheck: FAIL raft keys\n");
      ok = false;
    }
    auto recs = dbengine::storage::Wal::replay_file(restored_wal);
    std::uint64_t mx = 0;
    for (const auto& r : recs) mx = r.lsn > mx ? r.lsn : mx;
    if (mx != src_lsn) {
      std::fprintf(stderr, "dbbackup selfcheck: FAIL wal lsn %llu != %llu\n",
                   (unsigned long long)mx, (unsigned long long)src_lsn);
      ok = false;
    }
    if (raft2.commitIndex() != src_commit) {
      std::fprintf(stderr, "dbbackup selfcheck: FAIL commit %llu != %llu\n",
                   (unsigned long long)raft2.commitIndex(),
                   (unsigned long long)src_commit);
      ok = false;
    }
    if (tail != 0) {
      std::fprintf(stderr, "dbbackup selfcheck: FAIL tail %zu != 0\n", tail);
      ok = false;
    }
    // Aufraeumen (best effort).
    std::filesystem::remove_all(backup_dir, ec);
    ::unlink(src_wal.c_str());
    ::unlink(restored_wal.c_str());
    if (!ok) return 1;
    std::printf("OK\n");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "dbbackup selfcheck: FAIL %s\n", e.what());
    return 1;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage(argv[0]);
    return 2;
  }
  std::string mode = argv[1];
  if (mode == "--selfcheck") {
    if (argc != 2) {
      Usage(argv[0]);
      return 2;
    }
    return SelfCheck();
  }
  if (mode == "--backup") {
    std::string wal, out, col, raft_log, raft_snap;
    for (int i = 2; i < argc; ++i) {
      std::string a = argv[i];
      auto need = [&](std::string& dst) -> bool {
        if (i + 1 >= argc) return false;
        dst = argv[++i];
        return true;
      };
      if (a == "--wal") {
        if (!need(wal)) {
          Usage(argv[0]);
          return 2;
        }
      } else if (a == "--out") {
        if (!need(out)) {
          Usage(argv[0]);
          return 2;
        }
      } else if (a == "--col") {
        if (!need(col)) {
          Usage(argv[0]);
          return 2;
        }
      } else if (a == "--raft-log") {
        if (!need(raft_log)) {
          Usage(argv[0]);
          return 2;
        }
      } else if (a == "--raft-snap") {
        if (!need(raft_snap)) {
          Usage(argv[0]);
          return 2;
        }
      } else {
        Usage(argv[0]);
        return 2;
      }
    }
    if (wal.empty() || out.empty()) {
      Usage(argv[0]);
      return 2;
    }
    std::string err;
    if (!BackupCoordinator::BackupFiles(wal, col, raft_log, raft_snap, out,
                                        &err)) {
      std::fprintf(stderr, "dbbackup: backup failed: %s\n", err.c_str());
      return 1;
    }
    std::printf("backup ok: %s\n", out.c_str());
    return 0;
  }
  if (mode == "--restore") {
    std::string in, wal, col;
    for (int i = 2; i < argc; ++i) {
      std::string a = argv[i];
      auto need = [&](std::string& dst) -> bool {
        if (i + 1 >= argc) return false;
        dst = argv[++i];
        return true;
      };
      if (a == "--in") {
        if (!need(in)) {
          Usage(argv[0]);
          return 2;
        }
      } else if (a == "--wal") {
        if (!need(wal)) {
          Usage(argv[0]);
          return 2;
        }
      } else if (a == "--col") {
        if (!need(col)) {
          Usage(argv[0]);
          return 2;
        }
      } else {
        Usage(argv[0]);
        return 2;
      }
    }
    if (in.empty() || wal.empty()) {
      Usage(argv[0]);
      return 2;
    }
    std::string err;
    std::size_t tail = 0;
    if (!BackupCoordinator::RestoreFiles(in, wal, col, &err, &tail)) {
      std::fprintf(stderr, "dbbackup: restore failed: %s\n", err.c_str());
      return 1;
    }
    std::printf("restore ok: %s (tail=%zu)\n", wal.c_str(), tail);
    return 0;
  }
  Usage(argv[0]);
  return 2;
}
