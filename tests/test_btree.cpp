// Phase-0-Herzstueck btreekv-Tests (ohne gtest, check-basiert wie test_kv.cpp):
//  1. open-empty (frisch: leer, Height 0)
//  2. basic-crud (put/get/overwrite/empty-value/delete + flush/reopen)
//  3. puts10k (Seed 42 geshuffelt, exakte Gets, sortierter Scan, Tiefe >= 3)
//  4. scan-range ([from,to)-Bounds, Limit, leer-Faelle)
//  5. batch-atomicity (valid commit+durable ohne expliziten Flush, invalid
//     alles-oder-nichts, delete-missing, empty-batch, oversize)
//  6. split-depth-klein (100 sequenziell => Height >= 2, Reopen-stabil)
//  7. merge-shrink (alles loeschen => leer, Height 0, Reopen-stabil)
//  8. deletes-reopen (10k halb loeschen, Flush, Reopen-Konsistenz)
//  9. oversize (Eintrag > MaxEntryBytes => false)
// 10. iterator (prefix, seek/next, past-end, full-count)
// Temp-Pfade + Cleanup. CTest-Name: btreekv (ctest -R btreekv).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "dbengine/kv/btree.h"

using dbengine::kv::BTreeKV;
using dbengine::kv::Op;
using dbengine::kv::WriteBatch;

namespace {

int g_failures = 0;
int g_tmp_counter = 0;

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

std::string TmpPath(const char* name) {
  auto p = std::filesystem::temp_directory_path() /
           (std::string("btreekv_") + name + "_" +
            std::to_string(static_cast<long long>(::getpid())) + "_" +
            std::to_string(g_tmp_counter++) + ".db");
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(std::string(p.string()) + ".tmp", ec);
  return p.string();
}

void Cleanup(const std::string& p) {
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p + ".tmp", ec);  // Pager-tmp-Rest (Crash-Fenster)
}

void TestOpenEmpty() {
  const std::string path = TmpPath("empty");
  BTreeKV db;
  Check(db.Open(path), "empty/open-fresh");
  Check(!db.Open(path), "empty/double-open-rejected");
  Check(db.IsOpen(), "empty/is-open");
  Check(db.Empty() && db.Size() == 0, "empty/size-zero");
  Check(!db.Get("missing").has_value(), "empty/get-missing-nullopt");
  Check(db.Height() == 0, "empty/height-zero");
  Check(db.ScanRange("", "").empty(), "empty/scan-empty");
  Check(db.Scan("", 10).empty(), "empty/prefix-scan-empty");
  auto it = db.NewIterator();
  it->SeekToFirst();
  Check(!it->Valid(), "empty/iterator-invalid");
  Check(db.Delete("missing") == false, "empty/delete-missing-false");
  Check(db.Flush(), "empty/flush-noop-true");
  db.Close();
  Check(!db.IsOpen(), "empty/closed");
  Check(!db.Get("x").has_value(), "empty/get-when-closed-nullopt");
  Check(!db.Put("x", "y"), "empty/put-when-closed-false");
  Check(!db.Flush(), "empty/flush-when-closed-false");
  Cleanup(path);
}

void TestBasicCrud() {
  const std::string path = TmpPath("basic");
  {
    BTreeKV db;
    Check(db.Open(path), "basic/open");
    Check(db.Put("a", "1"), "basic/put");
    Check(db.Get("a") == std::optional<std::string>("1"), "basic/get");
    Check(db.Put("a", "2"), "basic/overwrite-put");
    Check(db.Get("a") == std::optional<std::string>("2"),
          "basic/overwrite-get");
    Check(db.Put("empty-val", ""), "basic/empty-value-put");
    Check(db.Get("empty-val").has_value() &&
              db.Get("empty-val")->empty(),
          "basic/empty-value-get");
    Check(!db.Put("", "x"), "basic/empty-key-rejected");
    Check(db.Size() == 2, "basic/size");
    Check(db.Delete("a"), "basic/delete-existing");
    Check(!db.Get("a").has_value(), "basic/get-after-delete");
    Check(!db.Delete("a"), "basic/delete-missing-idempotent");
    Check(db.Flush(), "basic/flush");
  }
  {  // Reopen: committed (geflusht) steht wieder.
    BTreeKV db;
    Check(db.Open(path), "basic/reopen");
    Check(db.Size() == 1, "basic/reopen-size");
    Check(db.Get("empty-val").has_value() &&
              db.Get("empty-val")->empty(),
          "basic/reopen-value");
    Check(!db.Get("a").has_value(), "basic/reopen-delete-persisted");
    db.Close();
  }
  Cleanup(path);
}

