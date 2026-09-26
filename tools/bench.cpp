// s18-bench: CLI dbbench — KV-Throughput, SQL TPC-H-like Q1, ANN Recall/Latenz.
// CSV-Report auf stdout: op,throughput,lat_p95 (throughput=ops/s, lat_p95=ms).
// Human-Readable geht nach stderr. Optional --csv PATH schreibt CSV zusaetzlich.
// Graceful degrade: jede Engine ist per __has_include guard optional; fehlt ein
// Header, wird der Bench uebersprungen (CSV-Zeile mit 0 + Hinweis auf stderr).
// WAL-Durability: --data-dir DIR misst sustained append-Throughput (ops/s) +
// p50/p95-flush-Latenz via Wal::append_many + flush in Batches auf DIR/dbbench.wal.
// CSV-Zeilen: wal_durable_puts + wal_flush_p95. Ohne --data-dir: exakt wie bisher.
//
// CLI:
//   dbbench [--kv-puts N] [--sql-q1 [--sql-rows R]] [--ann N,d,k]
//           [--csv PATH] [--data-dir DIR] [--tpch [N]] [--smoke] [--help]
//   dbbench --smoke  => klein: kv=1000, sql-rows=1000, ann=1000,16,10
//   dbbench --data-dir DIR => nur WAL-Bench auf DIR (mit anderen Flags kombinierbar)
//   dbbench --tpch [N] => TPC-H-like lineitem (Default 10000, Seed 42) ueber
//     echten Executor (KV+MVCC): Q6 + Q1-Kern, CSV-Zeilen tpch_q1/tpch_q6.
//   Ohne Bench-Flags => Default: kv=10000, sql-q1 (50000 rows), ann=1000,64,10.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <variant>
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
#if __has_include("dbengine/storage/wal.h")
#define BENCH_HAVE_WAL 1
#include "dbengine/storage/wal.h"
#endif
#if __has_include("dbengine/sql/executor.h") && \
    __has_include("dbengine/txn/mvcc.h")
