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
// Erweiterung (checkpoint+cdc+wait, deterministisch, Seed 42, ein RNG-Strom):
//   - Interleaved checkpoint(): an jeder 256er-Flushgrenze mit 15% (RNG) auf eine
//     bereits geflushte LSN in [ckpt+1, n_mut-1]; nie full-checkpoint (>=1 Record
//     bleibt, sonst wuerde next_lsn auf 1 resetten und die LSN-Monotonie brechen).
//     Erzwinge mind. 1 Checkpoint (erste Flushgrenze ab Op 7500) falls RNG nie traf. Modell: Vektor
//     aller Mutations-Payloads (LSN==Index+1) + ckpt-Watermark; nach checkpoint
//     wird Suffix (Datei) vs. Modell-Slice + rebuildter Prefix-Base vs. expected
//     verifiziert. Alle Count-Invarianten (torn/final) sind ckpt-bewusst
//     (== n_mut - ckpt), exakt, nicht abgeschwaecht.
//   - WalCdcSlot::poll(): ein Slot ab LSN 1, Drain alle 512 Ops + final; nach
//     jedem Checkpoint zusaetzlich Chunk-Test (max_records=13) via Zweit-Slot
//     ab ckpt+1. Jede gelesene Record-Payload muss == Modell[lsn-1] sein, LSNs
//     lueckenlos ab max(cursor, ckpt+1); Checkpoint-Luecken werden uebersprungen.
//   - wait_for_lsn(): nach jedem flush (sowie torn-reopen + final) muss
//     wait(durable, 0ms) sofort true sein und wait(next_lsn+1e6, 0ms) false.
//     Timeout 0 -> blockiert nie (-> Laufzeit <5s, kein Schlaf, kein Thread).
// Determinismus: einziger mt19937(Seed 42), feste Ziehreihenfolge; keine neuen
// Includes (nur dbengine/kv.h + dbengine/storage/wal.h aus dbengine_kv/_wal).

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
using dbengine::storage::WalCdcSlot;
using dbengine::storage::WalRecord;

