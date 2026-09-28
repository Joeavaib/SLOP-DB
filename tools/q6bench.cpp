// s104-q6bench: Q6-Microbench (100k lineitem, deterministisch Seed 42).
// STL-only (+ dbengine Executor). Misst nur Q6-Scan (SUM(price*disc) mit
// 5x AND-Range), prueft Summe gegen Referenz (identische Praedikate) und
// druckt Throughput/p95. Keine Optimierung hier — Baseline fuer s105.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "dbengine/kv.h"
#include "dbengine/sql/executor.h"
#include "dbengine/txn/mvcc.h"

namespace {
double ValToDouble(const dbengine::sql::Value& v, bool& ok) {
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
double P95(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  std::size_t i = static_cast<std::size_t>(std::ceil(0.95 * v.size())) - 1;
  return v[std::min(i, v.size() - 1)];
}
int g_fail = 0;
void Check(bool c, const std::string& n) {
  if (c)
    std::cout << "PASS " << n << "\n";
  else {
    std::cout << "FAIL " << n << "\n";
    ++g_fail;
  }
}
int selfcheck() {
  const long long rows = 100000;
  dbengine::kv::KVStore kv;
  dbengine::txn::MvccStore mvcc;
  dbengine::sql::Executor ex(kv, mvcc, nullptr);
  ex.execute(
      "CREATE TABLE lineitem (orderkey INT, qty INT, price DOUBLE, "
      "disc DOUBLE, tax DOUBLE, rf TEXT, ls TEXT, shipdate INT)");
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
  double ref = 0.0;
  const long long kBatch = 500;
  for (long long base = 0; base < rows; base += kBatch) {
    long long cur = std::min(kBatch, rows - base);
    std::string sql = "INSERT INTO lineitem VALUES ";
    for (long long i = 0; i < cur; ++i) {
      long long idx = base + i;
      double price = static_cast<double>(price_cents_d(rng)) / 100.0;
      double disc = static_cast<double>(disc_pct_d(rng)) / 100.0;
      double tax = static_cast<double>(tax_pct_d(rng)) / 100.0;
      long long qty = qty_d(rng);
      int shipdate = yr_d(rng) * 10000 + mo_d(rng) * 100 + da_d(rng);
      bool pass = (disc >= 0.05 && disc <= 0.07) && (qty < 24) &&
                  (price >= 500.0) && (price < 5000.0) && (tax <= 0.05) &&
                  (shipdate >= 19940101 && shipdate <= 19951231);
      if (pass) ref += price * disc;
      char row[128];
      std::snprintf(row, sizeof(row), "%s(%lld,%lld,%.2f,%.2f,%.2f,'%s','%s',%d)",
                    (i == 0 ? "" : ","), idx + 1, qty, price, disc, tax,
                    rfs[rf_d(rng)], lss[ls_d(rng)], shipdate);
      sql += row;
    }
    ex.execute(sql);
  }
  const std::string q6 =
      "SELECT SUM(price*disc) FROM lineitem WHERE disc BETWEEN 0.05 AND "
      "0.07 AND qty < 24 AND price >= 500.0 AND price < 5000.0 AND tax <= "
      "0.05 AND shipdate BETWEEN 19940101 AND 19951231";
  std::vector<double> lat;
  double sum = 0.0;
  for (int r = 0; r < 3; ++r) {
    auto s = std::chrono::steady_clock::now();
    dbengine::sql::Result res = ex.execute(q6);
    auto e = std::chrono::steady_clock::now();
    lat.push_back(std::chrono::duration<double, std::milli>(e - s).count());
    if (!res.rows.empty() && !res.rows[0].empty()) {
      bool ok = false;
      sum = ValToDouble(res.rows[0][0], ok);
      if (!ok) sum = 0.0;
    }
  }
  double denom = std::max(1.0, std::fabs(ref));
  double rel = std::fabs(sum - ref) / denom;
  Check(rel <= 1e-9, "q6/sum-correct");
  Check(lat.size() == 3 && P95(lat) > 0.0, "q6/p95-valid");
  std::cout << "q6bench: rows=" << rows << " sum=" << sum << " ref=" << ref
            << " p95_ms=" << P95(lat) << "\n";
  return g_fail;
}
}  // namespace
int main(int argc, char** argv) {
  std::string a = argc > 1 ? argv[1] : "";
  if (a == "--selfcheck") return selfcheck() == 0 ? 0 : 1;
  std::cerr << "Aufruf: q6bench --selfcheck\n";
  return 1;
}
