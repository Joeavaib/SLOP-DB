// s07-index Tests: Secondary-Lookup, RangeScan-Order, TTL-Expire simuliert.
// Framework-frei (assert + main), damit CTest ohne gtest laeuft.

#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "dbengine/index/btree.h"

using dbengine::index::ArtIndexStub;
using dbengine::index::GinIndexStub;
using dbengine::index::SecondaryIndex;

static void test_secondary_lookup() {
  SecondaryIndex idx;
  // Duplikat-Sekundaerschluessel -> mehrere PKs (1:n, wie SQL Secondary Index)
  idx.insert(10, 100, 1000);
  idx.insert(10, 101, 1000);
  idx.insert(20, 200, 1000);

  auto v10 = idx.lookup(10, 1000);
  assert(v10.size() == 2);
  assert((v10[0] == 100 && v10[1] == 101) ||
         (v10[0] == 101 && v10[1] == 100));

  auto v20 = idx.lookup(20, 1000);
  assert(v20.size() == 1 && v20[0] == 200);

  auto vMiss = idx.lookup(999, 1000);
  assert(vMiss.empty());

  // Trennung Primary/Secondary: Index liefert nur PKs, kein Payload.
  // Remove genau eines Duplikat-Paares:
  assert(idx.remove(10, 100) == true);
  auto v10b = idx.lookup(10, 1000);
  assert(v10b.size() == 1 && v10b[0] == 101);
  // Nicht-existentes Paar:
  assert(idx.remove(10, 999) == false);
  assert(idx.remove(999, 101) == false);

  std::cout << "[ok] secondary_lookup\n";
}

static void test_range_scan_order() {
  SecondaryIndex idx;
  // Out-of-order einfuegen, inkl. Duplikat
  idx.insert(30, 300, 1000);
  idx.insert(10, 100, 1000);
  idx.insert(20, 200, 1000);
  idx.insert(20, 201, 1000);
  idx.insert(40, 400, 1000);

  auto all = idx.rangeScan(0, 100, 1000);
  assert(all.size() == 5);
  // Aufsteigend nach secondaryKey:
  for (size_t i = 1; i < all.size(); ++i) {
    assert(all[i - 1].first <= all[i].first);
  }
  assert(all[0].first == 10 && all[0].second == 100);
  assert(all[1].first == 20);
  assert(all[2].first == 20);
  assert(all[3].first == 30 && all[3].second == 300);
  assert(all[4].first == 40 && all[4].second == 400);

  // Fenster [15,30] inklusiv:
  auto win = idx.rangeScan(15, 30, 1000);
  assert(win.size() == 3);
  assert(win[0].first == 20 && win[2].first == 30);

  // Leeres Fenster / invertiert:
  assert(idx.rangeScan(50, 60, 1000).empty());
  assert(idx.rangeScan(30, 10, 1000).empty());

  std::cout << "[ok] range_scan_order\n";
}

static void test_ttl_expire_simuliert() {
  SecondaryIndex idx(10);  // expireAfterSec = 10
  const int64_t t0 = 1000;
  idx.insert(1, 101, t0);
  idx.insert(2, 102, t0);

  // Vor Ablauf sichtbar:
  assert(idx.lookup(1, t0 + 5).size() == 1);
  assert(idx.size(t0 + 5) == 2);

  // Grenze: now - created >= 10 -> abgelaufen (lazy, ohne Sweep):
  assert(idx.lookup(1, t0 + 10).empty());
  assert(idx.lookup(2, t0 + 10).empty());
  // rawSize enthaelt noch beide (noch kein Sweep):
  assert(idx.rawSize() == 2);
  assert(idx.size(t0 + 10) == 0);

  // RangeScan filtert ebenfalls lazy:
  assert(idx.rangeScan(0, 100, t0 + 10).empty());

  // Sweep entfernt physisch:
  size_t removed = idx.sweepExpired(t0 + 10);
  assert(removed == 2);
  assert(idx.rawSize() == 0);

  // Gemischt: ein Eintrag frisch, einer alt:
  SecondaryIndex idx2(10);
  idx2.insert(5, 501, t0);        // alt
  idx2.insert(5, 502, t0 + 100);  // frisch
  auto v = idx2.lookup(5, t0 + 100 + 5);
  // 501 abgelaufen (105-1000 >> 10), 502 lebt:
  assert(v.size() == 1 && v[0] == 502);
  assert(idx2.sweepExpired(t0 + 100 + 5) == 1);
  assert(idx2.rawSize() == 1);

  // TTL=0: nie Ablauf:
  SecondaryIndex noTtl(0);
  noTtl.insert(7, 701, t0);
  assert(noTtl.lookup(7, t0 + 100000).size() == 1);
  assert(noTtl.sweepExpired(t0 + 100000) == 0);

  // Wall-Clock-Pfad mit Override (deterministisch, ohne sleep):
  SecondaryIndex idx3(10);
  idx3.setNowOverride(t0);
  idx3.insert(9, 901);  // nutzt Override als createdSec
  idx3.setNowOverride(t0 + 5);
  assert(idx3.lookup(9).size() == 1);
  idx3.setNowOverride(t0 + 11);
  assert(idx3.lookup(9).empty());
  assert(idx3.sweepExpired() == 1);
  idx3.clearNowOverride();

  std::cout << "[ok] ttl_expire_simuliert\n";
}

static void test_art_gin_stubs() {
  ArtIndexStub art;
  bool threw = false;
  try {
    art.insert("hello", 1);
  } catch (const std::logic_error&) {
    threw = true;
  }
  assert(threw);

  GinIndexStub gin;
  threw = false;
  try {
    gin.insert(1, {"a", "b"});
  } catch (const std::logic_error&) {
    threw = true;
  }
  assert(threw);

  std::cout << "[ok] art_gin_stubs\n";
}

static void bench_inserts() {
  // Grobe Perf-Probe (kein SLO-Gate, nur Reporting fuer RFG-Meldung).
  SecondaryIndex idx;
  const int N = 200000;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; ++i) {
    idx.insert(i % 10000, i, 0);
  }
  auto t1 = std::chrono::steady_clock::now();
  double secs =
      std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0)
          .count();
  double ips = N / (secs > 0 ? secs : 1e-9);
  std::cout << "[bench] inserts: " << N << " in " << secs << "s -> " << ips
            << " inserts/s, rawSize=" << idx.rawSize() << "\n";
  // Lookup-Sanity nach Bench (kein Full-Scan nötig, equal_range):
  auto v = idx.lookup(42, 0);
  assert(v.size() == static_cast<size_t>(N / 10000));
}

int main() {
  test_secondary_lookup();
  test_range_scan_order();
  test_ttl_expire_simuliert();
  test_art_gin_stubs();
  bench_inserts();
  std::cout << "index tests passed\n";
  return 0;
}