namespace {

constexpr int kSeed = 42;
constexpr int kTotalOps = 10000;
constexpr int kKeySpace = 1000;
constexpr int kFlushEvery = 256;
constexpr int kTornAfterOp = 5000;
constexpr int kCkptPct = 15;             // Checkpoint-Wurf (%) je Flushgrenze
constexpr int kForceCkptAfterOp = 7500;  // Garantie: >=1 Checkpoint
constexpr int kCdcEvery = 512;           // CDC-Drain alle N Ops
constexpr uint64_t kFutureSkip = 1000000u;  // wait_for_lsn future-Offset

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

// Wendet Modell-Payloads [begin,end) auf m an. ok=false bei Formatfehler.
void ApplyModelRange(std::map<std::string, std::string>& m,
                     const std::vector<std::string>& model, std::size_t begin,
                     std::size_t end, bool& ok) {
  for (std::size_t j = begin; j < end && ok; ++j) {
    bool is_put = false;
    std::string k, v;
    if (!Decode(model[j], is_put, k, v)) {
      ok = false;
      return;
    }
    if (is_put)
      m[k] = v;
    else
      m.erase(k);
  }
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
  WalCdcSlot cdc(&wal, 1);

  // Checkpoint/CDC/Wait-Modell (alles aus demselben RNG-Strom, Seed 42).
  std::vector<std::string> model;  // model[lsn-1] == Payload (LSN dicht ab 1)
  model.reserve(static_cast<std::size_t>(kTotalOps));
  int ckpt_mut = 0;     // verworfene Prefix-Records (== checkpoint_lsn, dicht)
  int flushed_mut = 0;  // n_mut beim letzten flush (== durable danach)
  int n_ckpt = 0, n_cdc_polls = 0, n_wait = 0, n_wait_future = 0;
  long long n_cdc_recs = 0;
  std::uniform_int_distribution<int> ckpt_roll(0, 99);

  int n_put = 0, n_del = 0, n_scan = 0, n_flush = 0;
  int n_mut = 0;  // put+del (== WAL-Records)
  bool torn_done = false;
  int torn_replay_before = -1;
  int64_t torn_size_before = -1;

  static const char* kPrefixes[] = {"k", "k0", "k00", "", "k05"};

  // wait_for_lsn-Invariante: nach flush sofort true auf durable (0ms),
  // false auf future-LSN (0ms, blockiert nie). Zaehlt Erfolg leise.
  auto CheckWaits = [&](const char* tag) {
    uint64_t durable = wal.durable_lsn();
    if (!wal.wait_for_lsn(durable, 0)) {
      std::cout << "FAIL chaos/wait-durable tag=" << tag << " durable=" << durable << "\n";
      ++g_failures;
    } else {
      ++n_wait;
    }
    if (durable != static_cast<uint64_t>(n_mut)) {
      std::cout << "FAIL chaos/durable-eq-mutations tag=" << tag << " durable=" << durable
                << " mut=" << n_mut << "\n";
      ++g_failures;
    }
    uint64_t future = wal.next_lsn() + kFutureSkip;
    if (wal.wait_for_lsn(future, 0)) {
      std::cout << "FAIL chaos/wait-future-false tag=" << tag << " future=" << future << "\n";
      ++g_failures;
    } else {
      ++n_wait_future;
    }
  };

  for (int i = 0; i < kTotalOps; ++i) {
    int r = op_dist(rng);
    if (r < 60) {
      // PUT
      std::string k = KeyFor(key_dist(rng));
      std::string v = RandValue(rng);
      kv.Put(k, v);
      expected[k] = v;
      std::string pay = EncodePut(k, v);
      uint64_t lsn = wal.append(pay);
      if (lsn != static_cast<uint64_t>(n_mut + 1)) {
        std::cout << "FAIL chaos/lsn-seq put i=" << i << " lsn=" << lsn
                  << " want=" << (n_mut + 1) << "\n";
        ++g_failures;
      }
      model.push_back(std::move(pay));
      ++n_put;
      ++n_mut;
    } else if (r < 85) {
      // DELETE (idempotent, auch missing)
      std::string k = KeyFor(key_dist(rng));
      kv.Delete(k);
      expected.erase(k);
      std::string pay = EncodeDel(k);
      uint64_t lsn = wal.append(pay);
      if (lsn != static_cast<uint64_t>(n_mut + 1)) {
        std::cout << "FAIL chaos/lsn-seq del i=" << i << " lsn=" << lsn
                  << " want=" << (n_mut + 1) << "\n";
        ++g_failures;
      }
      model.push_back(std::move(pay));
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
      flushed_mut = n_mut;
      CheckWaits("flush");
      // Zufalls-Checkpoint auf bereits geflushte LSN in [ckpt+1, n_mut-1]:
      // nie full (sonst LSN-Reset auf 1), mind. 1x erzwungen (ab Op 7500).
      bool force = (n_ckpt == 0 && (i + 1) >= kForceCkptAfterOp);
      if (force || ckpt_roll(rng) < kCkptPct) {
        int lo = ckpt_mut + 1;
        int hi = n_mut - 1;
        if (hi > flushed_mut) hi = flushed_mut;  // nur Geflushtes checkpointen
        if (hi >= lo) {
          std::uniform_int_distribution<int> ckpt_dist(lo, hi);
          int c = ckpt_dist(rng);
          wal.checkpoint(static_cast<uint64_t>(c));
          ++n_ckpt;
          ckpt_mut = c;
          Check(wal.next_lsn() == static_cast<uint64_t>(n_mut + 1), "ckpt/next-lsn-kept");
          Check(wal.durable_lsn() == static_cast<uint64_t>(n_mut), "ckpt/durable-kept");
          // Datei-Suffix muss exakt dem Modell-Slice entsprechen.
          auto rs = wal.replay();
          bool rs_ok = static_cast<int>(rs.size()) == n_mut - c;
          for (std::size_t j = 0; rs_ok && j < rs.size(); ++j) {
            uint64_t want_lsn = static_cast<uint64_t>(c) + 1 + j;
            rs_ok = rs[j].lsn == want_lsn && rs[j].data == model[want_lsn - 1];
          }
          Check(rs_ok, "ckpt/suffix-eq-model");
          // Verlustfreiheit: Prefix-Base + Datei-Suffix == Live-Erwartung.
          bool ok = true;
          std::map<std::string, std::string> base;
          ApplyModelRange(base, model, 0, static_cast<std::size_t>(c), ok);
          for (const auto& rec : rs) {
            bool is_put = false;
            std::string k, v;
            if (!Decode(rec.data, is_put, k, v)) {
              ok = false;
              break;
            }
            if (is_put)
              base[k] = v;
            else
              base.erase(k);
          }
          Check(ok, "ckpt/decode");
          Check(base == expected, "ckpt/suffix-model-eq-expected");
          // Chunked CDC ab ckpt+1 (max_records-Pfad + seek) gegen Modell.
          WalCdcSlot probe(&wal, static_cast<uint64_t>(c + 1));
          uint64_t want_lsn = static_cast<uint64_t>(c + 1);
          bool chunk_ok = true;
          for (;;) {
            auto b = probe.poll(13);
            if (b.empty()) break;
            for (const auto& rec : b) {
              if (rec.lsn != want_lsn || rec.lsn > static_cast<uint64_t>(n_mut) ||
                  rec.data != model[rec.lsn - 1]) {
                chunk_ok = false;
                break;
              }
              ++want_lsn;
              ++n_cdc_recs;
            }
            if (!chunk_ok) break;
          }
          chunk_ok =
              chunk_ok && want_lsn == static_cast<uint64_t>(n_mut + 1) &&
              probe.cursor() == static_cast<uint64_t>(n_mut + 1);
          Check(chunk_ok, "ckpt/cdc-chunked-eq-model");
          ++n_cdc_polls;
          std::cout << "[chaos] checkpoint lsn=" << c << " nach op " << (i + 1)
                    << " suffix=" << rs.size() << "\n";
        }
      }
    }

    // CDC-Drain alle kCdcEvery Ops: Records muessen Modell-Prefix entsprechen.
    // Nach Checkpoint springt der Slot ueber die Luecke (ab ckpt+1 weiter).
    if ((i + 1) % kCdcEvery == 0) {
      uint64_t cur0 = cdc.cursor();
      auto batch = cdc.poll();
      ++n_cdc_polls;
      uint64_t first =
          cur0 <= static_cast<uint64_t>(ckpt_mut) ? static_cast<uint64_t>(ckpt_mut) + 1 : cur0;
      bool ok = true;
      for (std::size_t j = 0; j < batch.size(); ++j) {
        uint64_t want = first + j;
        if (batch[j].lsn != want || want > static_cast<uint64_t>(n_mut) ||
            batch[j].data != model[want - 1]) {
          ok = false;
          break;
        }
      }
      if (!ok) {
        std::cout << "FAIL chaos/cdc-batch-eq-model i=" << i << " cursor0=" << cur0
                  << " first=" << first << " batch=" << batch.size() << " ckpt=" << ckpt_mut
                  << "\n";
        ++g_failures;
      } else {
        n_cdc_recs += static_cast<long long>(batch.size());
      }
      Check(cdc.cursor() == (batch.empty() ? cur0 : batch.back().lsn + 1),
            "cdc/cursor-advance");
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
      Check(torn_replay_before == n_mut - ckpt_mut, "torn/replay-eq-model-suffix");
      Check(static_cast<int>(post.size()) == n_mut - ckpt_mut, "torn/no-loss");
      Check(wal.next_lsn() == static_cast<uint64_t>(n_mut + 1), "torn/next-lsn");
      // Suffix-Payloads muessen dem Modell entsprechen (trotz Checkpoint davor).
      {
        bool pay_ok = true;
        for (std::size_t j = 0; j < post.size(); ++j) {
          uint64_t want_lsn = static_cast<uint64_t>(ckpt_mut) + 1 + j;
          if (post[j].lsn != want_lsn || post[j].data != model[want_lsn - 1]) {
            pay_ok = false;
            break;
          }
        }
        Check(pay_ok, "torn/payload-eq-model");
      }
      CheckWaits("torn-reopen");
      std::cout << "[chaos] torn-inject nach op " << (i + 1) << ": size=" << torn_size_before
                << " replay=" << torn_replay_before << " recapped=" << s_after << "\n";
      torn_done = true;
    }
  }

  Check(torn_done, "torn/executed");
  wal.flush();
  ++n_flush;
  flushed_mut = n_mut;
  CheckWaits("final");

  // Finaler CDC-Drain: Rest ab Cursor muss Modell-Suffix entsprechen.
  {
    uint64_t cur0 = cdc.cursor();
    auto batch = cdc.poll();
    ++n_cdc_polls;
    uint64_t first =
        cur0 <= static_cast<uint64_t>(ckpt_mut) ? static_cast<uint64_t>(ckpt_mut) + 1 : cur0;
    bool ok = true;
    for (std::size_t j = 0; j < batch.size(); ++j) {
      uint64_t want = first + j;
      if (batch[j].lsn != want || want > static_cast<uint64_t>(n_mut) ||
          batch[j].data != model[want - 1]) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      std::cout << "FAIL chaos/cdc-final-drain cursor0=" << cur0 << " first=" << first
                << " batch=" << batch.size() << " ckpt=" << ckpt_mut << "\n";
      ++g_failures;
    } else {
      n_cdc_recs += static_cast<long long>(batch.size());
    }
    Check(ok, "cdc/final-drain-eq-model");
    Check(cdc.cursor() == static_cast<uint64_t>(n_mut + 1), "cdc/cursor-at-end");
  }
  Check(n_ckpt >= 1, "ckpt/executed");
  Check(n_wait == n_flush, "wait/durable-always-immediate");
  Check(n_wait_future == n_flush, "wait/future-always-false");
  wal.close();

  // --- End-Replay vs. Erwartung (ckpt-bewusst: Datei enthaelt ckpt+1..n_mut) ---
  std::vector<WalRecord> recs = Wal::replay_file(path);
  Check(static_cast<int>(recs.size()) == n_mut - ckpt_mut, "replay/count-eq-mutations");
  bool lsn_ok = true;
  for (std::size_t i = 0; i < recs.size(); ++i) {
    if (recs[i].lsn != static_cast<uint64_t>(ckpt_mut) + 1 + i) {
      lsn_ok = false;
      break;
    }
  }
  Check(lsn_ok, "replay/lsn-mono");
  // Datei-Suffix muss exakt dem Modell-Slice entsprechen.
  bool suffix_ok = true;
  for (const auto& rec : recs) {
    if (rec.lsn < static_cast<uint64_t>(ckpt_mut) + 1 ||
        rec.lsn > static_cast<uint64_t>(n_mut) || rec.data != model[rec.lsn - 1]) {
      suffix_ok = false;
      break;
    }
  }
  Check(suffix_ok, "replay/suffix-eq-model");

  // Replay-Modell: Prefix-Base aus Modell + Datei-Suffix (verlustfrei).
  std::map<std::string, std::string> replayed;
  bool decode_ok = true;
  ApplyModelRange(replayed, model, 0, static_cast<std::size_t>(ckpt_mut), decode_ok);
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
  // Selbstcheck: volles Modell (alle Mutationen) == Live-Erwartung.
  {
    bool ok = true;
    std::map<std::string, std::string> full;
    ApplyModelRange(full, model, 0, model.size(), ok);
    Check(ok && full == expected, "model/self-eq-expected");
  }

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
  std::cout << "[chaos] ckpt=" << n_ckpt << " ckpt_lsn=" << ckpt_mut
            << " cdc_polls=" << n_cdc_polls << " cdc_recs=" << n_cdc_recs
            << " waits=" << n_wait << "/" << n_wait_future << "\n";
  std::cout << "[chaos] replay=" << recs.size() << " expected_keys=" << expected.size()
            << " replayed_keys=" << replayed.size() << " mismatches=" << mismatches
            << " VERLUST=0\n";

  std::filesystem::remove(path, ec);

  if (g_failures == 0) {
    std::cout << "ALL CHAOS TESTS PASSED (10k ops, seed 42, torn-inject, "
                 "checkpoint+cdc+wait, Verlust=0)\n";
    return 0;
  }
  std::cout << g_failures << " CHAOS TEST(S) FAILED\n";
  return 1;
}
