// s118: deterministischer stateful Fuzzer (Modell-Vergleich + Crash-Reopens).
// Kein libFuzzer (keine Dep, STL-only): mt19937 mit Seed (--seed N, Default
// 42), Op-Mix ueber BTreeKV + WAL gegen std::map-/Log-Modell. Findet die
// Fehlerklasse aus dem adversarialen Review (gueltige-aber-boesartige
// Strukturen, Teilmutation, Lifetime), nicht nur Crash/Torn-Pfade.
// Aufruf: test_fuzz [--seed N]. CTest-Name: fuzz.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "dbengine/kv.h"
#include "dbengine/kv/btree.h"
#include "dbengine/sql/executor.h"
#include "dbengine/storage/wal.h"
#include "dbengine/txn/mvcc.h"

using dbengine::kv::BTreeKV;
using dbengine::kv::Op;
using dbengine::kv::WriteBatch;
using dbengine::storage::Wal;
using dbengine::storage::WalRecord;

namespace {

int g_fail = 0;
void Check(bool c, const std::string& n) {
  if (c) {
    std::cout << "PASS " << n << "\n";
  } else {
    std::cout << "FAIL " << n << "\n";
    ++g_fail;
  }
}

std::string Tmp(const std::string& n, unsigned seed) {
  auto p = std::filesystem::temp_directory_path() /
           ("fuzz_" + n + "_" + std::to_string(seed) + ".db");
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(std::string(p.string()) + ".tmp", ec);
  return p.string();
}

// ---- BTree vs. std::map-Modell --------------------------------------------
bool FuzzBTree(unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> key_d(0, 199);
  std::uniform_int_distribution<int> op_d(0, 99);
  std::uniform_int_distribution<int> len_d(0, 64);
  std::uniform_int_distribution<int> batch_d(1, 5);
  const std::string path = Tmp("btree", seed);
  std::map<std::string, std::string> model;
  BTreeKV db;
  if (!db.Open(path)) {
    std::cout << "FAIL fuzz-btree/open\n";
    return false;
  }
  auto key = [&] {
    char b[16];
    std::snprintf(b, sizeof b, "k:%04d", key_d(rng));
    return std::string(b);
  };
  auto val = [&] {
    int n = len_d(rng);
    if (n == 0) return std::string{};
    std::string v;
    v.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) v.push_back(char('a' + (rng() % 26)));
    return v;
  };
  auto full_check = [&](const char* ctx) -> bool {
    auto got = db.Scan("", 1000000);
    if (got.size() != model.size()) {
      std::cout << "FAIL fuzz-btree/size@" << ctx << " got=" << got.size()
                << " want=" << model.size() << "\n";
      return false;
    }
    size_t i = 0;
    for (auto& [k, v] : model) {
      if (got[i].first != k || got[i].second != v) {
        std::cout << "FAIL fuzz-btree/content@" << ctx << " idx=" << i << "\n";
        return false;
      }
      ++i;
    }
    return true;
  };
  constexpr int kOps = 10000;
  for (int step = 0; step < kOps; ++step) {
    const int r = op_d(rng);
    if (r < 40) {
      const std::string k = key(), v = val();
      if (!db.Put(k, v)) {
        std::cout << "FAIL fuzz-btree/put step=" << step << "\n";
        return false;
      }
      model[k] = v;
    } else if (r < 55) {
      const std::string k = key();
      db.Delete(k);  // delete-missing ist ok (idempotent)
      model.erase(k);
    } else if (r < 75) {
      // Batch, manchmal mit invalidem Op (leerer Key / oversize).
      std::vector<Op> ops;
      std::map<std::string, std::string> shadow = model;
      const int nb = batch_d(rng);
      bool want_ok = true;
      for (int i = 0; i < nb; ++i) {
        const int kind = rng() % 10;
        if (kind == 0) {
          ops.push_back(Op::PutOp("", "bad-empty-key"));
          want_ok = false;
        } else if (kind == 1) {
          ops.push_back(Op::PutOp(key(), std::string(BTreeKV::MaxEntryBytes() + 1, 'z')));
          want_ok = false;
        } else if (kind < 5) {
          const std::string k = key();
          ops.push_back(Op::DeleteOp(k));
          shadow.erase(k);
        } else {
          const std::string k = key(), v = val();
          ops.push_back(Op::PutOp(k, v));
          shadow[k] = v;
        }
      }
      const bool ok = db.Write(ops);
      if (ok != want_ok) {
        std::cout << "FAIL fuzz-btree/batch-verdict step=" << step << "\n";
        return false;
      }
      if (want_ok) model = std::move(shadow);
      // Bei !ok: Modell unveraendert (atomar) — Full-Check unten prueft es.
    } else if (r < 85) {
      // Stichproben-Gets.
      for (int i = 0; i < 20; ++i) {
        const std::string k = key();
        auto got = db.Get(k);
        auto it = model.find(k);
        const bool same = (it == model.end()) ? !got.has_value()
                                              : (got.has_value() && *got == it->second);
        if (!same) {
          std::cout << "FAIL fuzz-btree/get step=" << step << " key=" << k << "\n";
          return false;
        }
      }
    } else if (r < 90) {
      if (!db.Flush()) {
        std::cout << "FAIL fuzz-btree/flush step=" << step << "\n";
        return false;
      }
    } else if (r < 93) {
      // Crash-Reopen: alles durable (Put puffert RAM — vorher flushen!).
      if (!db.Flush()) {
        std::cout << "FAIL fuzz-btree/pre-reopen-flush step=" << step << "\n";
        return false;
      }
      db.Close();
      if (!db.Open(path)) {
        std::cout << "FAIL fuzz-btree/reopen step=" << step << "\n";
        return false;
      }
    } else {
      // Range-Spot-Check gegen Modell.
      const std::string lo = key(), hi = key();
      const std::string from = std::min(lo, hi), to = std::max(lo, hi);
      auto got = db.ScanRange(from, to, 100000);
      std::vector<std::pair<std::string, std::string>> want;
      for (auto& [k, v] : model) {
        if (k < from) continue;
        if (!(k < to)) break;
        want.emplace_back(k, v);
      }
      if (got != want) {
        std::cout << "FAIL fuzz-btree/range step=" << step << "\n";
        return false;
      }
    }
    if (step % 500 == 499 && !full_check("periodic")) return false;
  }
  if (!db.Flush()) {
    std::cout << "FAIL fuzz-btree/final-flush\n";
    return false;
  }
  db.Close();
  if (!db.Open(path)) {
    std::cout << "FAIL fuzz-btree/final-reopen\n";
    return false;
  }
  // Finaler Full-Check nach Reopen.
  {
    auto got = db.Scan("", 1000000);
    if (got.size() != model.size()) {
      std::cout << "FAIL fuzz-btree/final-size\n";
      return false;
    }
    size_t i = 0;
    for (auto& [k, v] : model) {
      if (got[i].first != k || got[i].second != v) {
        std::cout << "FAIL fuzz-btree/final-content\n";
        return false;
      }
      ++i;
    }
  }
  db.Close();
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path + ".tmp", ec);
  return true;
}

