// s05-kv Tests: 10k puts, scan-order, batch-atomicity, snapshot-isolation.
// Framework-los (assert + cout), damit kein gtest-Dependency noetig ist.
// CTest-Name: kv (ctest -R kv).

#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "dbengine/kv.h"

using dbengine::kv::KVStore;
using dbengine::kv::Op;
using dbengine::kv::WriteBatch;

namespace {

int g_failures = 0;

void Check(bool cond, const std::string& name) {
  if (cond) {
    std::cout << "PASS " << name << "\n";
  } else {
    std::cout << "FAIL " << name << "\n";
    ++g_failures;
  }
}

std::string Pad(int n, int width = 5) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%0*d", width, n);
  return std::string(buf);
}

void TestBasic() {
  KVStore kv;
  Check(!kv.Get("missing").has_value(), "basic/get-missing-nullopt");
  Check(kv.Empty() && kv.Size() == 0, "basic/empty");

  kv.Put("a", "1");
  auto v = kv.Get("a");
  Check(v.has_value() && *v == "1", "basic/put-get");

  kv.Put("a", "2");  // overwrite
  Check(kv.Get("a") == std::optional<std::string>("2"), "basic/overwrite");

  kv.Put("empty-val", "");
  Check(kv.Get("empty-val").has_value() && kv.Get("empty-val")->empty(),
        "basic/empty-value-ok");

  Check(kv.Delete("a") == true, "basic/delete-existing");
  Check(!kv.Get("a").has_value(), "basic/get-after-delete");
  Check(kv.Delete("a") == false, "basic/delete-missing-idempotent");
}

void TestPuts10k() {
  KVStore kv;
  constexpr int kN = 10000;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) {
    kv.Put("k" + Pad(i), "v" + Pad(i));
  }
  auto t1 = std::chrono::steady_clock::now();
  double secs =
      std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
  double ops = kN / (secs > 0 ? secs : 1e-9);
  std::cout << "THROUGHPUT puts10k: " << static_cast<long long>(ops)
            << " ops/s (" << kN << " puts in " << secs * 1000.0 << " ms)\n";

  Check(kv.Size() == static_cast<std::size_t>(kN), "puts10k/size");
  bool all_ok = true;
  for (int i = 0; i < kN; ++i) {
    auto got = kv.Get("k" + Pad(i));
    if (!got.has_value() || *got != "v" + Pad(i)) {
      all_ok = false;
      break;
    }
  }
  Check(all_ok, "puts10k/readback");

  // Scan ueber alles muss sortiert sein.
  auto all = kv.Scan("");
  bool sorted = all.size() == static_cast<std::size_t>(kN);
  for (std::size_t i = 1; sorted && i < all.size(); ++i) {
    if (!(all[i - 1].first < all[i].first)) sorted = false;
  }
  Check(sorted, "puts10k/scan-sorted");
}

void TestScanOrder() {
  KVStore kv;
  // Absichtlich rueckwaerts einfuegen.
  for (int i = 999; i >= 0; --i) kv.Put("user:" + Pad(i, 4), "n" + Pad(i, 4));
  kv.Put("order:1", "x");
  kv.Put("other", "y");

  auto users = kv.Scan("user:");
  Check(users.size() == 1000, "scan/prefix-count");
  bool sorted = true;
  for (std::size_t i = 1; i < users.size(); ++i) {
    if (!(users[i - 1].first < users[i].first)) {
      sorted = false;
      break;
    }
  }
  Check(sorted, "scan/order-sorted");
  Check(!users.empty() && users.front().first == "user:0000" &&
            users.back().first == "user:0999",
        "scan/order-endpoints");

  auto limited = kv.Scan("user:", 10);
  Check(limited.size() == 10 && limited.front().first == "user:0000",
        "scan/limit");

  Check(kv.Scan("nope:").empty(), "scan/unknown-prefix-empty");
  Check(kv.Scan("").size() == 1002, "scan/empty-prefix-all");
  Check(kv.Scan("", 0).empty(), "scan/limit-zero-empty");
}

void TestBatchAtomicity() {
  KVStore kv;
  kv.Put("a", "1");
  kv.Put("b", "1");

  // Gueltiger Batch: puts + delete gemischt.
  WriteBatch batch;
  batch.Put("a", "2");
  batch.Put("c", "3");
  batch.Delete("b");
  Check(kv.Write(batch) == true, "batch/valid-returns-true");
  Check(kv.Get("a") == std::optional<std::string>("2"), "batch/put-applied");
  Check(kv.Get("c") == std::optional<std::string>("3"), "batch/new-applied");
  Check(!kv.Get("b").has_value(), "batch/delete-applied");

  // Ungueltiger Batch (leerer Key): alles-oder-nichts, keine Teilmutation.
  std::vector<Op> bad;
  bad.push_back(Op::PutOp("a", "HACK"));
  bad.push_back(Op::PutOp("", "invalid-empty-key"));
  bad.push_back(Op::PutOp("d", "4"));
  Check(kv.Write(bad) == false, "batch/invalid-returns-false");
  Check(kv.Get("a") == std::optional<std::string>("2"),
        "batch/atomic-no-partial-put");
  Check(!kv.Get("d").has_value(), "batch/atomic-no-partial-new");

  // Delete-missing im Batch ist ok (idempotent), Batch committet.
  WriteBatch b2;
  b2.Delete("never-existed");
  b2.Put("e", "5");
  Check(kv.Write(b2) == true, "batch/delete-missing-ok");
  Check(kv.Get("e") == std::optional<std::string>("5"),
        "batch/after-delete-missing");

  // Leerer Batch: erfolgreiches No-Op.
  Check(kv.Write(std::vector<Op>{}) == true, "batch/empty-ok");
  Check(kv.Write(WriteBatch{}) == true, "batch/empty-builder-ok");
}