void TestPuts10k() {
  const std::string path = TmpPath("puts10k");
  BTreeKV db;
  Check(db.Open(path), "puts10k/open");
  constexpr int kN = 10000;
  std::vector<int> order(kN);
  for (int i = 0; i < kN; ++i) order[static_cast<std::size_t>(i)] = i;
  std::shuffle(order.begin(), order.end(), std::mt19937(42));  // Seed 42
  auto t0 = std::chrono::steady_clock::now();
  for (int i : order) {
    if (!db.Put("k" + Pad(i), "v" + Pad(i))) {
      Check(false, "puts10k/put-ok");
      db.Close();
      Cleanup(path);
      return;
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  double secs =
      std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
  double ops = kN / (secs > 0 ? secs : 1e-9);
  std::cout << "THROUGHPUT btree-puts10k(ram): "
            << static_cast<long long>(ops) << " ops/s (" << kN << " puts in "
            << secs * 1000.0 << " ms)\n";
  Check(db.Size() == static_cast<std::size_t>(kN), "puts10k/size");
  // Tiefe > 1 erzwungen: max. 63 Keys/Knoten => 10k Keys brauchen >= 3 Ebenen
  // (2 Ebenen decken hoechstens 64*63 = 4032 Keys ab).
  Check(db.Height() >= 3, "puts10k/height-ge-3");
  Check(db.NodeCount() > 1, "puts10k/multiple-nodes");
  Check(db.Flush(), "puts10k/flush");

  bool all_ok = true;
  for (int i = 0; i < kN; ++i) {
    auto got = db.Get("k" + Pad(i));
    if (!got.has_value() || *got != "v" + Pad(i)) {
      all_ok = false;
      break;
    }
  }
  Check(all_ok, "puts10k/readback-exact");

  auto all = db.ScanRange("", "");
  bool sorted = all.size() == static_cast<std::size_t>(kN);
  for (std::size_t i = 1; sorted && i < all.size(); ++i) {
    if (!(all[i - 1].first < all[i].first)) sorted = false;
  }
  Check(sorted, "puts10k/scan-sorted");
  Check(!all.empty() && all.front().first == "k00000" &&
            all.back().first == "k09999",
        "puts10k/scan-endpoints");
  db.Close();
  Cleanup(path);
}

void TestScanRange() {
  const std::string path = TmpPath("scan");
  BTreeKV db;
  Check(db.Open(path), "scan/open");
  for (int i = 0; i < 1000; ++i) db.Put("k" + Pad(i), "v" + Pad(i));
  db.Put("other", "x");
  Check(db.Flush(), "scan/flush");

  auto all = db.ScanRange("", "");
  Check(all.size() == 1001, "scan/full-count");

  auto win = db.ScanRange("k" + Pad(100), "k" + Pad(200));
  Check(win.size() == 100, "scan/window-count");
  Check(!win.empty() && win.front().first == "k" + Pad(100) &&
            win.back().first == "k" + Pad(199),
        "scan/window-endpoints");
  bool sorted = true;
  for (std::size_t i = 1; i < win.size(); ++i) {
    if (!(win[i - 1].first < win[i].first)) {
      sorted = false;
      break;
    }
  }
  Check(sorted, "scan/window-sorted");

  auto lim = db.ScanRange("", "", 10);
  Check(lim.size() == 10 && lim.front().first == "k" + Pad(0),
        "scan/limit");
  Check(db.ScanRange("", "", 0).empty(), "scan/limit-zero-empty");
  Check(db.ScanRange("k" + Pad(500), "k" + Pad(100)).empty(),
        "scan/from-greater-to-empty");
  Check(db.ScanRange("other", "").size() == 1, "scan/open-end");

  auto pre = db.Scan("k" + Pad(0, 2), 5);  // Prefix "k00"
  Check(pre.size() == 5 && pre.front().first == "k" + Pad(0),
        "scan/prefix-limit");
  Check(db.Scan("nope:", 10).empty(), "scan/unknown-prefix-empty");
  db.Close();
  Cleanup(path);
}

void TestBatchAtomicity() {
  const std::string path = TmpPath("batch");
  {
    BTreeKV db;
    Check(db.Open(path), "batch/open");
    Check(db.Put("a", "1") && db.Put("b", "1"), "batch/baseline-puts");
    Check(db.Flush(), "batch/baseline-flush");
    const std::uint64_t seq0 = db.Sequence();

    WriteBatch batch;
    batch.Put("a", "2");
    batch.Put("c", "3");
    batch.Delete("b");
    Check(db.Write(batch), "batch/valid-true");
    Check(db.Get("a") == std::optional<std::string>("2"),
          "batch/put-applied");
    Check(db.Get("c") == std::optional<std::string>("3"),
          "batch/new-applied");
    Check(!db.Get("b").has_value(), "batch/delete-applied");
    Check(db.Sequence() > seq0, "batch/seq-advanced");

    std::vector<Op> bad;
    bad.push_back(Op::PutOp("a", "HACK"));
    bad.push_back(Op::PutOp("", "invalid-empty-key"));
    bad.push_back(Op::PutOp("d", "4"));
    Check(!db.Write(bad), "batch/invalid-false");
    Check(db.Get("a") == std::optional<std::string>("2"),
          "batch/atomic-no-partial-put");
    Check(!db.Get("d").has_value(), "batch/atomic-no-partial-new");

    std::vector<Op> oversize;
    oversize.push_back(
        Op::PutOp("ok", "1"));
    oversize.push_back(
        Op::PutOp("huge", std::string(BTreeKV::MaxEntryBytes() + 1, 'x')));
    Check(!db.Write(oversize), "batch/oversize-false");
    Check(!db.Get("ok").has_value() &&
              db.Get("a") == std::optional<std::string>("2"),
          "batch/oversize-no-partial");

    WriteBatch b2;
    b2.Delete("never-existed");
    b2.Put("e", "5");
    Check(db.Write(b2), "batch/delete-missing-ok");
    Check(db.Get("e") == std::optional<std::string>("5"),
          "batch/after-delete-missing");

    Check(db.Write(std::vector<Op>{}), "batch/empty-vector-ok");
    Check(db.Write(WriteBatch{}), "batch/empty-builder-ok");
    // KEIN expliziter Flush: Write() committet dauerhaft => Reopen-Test unten.
  }
  {
    BTreeKV db;
    Check(db.Open(path), "batch/reopen");
    Check(db.Get("a") == std::optional<std::string>("2"),
          "batch/reopen-put-durable");
    Check(db.Get("c") == std::optional<std::string>("3"),
          "batch/reopen-new-durable");
    Check(!db.Get("b").has_value(), "batch/reopen-delete-durable");
    Check(db.Get("e") == std::optional<std::string>("5"),
          "batch/reopen-second-batch-durable");
    db.Close();
  }
  Cleanup(path);
}

void TestSplitDepthSmall() {
  const std::string path = TmpPath("depth");
  {
    BTreeKV db;
    Check(db.Open(path), "depth/open");
    // 100 Keys > 63 (Knoten-Max) => Wurzel muss spalten => Height >= 2.
    for (int i = 0; i < 100; ++i) db.Put("s" + Pad(i, 3), "v");
    Check(db.Height() >= 2, "depth/height-ge-2");
    Check(db.NodeCount() > 1, "depth/multiple-nodes");
    Check(db.Size() == 100, "depth/size");
    Check(db.Flush(), "depth/flush");
  }
  {
    BTreeKV db;
    Check(db.Open(path), "depth/reopen");
    Check(db.Height() >= 2, "depth/reopen-height");
    Check(db.Size() == 100, "depth/reopen-size");
    Check(db.Get("s050") == std::optional<std::string>("v"),
          "depth/reopen-get");
    db.Close();
  }
  Cleanup(path);
}

void TestMergeShrink() {
  const std::string path = TmpPath("merge");
  {
    BTreeKV db;
    Check(db.Open(path), "merge/open");
    for (int i = 0; i < 500; ++i) db.Put("m" + Pad(i, 3), "v");
    Check(db.Flush(), "merge/fill-flush");
    const std::size_t h_before = db.Height();
    Check(h_before >= 2, "merge/grown-height");
    for (int i = 0; i < 500; ++i) {
      if (!db.Delete("m" + Pad(i, 3))) {
        Check(false, "merge/delete-ok");
        break;
      }
    }
    Check(db.Empty() && db.Size() == 0, "merge/empty-after-delete-all");
    Check(db.Height() == 0, "merge/height-zero-after-wipe");
    Check(!db.Get("m000").has_value(), "merge/get-after-wipe-missing");
    Check(db.Flush(), "merge/wipe-flush");
  }
  {
    BTreeKV db;
    Check(db.Open(path), "merge/reopen");
    Check(db.Empty() && db.Size() == 0, "merge/reopen-empty");
    Check(db.Height() == 0, "merge/reopen-height-zero");
    // Baum bleibt beschreibbar nach Komplett-Loeschung + Reopen.
    Check(db.Put("again", "1") && db.Flush(), "merge/reuse-after-wipe");
    db.Close();
  }
  {
    BTreeKV db;
    Check(db.Open(path), "merge/reopen2");
    Check(db.Get("again") == std::optional<std::string>("1"),
          "merge/reuse-durable");
    db.Close();
  }
  Cleanup(path);
}

void TestDeletesReopen() {
  const std::string path = TmpPath("delreopen");
  {
    BTreeKV db;
    Check(db.Open(path), "delreopen/open");
    constexpr int kN = 10000;
    for (int i = 0; i < kN; ++i) db.Put("k" + Pad(i), "v" + Pad(i));
    for (int i = 0; i < kN; i += 2) db.Delete("k" + Pad(i));
    Check(db.Size() == 5000, "delreopen/size-after-delete");
    Check(db.Flush(), "delreopen/flush");
  }
  {
    BTreeKV db;
    Check(db.Open(path), "delreopen/reopen");
    Check(db.Size() == 5000, "delreopen/reopen-size");
    bool ok = true;
    for (int i = 0; i < 10000; ++i) {
      auto got = db.Get("k" + Pad(i));
      const bool odd = (i % 2 == 1);
      if (odd && (!got.has_value() || *got != "v" + Pad(i))) {
        ok = false;
        break;
      }
      if (!odd && got.has_value()) {
        ok = false;
        break;
      }
    }
    Check(ok, "delreopen/reopen-gets");
    auto all = db.ScanRange("", "");
    bool sorted = all.size() == 5000;
    for (std::size_t i = 1; sorted && i < all.size(); ++i) {
      if (!(all[i - 1].first < all[i].first)) sorted = false;
    }
    Check(sorted, "delreopen/reopen-scan-sorted");
    db.Close();
  }
  Cleanup(path);
}

void TestOversize() {
  const std::string path = TmpPath("oversize");
  BTreeKV db;
  Check(db.Open(path), "oversize/open");
  Check(!db.Put("big", std::string(BTreeKV::MaxEntryBytes() + 1, 'x')),
        "oversize/put-rejected");
  Check(db.Empty(), "oversize/no-partial-state");
  Check(db.Put("small", "ok"), "oversize/normal-put-ok");
  db.Close();
  Cleanup(path);
}

void TestIterator() {
  const std::string path = TmpPath("iter");
  BTreeKV db;
  Check(db.Open(path), "iter/open");
  for (int i = 0; i < 100; ++i) db.Put("it:" + Pad(i, 3), "v");
  db.Put("zz", "end");
  Check(db.Flush(), "iter/flush");

  auto it = db.NewIterator("it:");
  it->SeekToFirst();
  Check(it->Valid() && it->key() == "it:000", "iter/seek-first");

  it->Seek("it:050");
  Check(it->Valid() && it->key() == "it:050", "iter/seek-mid");
  it->Next();
  Check(it->Valid() && it->key() == "it:051", "iter/next");

  it->Seek("it:999");
  Check(!it->Valid(), "iter/seek-past-end-invalid");

  auto all = db.NewIterator();
  std::size_t n = 0;
  for (all->SeekToFirst(); all->Valid(); all->Next()) ++n;
  Check(n == 101, "iter/full-count");

  auto pre = db.NewIterator("it:");
  n = 0;
  for (pre->SeekToFirst(); pre->Valid(); pre->Next()) ++n;
  Check(n == 100, "iter/prefix-count");

  // Iterator-Sicht stabil gegen spaetere Writes.
  auto snap = db.NewIterator();
  db.Put("it:000", "changed");
  db.Delete("zz");
  std::size_t m = 0;
  for (snap->SeekToFirst(); snap->Valid(); snap->Next()) ++m;
  Check(m == 101, "iter/snapshot-stable");
  db.Close();
  Cleanup(path);
}

}  // namespace

// s111: Persist-Fehlschlag -> false == kein Commit (weder sichtbar noch
// haltbar). Trick: Verzeichnis read-only (0555) -> Pager tmp+rename schlägt
// fehl (kein root nötig, uid!=0 vorausgesetzt). Danach alter Stand sichtbar,
// nach chmod zurück + Reopen ist die DB intakt.
void TestPersistFailureRollback() {
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("btreekv_rodir_" + std::to_string(static_cast<long long>(::getpid())));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directory(dir, ec);
  if (ec) {
    std::cout << "SKIP persist-rollback (kein tmp-Verzeichnis)\n";
    return;
  }
  const std::string path = (dir / "t.db").string();
  BTreeKV db;
  Check(db.Open(path), "rollback/open");
  Check(db.Put("stable", "v1"), "rollback/baseline-put");
  // Baseline erst durable machen (Einzel-Put puffert nur RAM) — der
  // durable Stand, auf den das Rollback zurueckfallen muss, ist v1.
  Check(db.Flush(), "rollback/baseline-flush");
  // Read-only schalten: Dateien anlegen geht nicht mehr (tmp+rename -> EACCES).
  ::chmod(dir.c_str(), 0555);
  WriteBatch wb;
  wb.Put("stable", "v2-poison");
  wb.Put("ghost", "should-not-exist");
  const bool ok = db.Write(wb);
  ::chmod(dir.c_str(), 0755);  // sofort zurück (Cleanup auch bei FAIL)
  Check(!ok, "rollback/write-fails-on-EACCES");
  if (db.IsOpen()) {
    auto g = db.Get("stable");
    Check(g.has_value() && *g == "v1", "rollback/old-value-visible");
    Check(!db.Get("ghost").has_value(), "rollback/ghost-absent");
  } else {
    Check(false, "rollback/handle-survives-reloadable-failure");
  }
  // Nach Entsperren: normale Writes + Reopen intakt.
  WriteBatch wb2;
  wb2.Put("stable", "v2");
  Check(db.Write(wb2), "rollback/write-after-heal");
  db.Close();
  BTreeKV db2;
  Check(db2.Open(path), "rollback/reopen");
  auto g = db2.Get("stable");
  Check(g.has_value() && *g == "v2", "rollback/durable-after-heal");
  Check(!db2.Get("ghost").has_value(), "rollback/ghost-never-persisted");
  db2.Close();
  std::filesystem::remove_all(dir, ec);
}

// s115: Early-Stop — limit=1 auf 100k-Baum darf keinen Full-Walk machen
// (vorher ~5 ms Vollmaterialisierung, jetzt µs). Korrektheit exakt:
// Range-Bounds, Prefix-Ende, Iterator-Vergleich.
void TestScanEarlyStop() {
  const std::string path = TmpPath("scanstop");
  BTreeKV db;
  Check(db.Open(path), "scanstop/open");
  char k[32];
  for (int i = 0; i < 100000; ++i) {
    std::snprintf(k, sizeof k, "p:%06d", i);
    Check(db.Put(k, "v"), "scanstop/fill");
  }
  Check(db.Flush(), "scanstop/flush");
  auto t0 = std::chrono::steady_clock::now();
  auto r1 = db.Scan("p:000000", 1);
  auto t1 = std::chrono::steady_clock::now();
  Check(r1.size() == 1 && r1[0].first == "p:000000", "scanstop/limit1-exact");
  const auto us1 =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  Check(us1 < 1000, "scanstop/limit1-no-fullwalk");
  auto rr = db.ScanRange("p:050000", "p:050010", 100);
  Check(rr.size() == 10, "scanstop/range-count");
  for (int i = 0; i < 10; ++i) {
    std::snprintf(k, sizeof k, "p:%06d", 50000 + i);
    Check(rr[static_cast<size_t>(i)].first == k, "scanstop/range-exact");
  }
  // Prefix-Ende: Treffer muessen exakt bei Prefix-Grenze aufhoeren.
  auto rp = db.Scan("p:00000", 100000);
  Check(rp.size() == 10, "scanstop/prefix-count");
  // Leerer Prefix + Limit 0 Verhalten unveraendert.
  Check(db.Scan("p:", 0).empty(), "scanstop/limit0-empty");
  Check(db.Scan("zzz", 5).empty(), "scanstop/miss-empty");
  // Iterator ueber Prefix sieht dieselbe Menge.
  auto it = db.NewIterator("p:00000");
  std::size_t m = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) ++m;
  Check(m == 10, "scanstop/iterator-prefix-count");
  std::cout << "  info: scanstop limit1 us=" << us1 << "\n";
  db.Close();
  Cleanup(path);
}

int main() {  TestOpenEmpty();
  TestBasicCrud();
  TestPuts10k();
  TestScanRange();
  TestBatchAtomicity();
  TestSplitDepthSmall();
  TestMergeShrink();
  TestDeletesReopen();
  TestOversize();
  TestIterator();
  TestPersistFailureRollback();
  TestScanEarlyStop();

  if (g_failures == 0) {
    std::cout << "ALL BTREE TESTS PASSED\n";
    return 0;
  }
  std::cout << g_failures << " BTREE TEST(S) FAILED\n";
  return 1;
}
