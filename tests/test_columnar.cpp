// s10-columnar Tests: 100k ints, seal, Scan mit Filter + TPC-H-like SUM.
// Stil: plain assert + ctest (wie test_smoke.cpp), kein gtest.
// Misst grob Scan-GB/s (wall-clock, single-threaded).

#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "dbengine/columnar/store.h"

using dbengine::columnar::ColumnarStore;
using dbengine::columnar::DecodeRle;
using dbengine::columnar::EncodeRle;
using dbengine::columnar::Part;

int main() {
  // 1) RLE Roundtrip (Encoding-Stub).
  {
    std::vector<int64_t> v = {1, 1, 1, 2, 3, 3, 1};
    auto runs = EncodeRle(v);
    assert(runs.size() == 4);
    assert(runs[0].value == 1 && runs[0].count == 3);
    assert(DecodeRle(runs) == v);
    assert(EncodeRle({}).empty());
    // Run-Fall: komprimiert stark.
    std::vector<int64_t> flat(10000, 7);
    auto r2 = EncodeRle(flat);
    assert(r2.size() == 1 && r2[0].count == 10000);
    assert(DecodeRle(r2) == flat);
  }

  // 2) Immutable Part: seal() macht read-only.
  {
    Part p(0, "imm");
    p.Append(10, "a");
    p.Append(20, "b");
    assert(p.Min() == 10 && p.Max() == 20);
    p.Seal();
    assert(p.sealed());
    bool threw = false;
    try {
      p.Append(30, "c");
    } catch (const std::logic_error&) {
      threw = true;
    }
    assert(threw && "append nach seal() muss werfen");
    assert(p.size() == 2);
    // Dict-Stub.
    assert(p.strs().DictSize() == 2);
    assert(p.strs().At(0) == "a");
  }

  // 3) Haupt-Test: 100k ints, seal, Scan mit Filter + TPC-H-like SUM.
  //    TPC-H Q1/Q6-Analogie: SUM(l_extendedprice) WHERE l_extendedprice < X.
  constexpr int64_t kN = 100000;
  constexpr int64_t kThreshold = 50000;
  // Erwartung: Werte 0..99999, Summe ueber [0,49999).
  constexpr int64_t kExpected = 49999LL * 50000LL / 2LL;  // 1249975000
  ColumnarStore store;
  for (int64_t i = 0; i < kN; ++i) {
    // Geringe String-Kardinalitaet -> Dict-Stub greift.
    store.Append(i, "part-" + std::to_string(i % 8));
  }
  assert(store.TotalRows() == static_cast<size_t>(kN));
  store.SealActive();
  assert(store.NumSealedParts() == 1);
  assert(store.ActiveSize() == 0);

  auto t0 = std::chrono::steady_clock::now();
  auto res = store.ScanSumLessThan(kThreshold);
  auto t1 = std::chrono::steady_clock::now();
  double secs =
      std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0)
          .count();
  if (secs <= 0) secs = 1e-9;

  std::cout << "sum=" << res.sum << " expected=" << kExpected << "\n";
  std::cout << "parts_total=" << res.parts_total
            << " pruned=" << res.parts_pruned << " full=" << res.parts_full
            << " rows_scanned=" << res.rows_scanned << "\n";
  assert(res.sum == kExpected);

  // Grobe Scan-Bandbreite: int-Spalte (8B/Zeile) / Wall-Clock.
  // Hinweis: single-threaded, O2, inkl. Branch -- grobe Hausnummer.
  double bytes = static_cast<double>(kN) * sizeof(int64_t);
  double gbs = bytes / secs / 1e9;
  std::cout << "scan_time_s=" << secs << " scan_gbs_int_col=" << gbs << "\n";
  std::cout << "note: single-thread grob, int-col only, 100k rows\n";

  // 4) Pushdown-Faelle: voll geprunt / voll getroffen.
  {
    // threshold <= min -> alles geprunt.
    auto r0 = store.ScanSumLessThan(0);
    assert(r0.sum == 0);
    assert(r0.parts_pruned >= 1);
    // threshold > max -> full-part Pfad (kein Zeilenvergleich).
    auto r1 = store.ScanSumLessThan(kN);
    int64_t full_expected = (kN - 1) * kN / 2;
    assert(r1.sum == full_expected);
    assert(r1.parts_full >= 1);
    assert(r1.rows_scanned == 0);
  }

  // 5) Multi-Part Pruning: 3 Parts mit disjunkten Ranges.
  {
    ColumnarStore s;
    for (int64_t i = 0; i < 30000; ++i) s.Append(i, "a");
    s.SealActive();
    for (int64_t i = 30000; i < 60000; ++i) s.Append(i, "b");
    s.SealActive();
    for (int64_t i = 60000; i < 100000; ++i) s.Append(i, "c");
    s.SealActive();
    assert(s.NumSealedParts() == 3);
    auto r = s.ScanSumLessThan(35000);
    // Part0 voll (max 29999 < 35000), Part1 partiell, Part2 geprunt.
    int64_t e0 = 29999LL * 30000LL / 2LL;
    int64_t e1 = 0;
    for (int64_t i = 30000; i < 35000; ++i) e1 += i;
    assert(r.sum == e0 + e1);
    assert(r.parts_pruned == 1);
    assert(r.parts_full == 1);
    std::cout << "multi-part prune ok: sum=" << r.sum
              << " pruned=" << r.parts_pruned << " full=" << r.parts_full
              << "\n";
  }

  // 6) Arrow-Export-Stub (CSV + Binaer) + Roundtrip.
  {
    const std::string csv = "/tmp/col_s10_test.csv";
    const std::string bin = "/tmp/col_s10_test.arw1";
    assert(store.ExportCsv(csv));
    assert(store.ExportBinary(bin));
    std::vector<int64_t> ints;
    std::vector<std::string> strs;
    assert(Part::DecodeBinary(bin, &ints, &strs));
    assert(ints.size() == static_cast<size_t>(kN));
    assert(strs.size() == static_cast<size_t>(kN));
    assert(ints[0] == 0 && ints[kN - 1] == kN - 1);
    assert(strs[0] == "part-0");
    // S3-Tier Stub.
    std::string uri = store.StageToS3("my-bucket", "tier1");
    assert(uri == "s3://my-bucket/tier1/columnar-1parts/");
    std::cout << "export ok: " << csv << " " << bin << " uri=" << uri << "\n";
    std::remove(csv.c_str());
    std::remove(bin.c_str());
  }

  std::cout << "columnar tests passed\n";
  return 0;
}
