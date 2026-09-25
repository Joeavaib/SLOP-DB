// s19-chaos: deterministischer Crash-Fuzz (Seed=42), 10k random Ops.
// Modell: KVStore + WAL-REDO + std::map-Erwartung.
//   - 10k Ops: put (60%) / del (25%) / scan (15%), Keyraum 1000 Keys.
//   - Jede Mutation: expected + kv + wal.append; flush alle 256 Ops + final.
//   - Mitte (nach Op 5000): WAL-torn-inject via Anhang + truncate(S+3),
//     danach reopen -> torn tail muss gekappt sein, Replay == Prefix (kein Verlust).
//   - Ende: wal.flush + close + replay_file; decode -> map_replayed.
//     Nachweis: replay.size == mutationen, map_replayed == expected == kv.Scan.
//     Verlust = expected.size - replayed.size-Abweichung -> muss 0 sein.
// Framework-los (assert/cout), CTest-Name: chaos (ctest -R chaos).
// ASAN/UBSAN: nur dieses Target mit -fsanitize=address,undefined (siehe CMake).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>

#include "dbengine/kv.h"
#include "dbengine/storage/wal.h"

using dbengine::kv::KVStore;
using dbengine::storage::Wal;
using dbengine::storage::WalRecord;

namespace {

constexpr int kSeed = 42;
constexpr int kTotalOps = 10000;
constexpr int kKeySpace = 1000;
constexpr int kFlushEvery = 256;
constexpr int kTornAfterOp = 5000;

int g_failures = 0;

void Check(bool cond, const char* name) {
  if (cond) {
    std::cout << "PASS chaos/" << name << "\n";
  } else {
    std::cout << "FAIL chaos/" << name << "\n";
    ++g_failures;
  }
}

// Zerlege Payload: "P\x1fkey\x1fvalue" | "D\x1fkey". Rueckgabe false bei Formatfehler.
bool Decode(const std::string& p, bool& is_put, std::string& key, std::string& val) {
  if (p.size() < 3 || p[1] != '\x1f') return false;
  if (p[0] == 'P') {
    auto sep = p.find('\x1f', 2);
    if (sep == std::string::npos) return false;
    is_put = true;
    key = p.substr(2, sep - 2);
    val = p.substr(sep + 1);
    return !key.empty();
  }
  if (p[0] == 'D') {
    if (p.find('\x1f', 2) != std::string::npos) return false;
    is_put = false;
    key = p.substr(2);
    val.clear();
    return !key.empty();
  }
  return false;
}

std::string EncodePut(const std::string& k, const std::string& v) {
  std::string s;
  s.reserve(2 + k.size() + 1 + v.size());
  s.push_back('P');
  s.push_back('\x1f');
  s += k;
  s.push_back('\x1f');
  s += v;
  return s;
}

std::string EncodeDel(const std::string& k) {
  std::string s;
  s.reserve(2 + k.size());
  s.push_back('D');
  s.push_back('\x1f');
  s += k;
  return s;
}

std::string RandValue(std::mt19937& rng) {
  static const char kAlpha[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::uniform_int_distribution<int> len_dist(1, 32);
  std::uniform_int_distribution<int> ch_dist(0, static_cast<int>(sizeof(kAlpha) - 2));
  int n = len_dist(rng);
  std::string v;
  v.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) v.push_back(kAlpha[ch_dist(rng)]);
  return v;
}

std::string KeyFor(int id) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "k%04d", id);
  return std::string(buf);
}

// Erwarteter Prefix-Scan auf std::map (lexikographisch, wie KVStore::Scan).
std::vector<std::pair<std::string, std::string>> ExpectedScan(
    const std::map<std::string, std::string>& m, const std::string& prefix,
    std::size_t limit) {
  std::vector<std::pair<std::string, std::string>> out;
  if (limit == 0) return out;
  auto it = prefix.empty() ? m.begin() : m.lower_bound(prefix);
  for (; it != m.end(); ++it) {
    if (!prefix.empty() &&
        it->first.compare(0, prefix.size(), prefix) != 0)
      break;
    out.emplace_back(it->first, it->second);
    if (out.size() >= limit) break;
  }
  return out;
}

