// s118: deterministischer stateful Fuzzer (Modell-Vergleich + Crash-Reopens).
// Kein libFuzzer (keine Dep, STL-only): mt19937 mit Seed (--seed N, Default
// 42), Op-Mix ueber BTreeKV + WAL gegen std::map-/Log-Modell. Findet die
// Fehlerklasse aus dem adversarialen Review (gueltige-aber-boesartige
// Strukturen, Teilmutation, Lifetime), nicht nur Crash/Torn-Pfade.
// Aufruf: test_fuzz [--seed N]. CTest-Name: fuzz.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "dbengine/kv.h"
#include "dbengine/kv/btree.h"
#include "dbengine/storage/wal.h"

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

int main(int argc, char** argv) {
  unsigned seed = 42;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--seed") seed = static_cast<unsigned>(std::stoul(argv[i + 1]));
  }
  std::cout << "[fuzz] seed=" << seed << "\n";
  Check(FuzzBTree(seed), "fuzz-btree-model");
  Check(FuzzWal(seed), "fuzz-wal-model");
  if (g_fail == 0) {
    std::cout << "FUZZ TESTS PASSED (seed " << seed << ")\n";
    return 0;
  }
  std::cout << "FUZZ TESTS FAILED (" << g_fail << ")\n";
  return 1;
}