// ---- WAL vs. Log-Modell (deckt s116-Index + Checkpoint mit ab) -------------
bool FuzzWal(unsigned seed) {
  std::mt19937 rng(seed + 1000);
  std::uniform_int_distribution<int> op_d(0, 99);
  std::uniform_int_distribution<int> len_d(0, 48);
  auto p = std::filesystem::temp_directory_path() /
           ("fuzz_wal_" + std::to_string(seed) + ".wal");
  const std::string path = p.string();
  std::error_code ec;
  std::filesystem::remove(p, ec);
  // Modell: (lsn, payload) in Ordnung; checkpoint verwirft Prefix.
  std::vector<std::pair<uint64_t, std::string>> log;
  uint64_t next = 1;
  Wal w(path);
  w.open();
  constexpr int kOps = 10000;
  for (int step = 0; step < kOps; ++step) {
    const int r = op_d(rng);
    if (r < 70) {
      const int n = len_d(rng);
      std::string v;
      for (int i = 0; i < n; ++i) v.push_back(char('0' + (rng() % 10)));
      const uint64_t lsn = w.append(v);
      if (lsn != next) {
        std::cout << "FAIL fuzz-wal/lsn-seq step=" << step << " got=" << lsn
                  << " want=" << next << " logn=" << log.size()
                  << " replayn=" << w.replay().size() << "\n";
        return false;
      }
      log.emplace_back(lsn, v);
      ++next;
    } else if (r < 80) {
      w.flush();
      auto got = w.replay();
      if (got.size() != log.size()) {
        std::cout << "FAIL fuzz-wal/replay-size step=" << step << "\n";
        return false;
      }
      for (size_t i = 0; i < got.size(); ++i) {
        if (got[i].lsn != log[i].first || got[i].data != log[i].second) {
          std::cout << "FAIL fuzz-wal/replay-content step=" << step << "\n";
          return false;
        }
      }
    } else if (r < 88) {
      // read_from-Spot-Check gegen Modell (s116-Index-Pfad!).
      const uint64_t from = 1 + (rng() % (next + 2));
      const size_t mx = static_cast<size_t>(rng() % 20);
      auto got = w.read_from(from, mx);
      std::vector<std::pair<uint64_t, std::string>> want;
      for (auto& e : log) {
        if (e.first < from) continue;
        want.push_back(e);
        if (mx && want.size() >= mx) break;
      }
      if (got.size() != want.size()) {
        std::cout << "FAIL fuzz-wal/cdc-size step=" << step << "\n";
        return false;
      }
      for (size_t i = 0; i < got.size(); ++i) {
        if (got[i].lsn != want[i].first || got[i].data != want[i].second) {
          std::cout << "FAIL fuzz-wal/cdc-content step=" << step << "\n";
          return false;
        }
      }
    } else if (r < 94) {
      // Checkpoint auf zufaellige gehaltene LSN (oder 0 = noop-nah).
      uint64_t cp = 0;
      if (!log.empty() && (rng() % 2)) cp = log[rng() % log.size()].first;
      w.checkpoint(cp);
      std::vector<std::pair<uint64_t, std::string>> tail;
      for (auto& e : log)
        if (e.first > cp) tail.push_back(e);
      log = std::move(tail);
      // Leere Datei -> LSNs beginnen neu bei 1 (Spezifikation, s. auch
      // Full-Checkpoint-Test). Modell-next entsprechend zuruecksetzen.
      next = log.empty() ? 1 : log.back().first + 1;
      auto got = w.replay();
      if (got.size() != log.size()) {
        std::cout << "FAIL fuzz-wal/checkpoint-size step=" << step << "\n";
        return false;
      }
      for (size_t i = 0; i < got.size(); ++i) {
        if (got[i].lsn != log[i].first || got[i].data != log[i].second) {
          std::cout << "FAIL fuzz-wal/checkpoint-content step=" << step << "\n";
          return false;
        }
      }
    } else {
      // Reopen-Crash: unflushed geht verloren — Modell auf durable kappen.
      w.flush();
      w.close();
      w.open();
      auto got = w.replay();
      if (got.size() != log.size()) {
        std::cout << "FAIL fuzz-wal/reopen-size step=" << step << "\n";
        return false;
      }
      for (size_t i = 0; i < got.size(); ++i) {
        if (got[i].lsn != log[i].first || got[i].data != log[i].second) {
          std::cout << "FAIL fuzz-wal/reopen-content step=" << step << "\n";
          return false;
        }
      }
    }
  }
  w.close();
  std::filesystem::remove(p, ec);
  return true;
}

}  // namespace