#define BENCH_HAVE_TPCH 1
#include "dbengine/sql/executor.h"
#include "dbengine/txn/mvcc.h"
#endif
#else
// Compiler ohne __has_include: alles voraussetzen.
#define BENCH_HAVE_KV 1
#define BENCH_HAVE_VECTOR 1
#define BENCH_HAVE_COLUMNAR 1
#define BENCH_HAVE_WAL 1
#define BENCH_HAVE_TPCH 1
#include "dbengine/columnar/store.h"
#include "dbengine/kv.h"
#include "dbengine/sql/executor.h"
#include "dbengine/storage/wal.h"
#include "dbengine/txn/mvcc.h"
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
  bool do_tpch = false;
  long long tpch_rows = 10000;
  bool smoke = false;
  std::string csv_path;
  std::string data_dir;
  bool has_data_dir = false;
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
    } else if (a == "--tpch" || a.rfind("--tpch=", 0) == 0) {
      long long v = 10000;
      if (a.rfind("--tpch=", 0) == 0) {
        std::string vs = a.substr(8);
        bool ok = false;
        v = ParseLong(vs, ok);
        if (!ok || v <= 0) {
          c.error = "invalid --tpch value: " + vs;
          return c;
        }
      } else if (i + 1 < argc && argv[i + 1][0] != '\0' &&
                 argv[i + 1][0] != '-') {
        // Optionales Positions-Argument: nur konsumieren wenn positiv numerisch,
        // sonst Fehler (kein stilles Ignorieren von Tippfehlern).
        bool ok = false;
        long long vv = ParseLong(argv[i + 1], ok);
        if (!ok || vv <= 0) {
          c.error = std::string("invalid --tpch value: ") + argv[i + 1];
          return c;
        }
        v = vv;
        ++i;
      }
      c.do_tpch = true;
      c.tpch_rows = v;
    } else if (a == "--csv") {
      if (!need_val(c.csv_path)) return c;
    } else if (a == "--data-dir") {
      std::string v;
      if (!need_val(v)) return c;
      if (v.empty()) {
        c.error = "invalid --data-dir value: empty";
        return c;
      }
      c.data_dir = v;
      c.has_data_dir = true;
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
  if (!c.do_kv && !c.do_sql && !c.do_ann && !c.do_tpch && !c.has_data_dir && c.error.empty() && !c.help) {
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
               "[--ann N,d,k] [--csv PATH] [--data-dir DIR] [--tpch [N]] "
               "[--smoke]\n",
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

// ---- TPC-H-like Bench ueber echten Executor (KV+MVCC-Pfad) ------------------
// Deterministische lineitem-Tabelle (Seed 42), Muster wie RunSqlQ1:
// einmal aufbauen (batched INSERTs), dann je Query kReps Scans messen.
//   Q6: SELECT SUM(price*disc) WHERE <5x AND-Range> (skalares Aggregat).
//   Q1-Kern: SELECT rf, ls, SUM/AVG/COUNT ... GROUP BY rf, ls (Hash-Agg).
// CSV: tpch_q1 + tpch_q6 (rows/s, p95 ms/scan). Details (Summe/Ref/Hash)
// human-readable nach stderr (stdout bleibt reines CSV fuer test_bench).
#ifdef BENCH_HAVE_TPCH
std::uint64_t Fnv1a64(const std::string& s,
                      std::uint64_t h = 1469598103934665603ULL) {
  for (unsigned char ch : s) {
    h ^= static_cast<std::uint64_t>(ch);
    h *= 1099511628211ULL;
  }
  return h;
}

double TpchValueToDouble(const dbengine::sql::Value& v, bool& ok) {
  if (auto* d = std::get_if<double>(&v)) {
    ok = true;
    return *d;
  }
  if (auto* n = std::get_if<int64_t>(&v)) {
    ok = true;
    return static_cast<double>(*n);
  }
  ok = false;
  return 0.0;
}

std::vector<BenchRow> RunTpch(long long rows) {
  BenchRow q1{"tpch_q1", 0.0, 0.0};
  BenchRow q6{"tpch_q6", 0.0, 0.0};
  try {
    dbengine::kv::KVStore kv;
    dbengine::txn::MvccStore mvcc;
    dbengine::sql::Executor ex(kv, mvcc, nullptr);
    ex.execute(
        "CREATE TABLE lineitem (orderkey INT, qty INT, price DOUBLE, "
        "disc DOUBLE, tax DOUBLE, rf TEXT, ls TEXT, shipdate INT)");

    // Deterministische Generierung (Seed 42). Spannen so gewaehlt, dass die
    // Q6-Praedikate unten selektiv, aber nicht leer sind.
    std::mt19937 rng(42u);
    std::uniform_int_distribution<int> qty_d(1, 50);
    std::uniform_int_distribution<int> price_cents_d(1000, 1000000);
    std::uniform_int_distribution<int> disc_pct_d(0, 10);
    std::uniform_int_distribution<int> tax_pct_d(0, 8);
    std::uniform_int_distribution<int> rf_d(0, 2);
    std::uniform_int_distribution<int> ls_d(0, 1);
    std::uniform_int_distribution<int> yr_d(1992, 1998);
    std::uniform_int_distribution<int> mo_d(1, 12);
    std::uniform_int_distribution<int> da_d(1, 28);
    const char* const rfs[3] = {"A", "N", "R"};
    const char* const lss[2] = {"O", "F"};

    // Referenz-Summe fuer Q6 (identische Praedikate wie q6_sql unten).
    double ref_q6 = 0.0;
    const long long kBatch = 500;
    for (long long base = 0; base < rows; base += kBatch) {
      long long cur = std::min(kBatch, rows - base);
      std::string sql = "INSERT INTO lineitem VALUES ";
      sql.reserve(static_cast<size_t>(cur * 64 + 32));
      for (long long i = 0; i < cur; ++i) {
        long long idx = base + i;
        long long orderkey = idx + 1;
        long long qty = static_cast<long long>(qty_d(rng));
        double price = static_cast<double>(price_cents_d(rng)) / 100.0;
        double disc = static_cast<double>(disc_pct_d(rng)) / 100.0;
        double tax = static_cast<double>(tax_pct_d(rng)) / 100.0;
        const char* rf = rfs[rf_d(rng)];
        const char* ls = lss[ls_d(rng)];
        int shipdate = yr_d(rng) * 10000 + mo_d(rng) * 100 + da_d(rng);
        bool pass = (disc >= 0.05 && disc <= 0.07) && (qty < 24) &&
                    (price >= 500.0) && (price < 5000.0) && (tax <= 0.05) &&
                    (shipdate >= 19940101 && shipdate <= 19951231);
        if (pass) ref_q6 += price * disc;
        char row[128];
        std::snprintf(row, sizeof(row),
                      "%s(%lld,%lld,%.2f,%.2f,%.2f,'%s','%s',%d)",
                      (i == 0 ? "" : ","), orderkey, qty, price, disc, tax,
                      rf, ls, shipdate);
        sql += row;
      }
      ex.execute(sql);
    }

    constexpr int kReps = 5;
    // Q6: 5x AND (6 Range-Praedikate).
    const std::string q6_sql =
        "SELECT SUM(price*disc) FROM lineitem WHERE disc BETWEEN 0.05 AND "
        "0.07 AND qty < 24 AND price >= 500.0 AND price < 5000.0 AND tax <= "
        "0.05 AND shipdate BETWEEN 19940101 AND 19951231";
    {
      std::vector<double> lat_ms;
      lat_ms.reserve(kReps);
      double sum = 0.0;
      auto t0 = std::chrono::steady_clock::now();
      for (int r = 0; r < kReps; ++r) {
        auto s = std::chrono::steady_clock::now();
        dbengine::sql::Result res = ex.execute(q6_sql);
        auto e = std::chrono::steady_clock::now();
        lat_ms.push_back(
            std::chrono::duration<double, std::milli>(e - s).count());
        if (!res.rows.empty() && !res.rows[0].empty() &&
            !dbengine::sql::valueIsNull(res.rows[0][0])) {
          bool ok = false;
          sum = TpchValueToDouble(res.rows[0][0], ok);
          if (!ok) sum = 0.0;
        } else {
          sum = 0.0;  // keine Treffer -> SUM NULL/0
        }
      }
      auto t1 = std::chrono::steady_clock::now();
      double secs = std::chrono::duration<double>(t1 - t0).count();
      if (secs <= 0) secs = 1e-9;
      q6.throughput = (static_cast<double>(rows) * kReps) / secs;
      q6.lat_p95_ms = Percentile(lat_ms, 0.95);
      double denom = std::max(1.0, std::fabs(ref_q6));
      double rel = std::fabs(sum - ref_q6) / denom;
      std::uint64_t h = Fnv1a64(std::to_string(sum));
      if (rel > 1e-9) {
        std::fprintf(stderr, "tpch_q6: WRONG SUM got=%.6f want=%.6f rel=%.3g\n",
                     sum, ref_q6, rel);
      }
      std::fprintf(stderr,
                   "tpch_q6: rows=%lld sum=%.6f ref=%.6f hash=%016llx "
                   "throughput=%.1f rows/s p95=%.4fms/scan\n",
                   rows, sum, ref_q6, (unsigned long long)h, q6.throughput,
                   q6.lat_p95_ms);
    }

    // Q1-Kern: GROUP BY rf, ls + SUM/AVG/COUNT.
    const std::string q1_sql =
        "SELECT rf, ls, SUM(qty), SUM(price), SUM(price*disc), AVG(disc), "
        "COUNT(*) FROM lineitem GROUP BY rf, ls ORDER BY rf, ls";
    {
      std::vector<double> lat_ms;
      lat_ms.reserve(kReps);
      std::uint64_t h = 0;
      long long groups = 0;
      long long counted = 0;
      auto t0 = std::chrono::steady_clock::now();
      for (int r = 0; r < kReps; ++r) {
        auto s = std::chrono::steady_clock::now();
        dbengine::sql::Result res = ex.execute(q1_sql);
        auto e = std::chrono::steady_clock::now();
        lat_ms.push_back(
            std::chrono::duration<double, std::milli>(e - s).count());
        // Kanonischer Hash: Zeilen sortieren (ORDER BY macht es stabil,
        // Sortierung hier macht den Check robust gegen Plan-Aenderungen).
        std::vector<std::string> keys;
        keys.reserve(res.rows.size());
        for (auto& row : res.rows) {
          std::string k;
          for (size_t c = 0; c < row.size(); ++c) {
            if (c) k += '|';
            k += dbengine::sql::valueToString(row[c]);
          }
          keys.push_back(std::move(k));
        }
        std::sort(keys.begin(), keys.end());
        std::string cat;
        for (auto& k : keys) {
          cat += k;
          cat += ';';
        }
        h = Fnv1a64(cat);
        groups = static_cast<long long>(res.rows.size());
        counted = 0;
        for (auto& row : res.rows) {
          if (row.size() >= 7 &&
              !dbengine::sql::valueIsNull(row[6])) {
            bool ok = false;
            double c = TpchValueToDouble(row[6], ok);
            if (ok) counted += static_cast<long long>(c);
          }
        }
      }
      auto t1 = std::chrono::steady_clock::now();
      double secs = std::chrono::duration<double>(t1 - t0).count();
      if (secs <= 0) secs = 1e-9;
      q1.throughput = (static_cast<double>(rows) * kReps) / secs;
      q1.lat_p95_ms = Percentile(lat_ms, 0.95);
      if (groups <= 0 || groups > 6 || counted != rows) {
        std::fprintf(stderr,
                     "tpch_q1: CHECK groups=%lld counted=%lld want_rows=%lld\n",
                     groups, counted, rows);
      }
      std::fprintf(stderr,
                   "tpch_q1: rows=%lld groups=%lld counted=%lld hash=%016llx "
                   "throughput=%.1f rows/s p95=%.4fms/scan\n",
                   rows, groups, counted, (unsigned long long)h,
                   q1.throughput, q1.lat_p95_ms);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "tpch: FAILED %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "tpch: FAILED unknown error\n");
  }
  return std::vector<BenchRow>{q1, q6};
}
#else
std::vector<BenchRow> RunTpch(long long) {
  std::fprintf(stderr, "tpch: SKIPPED (dbengine/sql/executor.h missing)\n");
  return std::vector<BenchRow>{{"tpch_q1", 0.0, 0.0},
                               {"tpch_q6", 0.0, 0.0}};
}
#endif

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

// ---- WAL-Durability-Bench (append_many + flush in Batches auf <dir>) ---------
// Misst sustained append-Throughput (ops/s) + p50/p95-flush-Latenz (ms).
// CSV: wal_durable_puts (ops/s, p95) + wal_flush_p95 (flushes/s, p95);
// p50 geht human-readable nach stderr. Fehler -> err gesetzt, leere Rows,
// kein Throw ausserhalb (main gibt nonzero exit zurueck).
#ifdef BENCH_HAVE_WAL
std::vector<BenchRow> RunWalDurable(const std::string& dir, std::string& err) {
  std::vector<BenchRow> out;
  err.clear();
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::file_status st = fs::status(dir, ec);
  if (ec) {
    err = "data-dir cannot stat '" + dir + "': " + ec.message();
    return out;
  }
  if (!fs::is_directory(st)) {
    err = "data-dir is not a directory: " + dir;
    return out;
  }
  // Schreibprobe (faengt unbeschreibbares Dir ohne Crash ab).
  {
    fs::path probe = fs::path(dir) / ".dbbench_probe.tmp";
    std::ofstream pf(probe, std::ios::out | std::ios::trunc);
    if (!pf) {
      err = "data-dir not writable: " + dir;
      return out;
    }
    pf << "probe\n";
    pf.flush();
    if (!pf) {
      err = "data-dir not writable: " + dir;
      return out;
    }
  }
  (void)fs::remove(fs::path(dir) / ".dbbench_probe.tmp", ec);
  ec.clear();

  fs::path wal_path = fs::path(dir) / "dbbench.wal";
  (void)fs::remove(wal_path, ec);  // sauberer Start (best effort)
  ec.clear();
  try {
    dbengine::storage::Wal wal(wal_path.string());
    wal.open();
    const long long kTotal = 5000;
    const long long kBatch = 50;
    std::vector<double> flush_ms;
    flush_ms.reserve(static_cast<size_t>((kTotal + kBatch - 1) / kBatch));
    auto t0 = std::chrono::steady_clock::now();
    for (long long base = 0; base < kTotal; base += kBatch) {
      long long cur =
          std::min(kBatch, kTotal - base);
      std::vector<std::string> batch;
      batch.reserve(static_cast<size_t>(cur));
      for (long long i = 0; i < cur; ++i) {
        char tmp[96];
        std::snprintf(tmp, sizeof(tmp), "wal-bench:%08lld:",
                      base + i);
        std::string s(tmp);
        if (s.size() < 128) s.append(128 - s.size(), 'x');
        batch.push_back(std::move(s));
      }
      wal.append_many(batch);
      auto fs0 = std::chrono::steady_clock::now();
      wal.flush();
      auto fs1 = std::chrono::steady_clock::now();
      flush_ms.push_back(
          std::chrono::duration<double, std::milli>(fs1 - fs0).count());
    }
    auto t1 = std::chrono::steady_clock::now();
    wal.close();
    (void)fs::remove(wal_path, ec);  // Aufräumen (best effort)
    double secs = std::chrono::duration<double>(t1 - t0).count();
    if (secs <= 0) secs = 1e-9;
    double throughput =
        static_cast<double>(kTotal) / secs;  // durable ops/s
    double flush_per_sec =
        static_cast<double>(flush_ms.size()) / secs;
    double p50 = Percentile(flush_ms, 0.50);
    double p95 = Percentile(flush_ms, 0.95);
    std::fprintf(stderr,
                 "wal_durable_puts: N=%lld batch=%lld file=%s "
                 "throughput=%.1f ops/s flush_p50=%.4fms flush_p95=%.4fms "
                 "flushes=%.1f/s\n",
                 kTotal, kBatch, wal_path.c_str(), throughput, p50, p95,
                 flush_per_sec);
    out.push_back(BenchRow{"wal_durable_puts", throughput, p95});
    out.push_back(BenchRow{"wal_flush_p95", flush_per_sec, p95});
  } catch (const std::exception& e) {
    err = std::string("wal bench failed: ") + e.what();
    (void)fs::remove(wal_path, ec);
    out.clear();
  } catch (...) {
    err = "wal bench failed: unknown error";
    (void)fs::remove(wal_path, ec);
    out.clear();
  }
  return out;
}
#endif

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
  if (c.do_tpch) {
    std::vector<BenchRow> tpch_rows = RunTpch(c.tpch_rows);
    rows.insert(rows.end(), tpch_rows.begin(), tpch_rows.end());
  }
  if (c.has_data_dir) {
#ifdef BENCH_HAVE_WAL
    std::string wal_err;
    std::vector<BenchRow> wal_rows = RunWalDurable(c.data_dir, wal_err);
    if (!wal_err.empty()) {
      std::fprintf(stderr, "dbbench: %s\n", wal_err.c_str());
      return 1;
    }
    rows.insert(rows.end(), wal_rows.begin(), wal_rows.end());
#else
    std::fprintf(stderr,
                 "dbbench: --data-dir unsupported "
                 "(dbengine/storage/wal.h missing)\n");
    return 1;
#endif
  }

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
