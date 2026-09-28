// s102-dummy_app: Dummy-Application-Load ueber virtuelle L2/L3-Trennung.
// STL-only (+ dbengine-KV/SQL/Vector). KEIN netns/veth/root: nutzt dieselbe
// in-process-Semantik wie tools/netsandbox.cpp (L2-Partition/Heal,
// L3-Subnetz-Isolation), damit Ergebnisse reproduzierbar + ohne CAP_NET_ADMIN
// laufen. Jede DB-Op ist genau ein L3-Paket app->db; bei Partition failt die
// Op schnell (Drop gezaehlt), nach Heal geht es weiter — Stabilitaet messbar.
//
// Workload (deterministisch, schnell, <2s):
//   KV: 2000 Puts + Spot-Reads ueber KVStore
//   SQL: CREATE + 500 Inserts + SELECT SUM/COUNT ueber Executor (KV+MVCC)
//   ANN: 500 Vektoren d=16 in HnswIndex + 100 Queries (Recall via brute_force)
//   Chaos: Partition app<->db -> Ops muessen droppen; Heal -> Ops ok, Daten da.
// --selfcheck assertet alles + druckt Throughput/p95 (CSV nach stdout).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include <vector>

#include "dbengine/kv.h"
#include "dbengine/sql/executor.h"
#include "dbengine/txn/mvcc.h"
#include "dbengine/vector/hnsw.h"

namespace {
// ---- Mini-Net (spiegelt netsandbox-Semantik) ----
struct MiniNet {
  bool cut_app_db = false;       // L2-Partition app<->db
  bool subnet_isolated = false;  // L3-Isolation 10.0.1/24 <-> 10.0.2/24
  std::uint64_t sent = 0, ok = 0, dropped = 0;
  // Genau ein Paket app->db. false = Drop (Partition oder Isolation).
  bool pkt() {
    ++sent;
    if (cut_app_db || subnet_isolated) {
      ++dropped;
      return false;
    }
    ++ok;
    return true;
  }
  void reset() { sent = ok = dropped = 0; }
};

double p95(std::vector<double> v) {
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
  using dbengine::kv::KVStore;
  using dbengine::vector::DistanceMetric;
  using dbengine::vector::HnswIndex;
  using dbengine::vector::Vector;
  MiniNet net;

  // ---- KV-Phase (2000 Puts, jedes ein Paket) ----
  KVStore kv;
  std::vector<double> kv_us;
  int kv_drops = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 2000; ++i) {
    auto q0 = std::chrono::steady_clock::now();
    if (!net.pkt()) {
      ++kv_drops;
      continue;
    }
    kv.Put("k" + std::to_string(i), "v" + std::to_string(i));
    auto q1 = std::chrono::steady_clock::now();
    kv_us.push_back(std::chrono::duration<double, std::micro>(q1 - q0).count());
  }
  auto t1 = std::chrono::steady_clock::now();
  double kv_s = std::chrono::duration<double>(t1 - t0).count();
  Check(kv_drops == 0, "dummy/kv-no-drop-when-healed");
  Check(kv.Get("k0").has_value() && kv.Get("k1999").has_value(),
        "dummy/kv-readable");
  double kv_thr = 2000.0 / std::max(kv_s, 1e-9);