// ---- MVCC vs. Versions-Modell (Snapshot-Isolation + Purge-Schutz) ---------
// Modell: pro Key Historie (commit#, wert|nullopt=Tombstone), globale
// Commit-Sequenz. Reader mit Sequenz S sieht je Key den letzten Eintrag mit
// commit# <= S. Purge darf aktive Snapshots nie brechen (danach ALLE aktiven
// Reader re-verifizieren = Kern des s112-Lifetime-Themas).
bool FuzzMvcc(unsigned seed) {
  using dbengine::txn::MvccStore;
  using dbengine::txn::Transaction;
  std::mt19937 rng(seed + 2000);
  std::uniform_int_distribution<int> key_d(0, 29);
  std::uniform_int_distribution<int> op_d(0, 99);
  MvccStore s;
  // Historie: key -> [(commit#, wert)].
  std::map<std::string, std::vector<std::pair<uint64_t, std::optional<std::string>>>> hist;
  uint64_t seq = 0;
  std::map<int, Transaction> readers;  // move-only: kein Kopieren
  int next_rid = 1;
  std::optional<Transaction> writer;
  std::map<std::string, std::optional<std::string>> pending;  // Writer-Puffer-Modell
  auto key = [&] {
    char b[16];
    std::snprintf(b, sizeof b, "m:%02d", key_d(rng));
    return std::string(b);
  };
  auto expect = [&](uint64_t snap, const std::string& k) -> std::optional<std::string> {
    auto it = hist.find(k);
    if (it == hist.end()) return std::nullopt;
    std::optional<std::string> out;
    bool any = false;
    for (auto& [c, v] : it->second) {
      if (c > snap) break;
      out = v;
      any = true;
    }
    return any ? out : std::optional<std::string>{};
  };
  // Reader-Snapshots merken (Sequenz bei Begin).
  std::map<int, uint64_t> rsnap;
  auto verify_reader = [&](int rid) -> bool {
    auto it = readers.find(rid);
    if (it == readers.end()) return false;
    for (int i = 0; i < 5; ++i) {
      const std::string k = key();
      auto got = s.Read(it->second, k);
      if (got != expect(rsnap[rid], k)) {
        std::cout << "FAIL fuzz-mvcc/read rid=" << rid << " key=" << k << "\n";
        return false;
      }
    }
    return true;
  };
  constexpr int kOps = 7000;
  for (int step = 0; step < kOps; ++step) {
    const int r = op_d(rng);
    if (r < 25) {
      // Writer beginnen (Single-Writer: zweiter muss scheitern).
      if (writer.has_value()) {
        auto dup = s.TryBeginWrite();
        if (dup.has_value()) {
          std::cout << "FAIL fuzz-mvcc/single-writer step=" << step << "\n";
          return false;
        }
        continue;
      }
      auto w = s.TryBeginWrite();
      if (!w.has_value()) {
        std::cout << "FAIL fuzz-mvcc/begin-write step=" << step << "\n";
        return false;
      }
      writer.emplace(std::move(*w));
      pending.clear();
    } else if (r < 50) {
      if (!writer.has_value()) continue;
      const std::string k = key();
      if (rng() % 5 == 0) {
        if (!s.Erase(*writer, k)) {
          std::cout << "FAIL fuzz-mvcc/erase step=" << step << "\n";
          return false;
        }
        pending[k] = std::nullopt;
      } else {
        const std::string v = "w" + std::to_string(step) + "_" + std::to_string(rng() % 100);
        if (!s.Write(*writer, k, v)) {
          std::cout << "FAIL fuzz-mvcc/write step=" << step << "\n";
          return false;
        }
        pending[k] = v;
      }
    } else if (r < 60) {
      // Commit oder Abort des Writers.
      if (!writer.has_value()) continue;
      if (rng() % 4 == 0) {
        s.Abort(*writer);
        writer.reset();
        pending.clear();
      } else {
        if (!s.Commit(*writer)) {
          std::cout << "FAIL fuzz-mvcc/commit step=" << step << "\n";
          return false;
        }
        writer.reset();
        ++seq;
        for (auto& [k, v] : pending) hist[k].emplace_back(seq, v);
        pending.clear();
      }
    } else if (r < 75) {
      // Reader beginnen (max 4 aktiv).
      if (readers.size() >= 4) continue;
      Transaction t = s.BeginRead();
      const int rid = next_rid++;
      rsnap[rid] = seq;
      readers.emplace(rid, std::move(t));
      if (!verify_reader(rid)) return false;
    } else if (r < 85) {
      // Zufälligen Reader lesen + manchmal committen.
      if (readers.empty()) continue;
      auto it = readers.begin();
      std::advance(it, static_cast<long>(rng() % readers.size()));
      if (!verify_reader(it->first)) return false;
      if (rng() % 3 == 0) {
        if (!s.Commit(it->second)) {
          std::cout << "FAIL fuzz-mvcc/reader-commit step=" << step << "\n";
          return false;
        }
        rsnap.erase(it->first);
        readers.erase(it);
      }
    } else {
      // Purge — danach ALLE aktiven Reader re-verifizieren (Snapshot-Schutz).
      s.Purge();
      for (auto& [rid, txn] : readers) {
        (void)txn;
        if (!verify_reader(rid)) {
          std::cout << "FAIL fuzz-mvcc/post-purge rid=" << rid << " step=" << step << "\n";
          return false;
        }
      }
    }
  }
  // Aufraeumen: Writer aborten, Reader committen, finaler Purge + Neu-Reader.
  if (writer.has_value()) {
    s.Abort(*writer);
    writer.reset();
  }
  for (auto& [rid, txn] : readers) {
    if (!s.Commit(txn)) {
      std::cout << "FAIL fuzz-mvcc/final-reader-commit\n";
      return false;
    }
  }
  readers.clear();
  s.Purge();
  {
    Transaction t = s.BeginRead();
    for (int i = 0; i < 30; ++i) {
      const std::string k = key();
      if (s.Read(t, k) != expect(seq, k)) {
        std::cout << "FAIL fuzz-mvcc/final-read\n";
        return false;
      }
    }
    if (!s.Commit(t)) {
      std::cout << "FAIL fuzz-mvcc/final-commit\n";
      return false;
    }
  }
  return true;
}

