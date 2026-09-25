// s17-ts Tests: Hypertable-Routing, 100k-Ingest, Retention-Drop, Cont-Agg, ASOF.
// Stil: NDEBUG-sicher (eigene CHECK, kein assert), ctest-Name: chunks.
// Release baut mit -DNDEBUG -> assert waere No-Op, daher CHECK mit return 1.

#include <cmath>
#include <chrono>
#include <iostream>
#include <vector>

#include "dbengine/ts/chunks.h"

using dbengine::ts::AsofJoin;
using dbengine::ts::Chunk;
using dbengine::ts::ContinuousAgg;
using dbengine::ts::FloorBucket;
using dbengine::ts::Hypertable;
using dbengine::ts::Row;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::cerr << "FAIL " << __func__ << ":" << __LINE__ << ": " #cond     \
                << "\n";                                                   \
      return false;                                                        \
    }                                                                      \
  } while (0)

namespace {

bool t_floor_bucket() {
  CHECK(FloorBucket(0, 10) == 0);
  CHECK(FloorBucket(9, 10) == 0);
  CHECK(FloorBucket(10, 10) == 10);
  CHECK(FloorBucket(-1, 10) == -10);
  CHECK(FloorBucket(-10, 10) == -10);
  CHECK(FloorBucket(-11, 10) == -20);
  return true;
}

bool t_chunk_ordered_insert() {
  Chunk c(0, 10);
  CHECK(c.t_start() == 0 && c.t_end() == 10);
  c.Insert(Row{5, 1.0});
  c.Insert(Row{1, 2.0});  // out-of-order -> sortiert einsortiert
  c.Insert(Row{9, 3.0});
  CHECK(c.Size() == 3);
  CHECK(c.IsOrdered());
  CHECK(c.rows()[0].ts_ms == 1);
  CHECK(c.rows()[1].ts_ms == 5);
  CHECK(c.rows()[2].ts_ms == 9);
  CHECK(c.Contains(0) && c.Contains(9) && !c.Contains(10));
  bool threw = false;
  try {
    c.Insert(Row{10, 0.0});
  } catch (const std::out_of_range&) {
    threw = true;
  }
  CHECK(threw);
  auto sc = c.ScanRange(2, 9);
  CHECK(sc.size() == 1 && sc[0].ts_ms == 5);
  CHECK(std::fabs(c.SumRange(0, 10) - 6.0) < 1e-9);
  CHECK(c.CountRange(0, 10) == 3);
  return true;
}

bool t_routing() {
  Hypertable h(10);
  for (int i = 0; i < 30; ++i) h.Insert(i, 1.0);
  CHECK(h.NumChunks() == 3);
  CHECK(h.TotalRows() == 30);
  CHECK(h.AllChunksOrdered());
  CHECK(h.chunks().count(0) == 1);
  CHECK(h.chunks().count(10) == 1);
  CHECK(h.chunks().count(20) == 1);
  auto q = h.QueryRange(5, 15);
  CHECK(q.size() == 10);
  for (size_t i = 0; i < q.size(); ++i)
    CHECK(q[i].ts_ms == static_cast<int64_t>(5 + i));
  CHECK(std::fabs(h.SumRange(5, 15) - 10.0) < 1e-9);
  CHECK(h.CountRange(5, 15) == 10);
  CHECK(h.QueryRange(100, 200).empty());
  CHECK(h.SumRange(100, 200) == 0.0);
  return true;
}

bool t_asof() {
  std::vector<Row> ls = {{1, 0}, {10, 1}, {20, 2}, {30, 3}};
  std::vector<Row> right = {{5, 50}, {15, 150}, {15, 151}, {25, 250}};
  auto out = AsofJoin(ls, right);
  CHECK(out.size() == 4);
  CHECK(!out[0].has_right);  // 1 < 5 -> kein Match
  CHECK(out[1].has_right && out[1].right.ts_ms == 5);
  CHECK(out[2].has_right && out[2].right.ts_ms == 15 &&
        std::fabs(out[2].right.value - 151.0) < 1e-9);  // letzter Dup
  CHECK(out[3].has_right && out[3].right.ts_ms == 25);
  auto e = AsofJoin(ls, {});
  for (auto& r : e) CHECK(!r.has_right);
  return true;
}

bool t_bulk_retention_agg(double* ingest_out) {
  constexpr int64_t kN = 100000;
  constexpr int64_t kChunkMs = 10000;  // -> 10 Chunks
  constexpr int64_t kBucketMs = 1000;  // -> 100 Buckets
  constexpr int64_t kExpectedSum = (kN - 1) * kN / 2;  // value=i

  Hypertable h(kChunkMs);
  ContinuousAgg agg(kBucketMs);

  auto t0 = std::chrono::steady_clock::now();
  for (int64_t i = 0; i < kN; ++i) {
    h.Insert(i, static_cast<double>(i));
    agg.Observe(i, static_cast<double>(i));
  }
  auto t1 = std::chrono::steady_clock::now();
  double secs =
      std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0)
          .count();
  if (secs <= 0) secs = 1e-9;
  double ingest = static_cast<double>(kN) / secs;
  if (ingest_out) *ingest_out = ingest;

  std::cout << "rows=" << h.TotalRows() << " chunks=" << h.NumChunks()
            << "\n";
  CHECK(h.TotalRows() == static_cast<size_t>(kN));
  CHECK(h.NumChunks() == 10);
  CHECK(h.AllChunksOrdered());
  for (const auto& kv : h.chunks()) {
    CHECK(kv.second.Size() == 10000);
    CHECK(kv.second.IsOrdered());
  }
  std::cout << "ingest_s=" << secs << " ingest_per_s=" << ingest << "\n";