int64_t FileSize(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return -1;
  return static_cast<int64_t>(st.st_size);
}

}  // namespace

int main() {
  const std::string path =
      (std::filesystem::temp_directory_path() / "dbengine_test_chaos_s19.wal").string();
  std::error_code ec;
  std::filesystem::remove(path, ec);

  std::mt19937 rng(kSeed);
  std::uniform_int_distribution<int> op_dist(0, 99);
  std::uniform_int_distribution<int> key_dist(0, kKeySpace - 1);
  std::uniform_int_distribution<int> prefix_pick(0, 4);
  std::uniform_int_distribution<int> limit_dist(1, 50);

  KVStore kv;
  std::map<std::string, std::string> expected;
  Wal wal(path);
  wal.open();

  int n_put = 0, n_del = 0, n_scan = 0, n_flush = 0;
  int n_mut = 0;  // put+del (== WAL-Records)
  bool torn_done = false;
  int torn_replay_before = -1;
  int64_t torn_size_before = -1;

  static const char* kPrefixes[] = {"k", "k0", "k00", "", "k05"};

  for (int i = 0; i < kTotalOps; ++i) {
    int r = op_dist(rng);
    if (r < 60) {
      // PUT
      std::string k = KeyFor(key_dist(rng));
      std::string v = RandValue(rng);
      kv.Put(k, v);
      expected[k] = v;
      uint64_t lsn = wal.append(EncodePut(k, v));
      if (lsn != static_cast<uint64_t>(n_mut + 1)) {
        std::cout << "FAIL chaos/lsn-seq put i=" << i << " lsn=" << lsn
                  << " want=" << (n_mut + 1) << "\n";
        ++g_failures;
      }
      ++n_put;
      ++n_mut;
    } else if (r < 85) {
      // DELETE (idempotent, auch missing)
      std::string k = KeyFor(key_dist(rng));
      kv.Delete(k);
      expected.erase(k);
      uint64_t lsn = wal.append(EncodeDel(k));
      if (lsn != static_cast<uint64_t>(n_mut + 1)) {
        std::cout << "FAIL chaos/lsn-seq del i=" << i << " lsn=" << lsn
                  << " want=" << (n_mut + 1) << "\n";
        ++g_failures;
      }
      ++n_del;
      ++n_mut;
    } else {
      // SCAN: live KV vs. Erwartung (Prefix + Limit zufaellig)
      const char* pre = kPrefixes[prefix_pick(rng)];
      std::size_t lim = static_cast<std::size_t>(limit_dist(rng));
      auto got = kv.Scan(pre, lim);
      auto want = ExpectedScan(expected, pre, lim);
      if (got != want) {
        std::cout << "FAIL chaos/scan-mismatch i=" << i << " prefix='" << pre
                  << "' limit=" << lim << " got=" << got.size()
                  << " want=" << want.size() << "\n";
        ++g_failures;
      }
      ++n_scan;
    }

    if ((i + 1) % kFlushEvery == 0) {
      wal.flush();
      ++n_flush;
    }

    // --- WAL-torn-inject genau einmal nach Op 5000 (nach flush) ---
    if (!torn_done && (i + 1) == kTornAfterOp) {
      wal.flush();
      ++n_flush;
      wal.close();
      torn_size_before = FileSize(path);
      auto pre = Wal::replay_file(path);
      torn_replay_before = static_cast<int>(pre.size());
      // 1) Garbage anhaengen (simuliert abgerissenen Write).
      {
        int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
        if (fd < 0) {
          std::cout << "FAIL chaos/torn-open\n";
          ++g_failures;
        } else {
          const char garbage[7] = {'X', 'Y', 'Z', 0, 1, 2, 3};
          ssize_t w = ::write(fd, garbage, sizeof(garbage));
          if (w != static_cast<ssize_t>(sizeof(garbage))) {
            std::cout << "FAIL chaos/torn-write\n";
            ++g_failures;
          }
          ::close(fd);
        }
      }
      // 2) truncate(S+3): mitten in den torn tail schneiden (echtes truncate).
      {
        int64_t s = FileSize(path);
        if (s != torn_size_before + 7) {
          std::cout << "FAIL chaos/torn-size s=" << s
                    << " want=" << (torn_size_before + 7) << "\n";
          ++g_failures;
        }
        if (::truncate(path.c_str(), torn_size_before + 3) != 0) {
          std::cout << "FAIL chaos/torn-truncate errno=" << errno << "\n";
          ++g_failures;
        }
      }
      // 3) reopen: muss torn tail kappen (size wieder S, Replay == Prefix).
      wal.open();
      int64_t s_after = FileSize(path);
      auto post = Wal::replay_file(path);
      Check(s_after == torn_size_before, "torn/size-recapped");
      Check(static_cast<int>(post.size()) == torn_replay_before, "torn/replay-prefix");
      Check(static_cast<int>(post.size()) == n_mut, "torn/no-loss");
      Check(wal.next_lsn() == static_cast<uint64_t>(n_mut + 1), "torn/next-lsn");
      std::cout << "[chaos] torn-inject nach op " << (i + 1) << ": size=" << torn_size_before
                << " replay=" << torn_replay_before << " recapped=" << s_after << "\n";
      torn_done = true;
    }
  }

  Check(torn_done, "torn/executed");
  wal.flush();
  ++n_flush;
  wal.close();

  // --- End-Replay vs. Erwartung ---
  std::vector<WalRecord> recs = Wal::replay_file(path);
  Check(static_cast<int>(recs.size()) == n_mut, "replay/count-eq-mutations");
  bool lsn_ok = true;
  for (std::size_t i = 0; i < recs.size(); ++i) {
    if (recs[i].lsn != i + 1) {
      lsn_ok = false;
      break;
    }
  }
  Check(lsn_ok, "replay/lsn-mono");

  std::map<std::string, std::string> replayed;
  bool decode_ok = true;
  for (const auto& rec : recs) {
    bool is_put = false;
    std::string k, v;
    if (!Decode(rec.data, is_put, k, v)) {
      decode_ok = false;
      break;
    }
    if (is_put)
      replayed[k] = v;
    else
      replayed.erase(k);
  }
  Check(decode_ok, "replay/decode");

  // Verlust-Nachweis: Replay-Modell vs. Live-Erwartung + KV.
  std::size_t mismatches = 0;
  for (const auto& [k, v] : expected) {
    auto it = replayed.find(k);
    if (it == replayed.end() || it->second != v) ++mismatches;
  }
  for (const auto& [k, v] : replayed) {
    auto it = expected.find(k);
    if (it == expected.end() || it->second != v) ++mismatches;
  }
  Check(mismatches == 0, "replay/model-eq-expected");
  auto kv_all = kv.Scan("");
  bool kv_ok = kv_all.size() == expected.size();
  if (kv_ok) {
    for (const auto& [k, v] : kv_all) {
      auto it = expected.find(k);
      if (it == expected.end() || it->second != v) {
        kv_ok = false;
        break;
      }
    }
  }
  Check(kv_ok, "kv/eq-expected");

  std::cout << "[chaos] seed=" << kSeed << " ops=" << kTotalOps << " put=" << n_put
            << " del=" << n_del << " scan=" << n_scan << " mutations=" << n_mut
            << " flushes=" << n_flush << "\n";
  std::cout << "[chaos] replay=" << recs.size() << " expected_keys=" << expected.size()
            << " replayed_keys=" << replayed.size() << " mismatches=" << mismatches
            << " VERLUST=0\n";

  std::filesystem::remove(path, ec);

  if (g_failures == 0) {
    std::cout << "ALL CHAOS TESTS PASSED (10k ops, seed 42, torn-inject, Verlust=0)\n";
    return 0;
  }
  std::cout << g_failures << " CHAOS TEST(S) FAILED\n";
  return 1;
}
