// s18-bench: CLI dbbench — KV-Throughput, SQL TPC-H-like Q1, ANN Recall/Latenz.
// CSV-Report auf stdout: op,throughput,lat_p95 (throughput=ops/s, lat_p95=ms).
// Human-Readable geht nach stderr. Optional --csv PATH schreibt CSV zusaetzlich.
// Graceful degrade: jede Engine ist per __has_include guard optional; fehlt ein
// Header, wird der Bench uebersprungen (CSV-Zeile mit 0 + Hinweis auf stderr).
//
// CLI:
//   dbbench [--kv-puts N] [--sql-q1 [--sql-rows R]] [--ann N,d,k]
//           [--csv PATH] [--smoke] [--help]
//   dbbench --smoke  => klein: kv=1000, sql-rows=1000, ann=1000,16,10
//   Ohne Bench-Flags => Default: kv=10000, sql-q1 (50000 rows), ann=1000,64,10.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// ---- optionale Engine-Header (graceful degrade) -----------------------------
#if defined(__has_include)
#if __has_include("dbengine/kv.h")
#define BENCH_HAVE_KV 1
#include "dbengine/kv.h"
#endif
#if __has_include("dbengine/vector/hnsw.h")
#define BENCH_HAVE_VECTOR 1
#include "dbengine/vector/hnsw.h"
#endif
#if __has_include("dbengine/columnar/store.h")
#define BENCH_HAVE_COLUMNAR 1
#include "dbengine/columnar/store.h"
#endif
#else
// Compiler ohne __has_include: alles voraussetzen.
#define BENCH_HAVE_KV 1
#define BENCH_HAVE_VECTOR 1
#define BENCH_HAVE_COLUMNAR 1
#include "dbengine/columnar/store.h"
#include "dbengine/kv.h"
#include "dbengine/vector/hnsw.h"
#endif

namespace {

struct BenchRow {
  std::string op;
  double throughput = 0.0;  // ops/s (kv: puts/s, sql: rows/s, ann: queries/s)
  double lat_p95_ms = 0.0;  // ms
};

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  size_t idx = static_cast<size_t>(p * (v.size() - 1));
  return v[idx];
}

long long ParseLong(const std::string& s, bool& ok) {
  try {
    size_t pos = 0;
    long long v = std::stoll(s, &pos, 10);
    ok = (pos == s.size());
    return v;
  } catch (...) {
    ok = false;
    return 0;
  }
}

// "N,d,k" -> triple. ok=false bei Formatfehler.
bool ParseAnnSpec(const std::string& s, long long& n, long long& d,
                  long long& k) {
  std::string t = s;
  for (char& c : t)
    if (c == ',') c = ' ';
  std::istringstream in(t);
  bool ok = false;
  long long nn = 0, dd = 0, kk = 0;
  if ((in >> nn >> dd >> kk) && in.eof()) {
    n = nn;
    d = dd;
    k = kk;
    ok = (n > 0 && d > 0 && k > 0);
    (void)ok;
    return ok;
  }
  return false;
}

struct Config {
  bool do_kv = false;
  long long kv_puts = 0;
  bool do_sql = false;
  long long sql_rows = 50000;
  bool do_ann = false;
  long long ann_n = 0, ann_d = 0, ann_k = 0;
  bool smoke = false;
  std::string csv_path;
  bool help = false;
  std::string error;
};

Config ParseArgs(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need_val = [&](std::string& out) -> bool {
      if (i + 1 >= argc) {
        c.error = "missing value for " + a;
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (a == "--help" || a == "-h") {
      c.help = true;
    } else if (a == "--smoke") {
      c.smoke = true;
    } else if (a == "--kv-puts") {
      std::string v;
      if (!need_val(v)) return c;
      bool ok = false;
      c.kv_puts = ParseLong(v, ok);
      if (!ok || c.kv_puts <= 0) {
        c.error = "invalid --kv-puts value: " + v;
        return c;
      }
      c.do_kv = true;
    } else if (a == "--sql-q1") {
      c.do_sql = true;
    } else if (a == "--sql-rows") {
      std::string v;
      if (!need_val(v)) return c;
      bool ok = false;
      c.sql_rows = ParseLong(v, ok);
      if (!ok || c.sql_rows <= 0) {
        c.error = "invalid --sql-rows value: " + v;
        return c;
      }
      c.do_sql = true;
    } else if (a == "--ann") {
      std::string v;
      if (!need_val(v)) return c;
      if (!ParseAnnSpec(v, c.ann_n, c.ann_d, c.ann_k)) {
        c.error = "invalid --ann spec (want N,d,k): " + v;
        return c;
      }
      c.do_ann = true;
    } else if (a == "--csv") {
      if (!need_val(c.csv_path)) return c;
    } else {
      c.error = "unknown arg: " + a;
      return c;
    }
  }
  if (c.smoke) {
    c.do_kv = true;
    c.kv_puts = 1000;
    c.do_sql = true;
    c.sql_rows = 1000;
    c.do_ann = true;
    c.ann_n = 1000;
    c.ann_d = 16;
    c.ann_k = 10;
  }
  if (!c.do_kv && !c.do_sql && !c.do_ann && c.error.empty() && !c.help) {
    // Default: alles, mittlere Groesse.
    c.do_kv = true;
    c.kv_puts = 10000;
    c.do_sql = true;
    c.sql_rows = 50000;
    c.do_ann = true;
    c.ann_n = 1000;
    c.ann_d = 64;
    c.ann_k = 10;
  }
  return c;
}

void Usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s [--kv-puts N] [--sql-q1 [--sql-rows R]] "
               "[--ann N,d,k] [--csv PATH] [--smoke]\n",
               prog);
}