// ---- Executor-SQL vs. Tabellen-Modell (DML/DDL/Aggregate) -----------------
// Modell: map<id,val> (INT only, keine NULLs). WHERE-Templates nur ueber id,
// Erwartung in C++ nachgerechnet, SELECTs mit ORDER BY id (deterministisch).
// Frische IDs aus monotonem Zaehler (keine PK-Duplikate). DROP/CREATE-Zyklen,
// VACUUM (kein Modell-Effekt), gelegentlich invalides SQL (muss werfen,
// Modell + Engine danach weiter nutzbar).
bool FuzzSql(unsigned seed) {
  using dbengine::sql::Executor;
  using dbengine::sql::Result;
  using dbengine::sql::Value;
  std::mt19937 rng(seed + 3000);
  std::uniform_int_distribution<int> op_d(0, 99);
  std::uniform_int_distribution<int> val_d(-1000, 1000);
  dbengine::kv::KVStore kv;
  dbengine::txn::MvccStore mvcc;
  Executor ex(kv, mvcc, nullptr);
  std::map<int64_t, int64_t> tab;
  bool exists = false;
  int64_t next_id = 1;
  auto exec_ok = [&](const std::string& q, Result* out = nullptr) -> bool {
    try {
      Result r = ex.execute(q);
      if (out) *out = std::move(r);
      return true;
    } catch (...) {
      return false;
    }
  };
  // WHERE-Generator: gibt (sql, praedikat) zurueck.
  auto gen_where = [&](std::function<bool(int64_t)>* pred) -> std::string {
    const int t = rng() % 6;
    const int64_t a = val_d(rng) % 200, b = val_d(rng) % 200;
    const int64_t lo = std::min(a, b), hi = std::max(a, b);
    switch (t) {
      case 0: *pred = [a](int64_t id) { return id == a; };
        return "id = " + std::to_string(a);
      case 1: *pred = [lo, hi](int64_t id) { return id >= lo && id <= hi; };
        return "id BETWEEN " + std::to_string(lo) + " AND " + std::to_string(hi);
      case 2: {
        int64_t x = val_d(rng) % 200, y = val_d(rng) % 200, z = val_d(rng) % 200;
        *pred = [x, y, z](int64_t id) { return id == x || id == y || id == z; };
        return "id IN (" + std::to_string(x) + "," + std::to_string(y) + "," +
               std::to_string(z) + ")";
      }
      case 3: *pred = [a](int64_t id) { return id < a; };
        return "id < " + std::to_string(a);
      case 4: *pred = [a](int64_t id) { return id >= a; };
        return "id >= " + std::to_string(a);
      default: *pred = [a](int64_t id) { return id != a; };
        return "id <> " + std::to_string(a);
    }
  };
  if (!exec_ok("CREATE TABLE t (id INT, val INT)")) {
    std::cout << "FAIL fuzz-sql/create\n";
    return false;
  }
  exists = true;
  constexpr int kOps = 3000;
  for (int step = 0; step < kOps; ++step) {
    const int r = op_d(rng);
    if (!exists) {
      // Tabelle weg: nur CREATE ist legal; alles andere muss werfen.
      if (r < 70) {
        if (!exec_ok("CREATE TABLE t (id INT, val INT)")) {
          std::cout << "FAIL fuzz-sql/recreate step=" << step << "\n";
          return false;
        }
        exists = true;
        tab.clear();
      } else {
        Result tmp;
        if (exec_ok("SELECT * FROM t", &tmp)) {
          std::cout << "FAIL fuzz-sql/select-on-missing step=" << step << "\n";
          return false;
        }
      }
      continue;
    }
    if (r < 35) {
      // Multi-INSERT mit frischen IDs.
      const int n = 1 + rng() % 5;
      std::string q = "INSERT INTO t VALUES ";
      std::vector<std::pair<int64_t, int64_t>> rows;
      for (int i = 0; i < n; ++i) {
        const int64_t id = next_id++, v = val_d(rng);
        rows.emplace_back(id, v);
        q += (i ? "," : "") + std::string("(") + std::to_string(id) + "," +
             std::to_string(v) + ")";
      }
      if (!exec_ok(q)) {
        std::cout << "FAIL fuzz-sql/insert step=" << step << " q=" << q << "\n";
        return false;
      }
      for (auto& [id, v] : rows) tab[id] = v;
    } else if (r < 50) {
      std::function<bool(int64_t)> pred;
      const std::string w = gen_where(&pred);
      const int64_t nv = val_d(rng);
      if (!exec_ok("UPDATE t SET val = " + std::to_string(nv) + " WHERE " + w)) {
        std::cout << "FAIL fuzz-sql/update step=" << step << "\n";
        return false;
      }
      for (auto& [id, v] : tab)
        if (pred(id)) v = nv;
    } else if (r < 60) {
      std::function<bool(int64_t)> pred;
      const std::string w = gen_where(&pred);
      if (!exec_ok("DELETE FROM t WHERE " + w)) {
        std::cout << "FAIL fuzz-sql/delete step=" << step << "\n";
        return false;
      }
      for (auto it = tab.begin(); it != tab.end();) {
        if (pred(it->first))
          it = tab.erase(it);
        else
          ++it;
      }
    } else if (r < 80) {
      // SELECT * mit/ohne WHERE, ORDER BY id, exakter Zeilenvergleich.
      std::function<bool(int64_t)> pred;
      const bool filtered = (rng() % 3 != 0);
      const std::string w = filtered ? gen_where(&pred) : "";
      Result got;
      if (!exec_ok("SELECT id, val FROM t" + (filtered ? " WHERE " + w : "") +
                       " ORDER BY id",
                   &got)) {
        std::cout << "FAIL fuzz-sql/select step=" << step << "\n";
        return false;
      }
      std::vector<std::pair<int64_t, int64_t>> want;
      for (auto& [id, v] : tab) {
        if (!filtered || pred(id)) want.emplace_back(id, v);
      }
      if (got.rows.size() != want.size()) {
        std::cout << "FAIL fuzz-sql/select-size step=" << step << " got="
                  << got.rows.size() << " want=" << want.size() << "\n";
        return false;
      }
      for (size_t i = 0; i < want.size(); ++i) {
        auto* gi = std::get_if<int64_t>(&got.rows[i][0]);
        auto* gv = std::get_if<int64_t>(&got.rows[i][1]);
        if (!gi || !gv || *gi != want[i].first || *gv != want[i].second) {
          std::cout << "FAIL fuzz-sql/select-content step=" << step << " row=" << i
                    << "\n";
          return false;
        }
      }
    } else if (r < 90) {
      // Aggregate gegen Modell (SUM NULL bei leer).
      std::function<bool(int64_t)> pred;
      const bool filtered = (rng() % 2 == 0);
      const std::string w = filtered ? gen_where(&pred) : "";
      Result got;
      if (!exec_ok("SELECT SUM(val), COUNT(*) FROM t" +
                       (filtered ? " WHERE " + w : ""),
                   &got)) {
        std::cout << "FAIL fuzz-sql/agg step=" << step << "\n";
        return false;
      }
      int64_t n = 0;
      int64_t sum = 0;
      for (auto& [id, v] : tab) {
        if (!filtered || pred(id)) {
          ++n;
          sum += v;
        }
      }
      if (got.rows.size() != 1 || got.rows[0].size() != 2) {
        std::cout << "FAIL fuzz-sql/agg-shape step=" << step << "\n";
        return false;
      }
      const Value& s0 = got.rows[0][0];
      const Value& s1 = got.rows[0][1];
      auto* cn = std::get_if<int64_t>(&s1);
      if (!cn || *cn != n) {
        std::cout << "FAIL fuzz-sql/agg-count step=" << step << "\n";
        return false;
      }
      if (n == 0) {
        if (!dbengine::sql::valueIsNull(s0)) {
          std::cout << "FAIL fuzz-sql/agg-sum-null step=" << step << "\n";
          return false;
        }
      } else {
        int64_t got_sum = 0;
        if (auto* sv = std::get_if<int64_t>(&s0)) {
          got_sum = *sv;
        } else if (auto* dv = std::get_if<double>(&s0)) {
          got_sum = static_cast<int64_t>(std::llround(*dv));
        } else {
          std::cout << "FAIL fuzz-sql/agg-sum-type step=" << step << "\n";
          return false;
        }
        if (got_sum != sum) {
          std::cout << "FAIL fuzz-sql/agg-sum step=" << step << " got=" << got_sum
                    << " want=" << sum << "\n";
          return false;
        }
      }
    } else if (r < 93) {
      if (!exec_ok("DROP TABLE t")) {
        std::cout << "FAIL fuzz-sql/drop step=" << step << "\n";
        return false;
      }
      exists = false;
      tab.clear();
    } else if (r < 96) {
      if (!exec_ok("VACUUM")) {
        std::cout << "FAIL fuzz-sql/vacuum step=" << step << "\n";
        return false;
      }
    } else {
      // Invalides SQL muss werfen; Engine danach weiter nutzbar.
      if (exec_ok("SELEC * FRM t WHER")) {
        std::cout << "FAIL fuzz-sql/invalid-accepted step=" << step << "\n";
        return false;
      }
      Result probe;
      const bool want_ok = exists;
      if (exec_ok("SELECT COUNT(*) FROM t", &probe) != want_ok) {
        std::cout << "FAIL fuzz-sql/post-error-usable step=" << step << "\n";
        return false;
      }
    }
  }
  return true;
}

int main(int argc, char** argv) {
  unsigned seed = 42;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--seed") seed = static_cast<unsigned>(std::stoul(argv[i + 1]));
  }
  std::cout << "[fuzz] seed=" << seed << "\n";
  Check(FuzzBTree(seed), "fuzz-btree-model");
  Check(FuzzWal(seed), "fuzz-wal-model");
  Check(FuzzMvcc(seed), "fuzz-mvcc-model");
  Check(FuzzSql(seed), "fuzz-sql-model");
  if (g_fail == 0) {
    std::cout << "FUZZ TESTS PASSED (seed " << seed << ")\n";
    return 0;
  }
  std::cout << "FUZZ TESTS FAILED (" << g_fail << ")\n";
  return 1;
}