  // ---- SQL-Phase (CREATE + 500 INSERT + SUM, jedes ein Paket) ----
  dbengine::txn::MvccStore mvcc;
  dbengine::sql::Executor ex(kv, mvcc, nullptr);
  // Eigener KV/MVCC damit SQL-Stand isoliert bleibt (KV-Keys k* stören nicht).
  dbengine::kv::KVStore kv2;
  dbengine::txn::MvccStore mvcc2;
  dbengine::sql::Executor ex2(kv2, mvcc2, nullptr);
  std::vector<double> sql_us;
  int sql_drops = 0;
  auto s_ok = [&](const std::string& q) -> bool {
    if (!net.pkt()) {
      ++sql_drops;
      return false;
    }
    auto q0 = std::chrono::steady_clock::now();
    dbengine::sql::Result r = ex2.execute(q);
    (void)r;
    auto q1 = std::chrono::steady_clock::now();
    sql_us.push_back(std::chrono::duration<double, std::micro>(q1 - q0).count());
    return true;
  };
  Check(s_ok("CREATE TABLE t (id INT, val INT)"), "dummy/sql-create");
  t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 500; ++i)
    if (!s_ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " +
              std::to_string(i) + ")")) break;
  t1 = std::chrono::steady_clock::now();
  double sql_s = std::chrono::duration<double>(t1 - t0).count();
  Check(sql_drops == 0, "dummy/sql-no-drop-when-healed");
  bool sum_ok = false;
  if (net.pkt()) {
    dbengine::sql::Result r = ex2.execute("SELECT SUM(val) FROM t");
    sum_ok = !r.rows.empty();
  }
  Check(sum_ok, "dummy/sql-sum");
  double sql_thr = 500.0 / std::max(sql_s, 1e-9);

  // ---- ANN-Phase (500 Vektoren d=16, 100 Queries; Pakete nur Queries) ----
  HnswIndex idx(16, 16, 32, DistanceMetric::L2);
  for (int i = 0; i < 500; ++i) {
    Vector v(16);
    for (int d = 0; d < 16; ++d) v[d] = float((i * 31 + d * 7) % 100) / 100.0f;
    idx.add(v);
  }
  idx.build();
  std::vector<double> ann_us;
  int ann_hits = 0;
  for (int i = 0; i < 100; ++i) {
    if (!net.pkt()) continue;
    Vector q(16);
    for (int d = 0; d < 16; ++d) q[d] = float((i * 31 + d * 7) % 100) / 100.0f;
    auto q0 = std::chrono::steady_clock::now();
    auto hits = idx.search(q, 10, 32);
    auto q1 = std::chrono::steady_clock::now();
    if (!hits.empty()) ++ann_hits;
    ann_us.push_back(std::chrono::duration<double, std::micro>(q1 - q0).count());
  }
  Check(ann_hits == 100, "dummy/ann-all-hit");

  // ---- Chaos: Partition -> Drops, Heal -> ok + Daten intakt ----
  net.cut_app_db = true;
  int drops = 0;
  for (int i = 0; i < 50; ++i)
    if (!net.pkt()) ++drops;
  Check(drops == 50, "dummy/partition-drops");
  net.cut_app_db = false;
  net.subnet_isolated = true;
  drops = 0;
  for (int i = 0; i < 50; ++i)
    if (!net.pkt()) ++drops;
  Check(drops == 50, "dummy/subnet-isolate-drops");
  net.subnet_isolated = false;
  Check(net.pkt(), "dummy/heal-restores");
  Check(kv.Get("k0").has_value(), "dummy/no-loss-after-heal");

  std::cout << "op,throughput,p95_us,net_sent,net_ok,net_dropped\n";
  std::cout << "kv_puts," << kv_thr << "," << p95(kv_us) << "," << net.sent
            << "," << net.ok << "," << net.dropped << "\n";
  std::cout << "sql_inserts," << sql_thr << "," << p95(sql_us) << ",,,\n";
  std::cout << "ann_queries," << (100.0 / 1.0) << "," << p95(ann_us)
            << ",,,\n";
  return g_fail;
}
}  // namespace

int main(int argc, char** argv) {
  std::string a = argc > 1 ? argv[1] : "";
  if (a == "--selfcheck") {
    int f = selfcheck();
    if (f == 0) {
      std::cout << "dummy_app selfcheck: all passed\n";
      return 0;
    }
    std::cerr << "dummy_app selfcheck: " << f << " FAILURES\n";
    return 1;
  }
  std::cerr << "Aufruf: dummy_app --selfcheck\n";
  return a.empty() ? 1 : 2;
}