// ---- KV-Bench ---------------------------------------------------------------
BenchRow RunKvPuts(long long n) {
  BenchRow row{"kv_puts", 0.0, 0.0};
#ifdef BENCH_HAVE_KV
  dbengine::kv::KVStore kv;
  std::vector<double> lat_ms;
  lat_ms.reserve(static_cast<size_t>(n));
  char kbuf[64], vbuf[64];
  auto t0 = std::chrono::steady_clock::now();
  for (long long i = 0; i < n; ++i) {
    std::snprintf(kbuf, sizeof(kbuf), "bench:%08lld", i);
    std::snprintf(vbuf, sizeof(vbuf), "v%08lld", i);
    auto s = std::chrono::steady_clock::now();
    kv.Put(kbuf, vbuf);
    auto e = std::chrono::steady_clock::now();
    lat_ms.push_back(
        std::chrono::duration<double, std::milli>(e - s).count());
  }
  auto t1 = std::chrono::steady_clock::now();
  double secs =
      std::chrono::duration<double>(t1 - t0).count();
  if (secs <= 0) secs = 1e-9;
  row.throughput = n / secs;
  row.lat_p95_ms = Percentile(lat_ms, 0.95);
  std::fprintf(stderr, "kv_puts: N=%lld size=%zu throughput=%.1f ops/s p95=%.4fms\n",
               n, kv.Size(), row.throughput, row.lat_p95_ms);
#else
  std::fprintf(stderr, "kv_puts: SKIPPED (dbengine/kv.h missing)\n");
#endif
  return row;
}

// ---- SQL Q1 (TPC-H-like SUM via ColumnarStore) -------------------------------
// Q1-Analogie: SUM(l_extendedprice) WHERE l_extendedprice < threshold.
BenchRow RunSqlQ1(long long rows) {
  BenchRow row{"sql_q1", 0.0, 0.0};
#ifdef BENCH_HAVE_COLUMNAR
  dbengine::columnar::ColumnarStore store;
  for (long long i = 0; i < rows; ++i) store.Append(i, "part");
  store.SealActive();
  const int64_t threshold = rows / 2;
  // Erwartungswert: Summe 0..threshold-1.
  const int64_t expected = (threshold - 1) * threshold / 2;
  constexpr int kReps = 5;
  std::vector<double> lat_ms;
  lat_ms.reserve(kReps);
  int64_t sum = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (int r = 0; r < kReps; ++r) {
    auto s = std::chrono::steady_clock::now();
    auto res = store.ScanSumLessThan(threshold);
    auto e = std::chrono::steady_clock::now();
    sum = res.sum;
    lat_ms.push_back(
        std::chrono::duration<double, std::milli>(e - s).count());
  }
  auto t1 = std::chrono::steady_clock::now();
  double secs = std::chrono::duration<double>(t1 - t0).count();
  if (secs <= 0) secs = 1e-9;
  if (sum != expected) {
    std::fprintf(stderr, "sql_q1: WRONG SUM got=%lld want=%lld\n",
                 (long long)sum, (long long)expected);
  }
  row.throughput = (rows * kReps) / secs;  // rows/s
  row.lat_p95_ms = Percentile(lat_ms, 0.95);
  std::fprintf(stderr,
               "sql_q1: rows=%lld sum=%lld throughput=%.1f rows/s p95=%.4fms/scan\n",
               rows, (long long)sum, row.throughput, row.lat_p95_ms);
#else
  std::fprintf(stderr, "sql_q1: SKIPPED (dbengine/columnar/store.h missing)\n");
#endif
  return row;
}