  double hsum = h.SumRange(0, kN);
  std::cout << "hsum=" << hsum << " expected=" << kExpectedSum << "\n";
  CHECK(std::fabs(hsum - static_cast<double>(kExpectedSum)) <
        1e-3 * static_cast<double>(kExpectedSum) + 1.0);
  CHECK(h.CountRange(0, kN) == static_cast<size_t>(kN));

  // Spot-Check Range [12345, 67890).
  {
    int64_t a = 12345, b = 67890;
    double exp = 0.0;
    for (int64_t i = a; i < b; ++i) exp += static_cast<double>(i);
    double got = h.SumRange(a, b);
    CHECK(std::fabs(got - exp) < 1e-3 * exp + 1.0);
    CHECK(h.CountRange(a, b) == static_cast<size_t>(b - a));
    auto rows = h.QueryRange(a, b);
    CHECK(rows.size() == static_cast<size_t>(b - a));
    CHECK(rows.front().ts_ms == a && rows.back().ts_ms == b - 1);
    for (size_t i = 1; i < rows.size(); ++i)
      CHECK(rows[i - 1].ts_ms < rows[i].ts_ms);
  }

  // ContinuousAgg korrekt (inkrementell vs. rebuild).
  CHECK(agg.NumBuckets() == 100);
  CHECK(agg.TotalCount() == static_cast<size_t>(kN));
  CHECK(std::fabs(agg.TotalSum() - static_cast<double>(kExpectedSum)) <
        1e-3 * static_cast<double>(kExpectedSum) + 1.0);
  for (int64_t b : {int64_t{0}, int64_t{7}, int64_t{42}, int64_t{99}}) {
    auto bk = agg.Bucket(b * 1000);
    CHECK(bk.count == 1000);
    double exp = 0.0;
    for (int64_t i = b * 1000; i < b * 1000 + 1000; ++i)
      exp += static_cast<double>(i);
    CHECK(std::fabs(bk.sum - exp) < 1e-3 * exp + 1.0);
    CHECK(std::fabs(bk.Avg() - exp / 1000.0) < 1e-6);
  }
  {
    ContinuousAgg rebuilt(kBucketMs);
    rebuilt.Rebuild(h);
    CHECK(rebuilt.NumBuckets() == agg.NumBuckets());
    CHECK(rebuilt.TotalCount() == agg.TotalCount());
    CHECK(std::fabs(rebuilt.TotalSum() - agg.TotalSum()) < 1.0);
    for (const auto& kv : agg.buckets()) {
      auto r = rebuilt.Bucket(kv.first);
      CHECK(r.count == kv.second.count);
      CHECK(std::fabs(r.sum - kv.second.sum) < 1e-3);
    }
    std::cout << "agg incremental==rebuild ok, buckets=" << agg.NumBuckets()
              << "\n";
  }

  // Retention-Drop.
  {
    constexpr int64_t kCutoff = 50000;  // droppt 5 Chunks [0..40k]
    size_t dropped = h.DropChunksBefore(kCutoff);
    std::cout << "dropped=" << dropped << " chunks_left=" << h.NumChunks()
              << " rows_left=" << h.TotalRows() << "\n";
    CHECK(dropped == 5);
    CHECK(h.NumChunks() == 5);
    CHECK(h.TotalRows() == 50000);
    CHECK(h.CountRange(0, kN) == 50000);
    double exp_left = 0.0;
    for (int64_t i = kCutoff; i < kN; ++i) exp_left += static_cast<double>(i);
    CHECK(std::fabs(h.SumRange(0, kN) - exp_left) < 1e-3 * exp_left + 1.0);
    auto rows = h.QueryRange(0, kN);
    CHECK(rows.size() == 50000);
    CHECK(rows.front().ts_ms == kCutoff);
    CHECK(h.QueryRange(0, kCutoff).empty());
    CHECK(h.SumRange(0, kCutoff) == 0.0);
    CHECK(h.DropChunksBefore(kCutoff) == 0);  // No-Op
    // Agg ist unabhaengig; nach Refresh halbiert.
    CHECK(agg.TotalCount() == static_cast<size_t>(kN));
    agg.Refresh(h);
    CHECK(agg.NumBuckets() == 50);
    CHECK(agg.TotalCount() == 50000);
    CHECK(std::fabs(agg.TotalSum() - exp_left) < 1e-3 * exp_left + 1.0);
    std::cout << "retention + agg-refresh ok\n";
  }
  return true;
}

}  // namespace

int main() {
  int fail = 0;
  auto run = [&](const char* name, bool (*fn)()) {
    bool ok = false;
    try {
      ok = fn();
    } catch (const std::exception& e) {
      std::cerr << "EXC " << name << ": " << e.what() << "\n";
      ok = false;
    } catch (...) {
      std::cerr << "EXC " << name << ": unknown\n";
      ok = false;
    }
    std::cout << (ok ? "PASS " : "FAIL ") << name << "\n";
    if (!ok) ++fail;
  };
  run("floor_bucket", t_floor_bucket);
  run("chunk_ordered", t_chunk_ordered_insert);
  run("routing", t_routing);
  run("asof", t_asof);
  double ingest = 0.0;
  {
    bool ok = false;
    try {
      ok = t_bulk_retention_agg(&ingest);
    } catch (const std::exception& e) {
      std::cerr << "EXC bulk: " << e.what() << "\n";
      ok = false;
    }
    std::cout << (ok ? "PASS " : "FAIL ") << "bulk_100k_retention_agg\n";
    if (!ok) ++fail;
  }
  if (fail == 0) std::cout << "chunks tests passed\n";
  return fail == 0 ? 0 : 1;
}