void TestSnapshotIsolation() {
  KVStore kv;
  kv.Put("k1", "v1");
  kv.Put("k2", "v1");
  auto snap = kv.GetSnapshot();

  // Nach Snapshot schreiben: live sieht Neues, Snapshot sieht Altes.
  kv.Put("k1", "v2");
  kv.Put("k3", "v3");
  kv.Delete("k2");

  Check(snap->Get("k1") == std::optional<std::string>("v1"),
        "snap/isolated-put");
  Check(snap->Get("k2") == std::optional<std::string>("v1"),
        "snap/isolated-delete");
  Check(!snap->Get("k3").has_value(), "snap/isolated-new-key");
  Check(kv.Get("k1") == std::optional<std::string>("v2"), "snap/live-put");
  Check(!kv.Get("k2").has_value(), "snap/live-delete");

  // Snapshot-Scan ist eingefroren (copy-on-read).
  auto srows = snap->Scan("");
  Check(srows.size() == 2, "snap/scan-frozen-size");
  auto snap2 = kv.GetSnapshot();
  Check(snap2->Scan("").size() == 2, "snap/scan-new-size");  // k1,k3
  Check(snap2->Get("k1") == std::optional<std::string>("v2"),
        "snap/new-sees-writes");

  // Snapshot-Iterator bleibt stabil trotz spaeterer Writes.
  auto it = kv.NewIterator(*snap);
  kv.Put("k0", "zero");
  std::vector<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next()) keys.push_back(it->key());
  Check(keys == std::vector<std::string>({"k1", "k2"}),
        "snap/iterator-stable");
}

void TestIterator() {
  KVStore kv;
  for (int i = 0; i < 100; ++i) kv.Put("it:" + Pad(i, 3), "v");
  kv.Put("zz", "end");

  auto it = kv.NewIterator("it:");
  it->SeekToFirst();
  Check(it->Valid() && it->key() == "it:000", "iter/seek-first");

  it->Seek("it:050");
  Check(it->Valid() && it->key() == "it:050", "iter/seek-mid");
  it->Next();
  Check(it->Valid() && it->key() == "it:051", "iter/next");

  it->Seek("it:999");  // hinter Ende
  Check(!it->Valid(), "iter/seek-past-end-invalid");

  // Volliteration zaehlt exakt.
  auto all = kv.NewIterator();
  std::size_t n = 0;
  for (all->SeekToFirst(); all->Valid(); all->Next()) ++n;
  Check(n == 101, "iter/full-count");

  // Prefix-Iterator sieht nur Prefix.
  auto pre = kv.NewIterator("it:");
  n = 0;
  for (pre->SeekToFirst(); pre->Valid(); pre->Next()) ++n;
  Check(n == 100, "iter/prefix-count");
}

void TestWalHook() {
  KVStore kv;
  std::vector<std::pair<std::string, std::uint64_t>> seen;
  kv.SetWalHook([&](const Op& op, std::uint64_t seq) {
    seen.emplace_back(op.key, seq);
  });
  kv.Put("a", "1");
  kv.Delete("b");
  WriteBatch batch;
  batch.Put("c", "3");
  batch.Put("d", "4");
  kv.Write(batch);
  Check(seen.size() == 4, "walhook/count");
  bool mono = true;
  for (std::size_t i = 1; i < seen.size(); ++i) {
    if (!(seen[i - 1].second < seen[i].second)) mono = false;
  }
  Check(mono, "walhook/seq-monotonic");

  kv.SetWalHook(nullptr);  // zurueck auf No-Op, darf nicht crashen
  kv.Put("e", "5");
  Check(kv.Get("e") == std::optional<std::string>("5"), "walhook/reset-noop");
}

void TestConcurrencySmoke() {
  KVStore kv;
  constexpr int kThreads = 4, kPer = 2500;
  std::vector<std::thread> th;
  for (int t = 0; t < kThreads; ++t) {
    th.emplace_back([&, t] {
      for (int i = 0; i < kPer; ++i) {
        kv.Put("t" + std::to_string(t) + ":" + Pad(i, 4),
               std::to_string(i));
      }
    });
  }
  for (auto& t : th) t.join();
  Check(kv.Size() == static_cast<std::size_t>(kThreads * kPer),
        "concurrent/size");
  auto rows = kv.Scan("t1:", 5);
  Check(rows.size() == 5, "concurrent/scan-after-join");
}

}  // namespace

int main() {
  TestBasic();
  TestPuts10k();
  TestScanOrder();
  TestBatchAtomicity();
  TestSnapshotIsolation();
  TestIterator();
  TestWalHook();
  TestConcurrencySmoke();

  if (g_failures == 0) {
    std::cout << "ALL KV TESTS PASSED\n";
    return 0;
  }
  std::cout << g_failures << " KV TEST(S) FAILED\n";
  return 1;
}