// ---- ANN-Bench (HNSW-lite vs Brute-Force Recall) ------------------------------
BenchRow RunAnn(long long n, long long dim, long long k) {
  BenchRow row{"ann_search", 0.0, 0.0};
#ifdef BENCH_HAVE_VECTOR
  using dbengine::vector::DistanceMetric;
  using dbengine::vector::HnswIndex;
  using dbengine::vector::Vector;
  std::mt19937 rng(42u);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  HnswIndex idx(static_cast<int>(dim), 16, 32, DistanceMetric::L2);
  for (long long i = 0; i < n; ++i) {
    Vector v(static_cast<size_t>(dim));
    for (long long d = 0; d < dim; ++d) v[static_cast<size_t>(d)] = u(rng);
    idx.add(v);
  }
  idx.build();
  // Frische Queries.
  std::mt19937 qrng(1234u);
  long long nq = std::min<long long>(n, 100);
  if (nq <= 0) nq = 1;
  std::vector<Vector> queries;
  queries.reserve(static_cast<size_t>(nq));
  for (long long i = 0; i < nq; ++i) {
    Vector v(static_cast<size_t>(dim));
    for (long long d = 0; d < dim; ++d) v[static_cast<size_t>(d)] = u(qrng);
    queries.push_back(std::move(v));
  }
  int ef = std::max(32, static_cast<int>(2 * k));
  std::vector<double> lat_ms;
  lat_ms.reserve(static_cast<size_t>(nq));
  auto t0 = std::chrono::steady_clock::now();
  for (auto& q : queries) {
    auto s = std::chrono::steady_clock::now();
    volatile auto hits = idx.search(q, static_cast<int>(k), ef);
    auto e = std::chrono::steady_clock::now();
    lat_ms.push_back(
        std::chrono::duration<double, std::milli>(e - s).count());
  }
  auto t1 = std::chrono::steady_clock::now();
  double secs = std::chrono::duration<double>(t1 - t0).count();
  if (secs <= 0) secs = 1e-9;
  row.throughput = nq / secs;  // queries/s
  row.lat_p95_ms = Percentile(lat_ms, 0.95);
  // Recall auf Teilmenge vs Brute-Force.
  long long nr = std::min<long long>(nq, 20);
  double sum_r = 0.0;
  for (long long i = 0; i < nr; ++i) {
    auto exact = idx.brute_force(queries[static_cast<size_t>(i)],
                                 static_cast<int>(k));
    auto approx = idx.search(queries[static_cast<size_t>(i)],
                             static_cast<int>(k), ef);
    std::vector<char> seen(static_cast<size_t>(n), 0);
    for (auto& h : exact)
      if (h.id >= 0 && h.id < n) seen[static_cast<size_t>(h.id)] = 1;
    long long hits = 0;
    for (auto& h : approx)
      if (h.id >= 0 && h.id < n && seen[static_cast<size_t>(h.id)]) ++hits;
    sum_r += static_cast<double>(hits) / static_cast<double>(k);
  }
  double recall = nr > 0 ? sum_r / nr : 0.0;
  std::fprintf(stderr,
               "ann_search: N=%lld d=%lld k=%lld recall@%lld=%.4f "
               "throughput=%.1f q/s p95=%.4fms\n",
               n, dim, k, k, recall, row.throughput, row.lat_p95_ms);
#else
  std::fprintf(stderr, "ann_search: SKIPPED (dbengine/vector/hnsw.h missing)\n");
#endif
  return row;
}

void EmitCsv(const std::vector<BenchRow>& rows, std::ostream& out) {
  out << "op,throughput,lat_p95\n";
  for (auto& r : rows) out << r.op << "," << r.throughput << "," << r.lat_p95_ms << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Config c = ParseArgs(argc, argv);
  if (!c.error.empty()) {
    std::fprintf(stderr, "dbbench: %s\n", c.error.c_str());
    Usage(argv[0]);
    return 2;
  }
  if (c.help) {
    Usage(argv[0]);
    return 0;
  }
  std::vector<BenchRow> rows;
  if (c.do_kv) rows.push_back(RunKvPuts(c.kv_puts));
  if (c.do_sql) rows.push_back(RunSqlQ1(c.sql_rows));
  if (c.do_ann) rows.push_back(RunAnn(c.ann_n, c.ann_d, c.ann_k));

  EmitCsv(rows, std::cout);
  if (!c.csv_path.empty()) {
    std::ofstream f(c.csv_path);
    if (!f) {
      std::fprintf(stderr, "dbbench: cannot write csv %s\n", c.csv_path.c_str());
      return 1;
    }
    EmitCsv(rows, f);
    std::fprintf(stderr, "dbbench: csv -> %s\n", c.csv_path.c_str());
  }
  return 0;
}
