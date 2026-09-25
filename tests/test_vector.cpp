// s08-vector Tests: 1000 zufaellige 64-dim Vektoren, Recall@10 vs. Brute-Force
// >0.9, filterbare Suche korrekt, ef runtime-tunbar, Latenz p95 (chrono).
// Stil: assert-basiert ohne GTest (Zero-Ops), CTest-Name "vector".

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>
#include <unordered_set>
#include <vector>

#include "dbengine/vector/hnsw.h"

using dbengine::vector::cosine_distance;
using dbengine::vector::DistanceMetric;
using dbengine::vector::HnswIndex;
using dbengine::vector::l2_distance;
using dbengine::vector::SearchHit;
using dbengine::vector::Vector;

namespace {

constexpr int kN = 1000;
constexpr int kDim = 64;
constexpr int kK = 10;
constexpr int kQueries = 100;
constexpr double kRecallMin = 0.9;

std::vector<Vector> make_dataset(unsigned seed, int n, int dim) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  std::vector<Vector> data;
  data.reserve(n);
  for (int i = 0; i < n; ++i) {
    Vector v(dim);
    for (int d = 0; d < dim; ++d) v[d] = u(rng);
    data.push_back(std::move(v));
  }
  return data;
}

double recall_at_k(const std::vector<SearchHit>& approx,
                   const std::vector<SearchHit>& exact, int k) {
  std::unordered_set<int> gt;
  for (int i = 0; i < k && i < (int)exact.size(); ++i) gt.insert(exact[i].id);
  int hits = 0;
  for (int i = 0; i < k && i < (int)approx.size(); ++i)
    if (gt.count(approx[i].id)) ++hits;
  return static_cast<double>(hits) / k;
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  size_t idx = static_cast<size_t>(p * (v.size() - 1));
  return v[idx];
}

}  // namespace

int main() {
  // 0) Distanz-Sanity: L2 + Cosine.
  {
    Vector a = {1.0f, 0.0f}, b = {0.0f, 1.0f}, c = {1.0f, 0.0f};
    assert(std::fabs(l2_distance(a, a)) < 1e-6f);
    assert(std::fabs(l2_distance(a, b) - std::sqrt(2.0f)) < 1e-5f);
    assert(std::fabs(cosine_distance(a, c)) < 1e-6f);
    assert(std::fabs(cosine_distance(a, b) - 1.0f) < 1e-6f);
    // Cosine(gegenlaeufig) = 2
    Vector d = {-1.0f, 0.0f};
    assert(std::fabs(cosine_distance(a, d) - 2.0f) < 1e-6f);
    // Nullvektor darf nicht NaN liefern
    Vector z = {0.0f, 0.0f};
    assert(std::isfinite(cosine_distance(a, z)));
  }

  // 1) Dataset + Index aufbauen.
  auto data = make_dataset(42u, kN, kDim);
  HnswIndex idx(kDim, /*m=*/16, /*ef_default=*/32, DistanceMetric::L2);
  assert(idx.dim() == kDim && idx.m() == 16 && idx.ef_default() == 32);
  for (auto& v : data) idx.add(v);
  assert(idx.size() == (size_t)kN);
  idx.build();
  assert(idx.built());
  for (int i = 0; i < kN; ++i) {
    assert(idx.neighbor_count(i) >= 1);
    assert(idx.neighbor_count(i) <= 32);  // <= 2*M nach Symmetrisierung
  }

  // Queries: frische Zufallsvektoren (gleiche Verteilung, andere Seeds).
  auto queries = make_dataset(1234u, kQueries, kDim);

  // 2) Recall@10 ungefiltet, ef=64 (runtime-tunbar).
  double sum_recall = 0.0;
  double min_recall = 1.0;
  for (auto& q : queries) {
    auto exact = idx.brute_force(q, kK);
    auto approx = idx.search(q, kK, /*ef=*/64);
    assert(exact.size() == (size_t)kK && approx.size() == (size_t)kK);
    // sortiert aufsteigend?
    for (size_t i = 1; i < approx.size(); ++i)
      assert(approx[i - 1].dist <= approx[i].dist + 1e-6f);
    double r = recall_at_k(approx, exact, kK);
    sum_recall += r;
    min_recall = std::min(min_recall, r);
  }
  double avg_recall = sum_recall / kQueries;
  std::printf("recall@10 unfilt (ef=64): avg=%.4f min=%.4f (need >%.2f)\n",
              avg_recall, min_recall, kRecallMin);
  assert(avg_recall > kRecallMin);

  // 3) ef ist runtime-tunbar: groesseres ef darf Recall nicht verschlechtern.
  //    (stellt sicher, dass ef wirklich als Parameter wirkt, F4.3)
  {
    double r_small = 0.0, r_big = 0.0;
    for (auto& q : queries) {
      auto exact = idx.brute_force(q, kK);
      r_small += recall_at_k(idx.search(q, kK, /*ef=*/16), exact, kK);
      r_big += recall_at_k(idx.search(q, kK, /*ef=*/128), exact, kK);
    }
    r_small /= kQueries;
    r_big /= kQueries;
    std::printf("ef-tunbar: recall ef=16 -> %.4f, ef=128 -> %.4f\n", r_small,
                r_big);
    assert(r_big + 1e-9 >= r_small);
    // Default-ef greift bei ef<=0
    idx.set_ef_default(64);
    auto a = idx.search(queries[0], kK);
    auto b = idx.search(queries[0], kK, 64);
    assert(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) assert(a[i].id == b[i].id);
    idx.set_ef_default(32);
  }

  // 4) Filterbare Suche, GEMEINSAM evaluiert (kein Pre/Post-Filter).
  //    4a) 50%-Filter (gerade IDs): korrekt + Recall hoch.
  {
    auto even = [](int id) { return id % 2 == 0; };
    double sum_r = 0.0;
    for (int qi = 0; qi < 20; ++qi) {
      const auto& q = queries[qi];
      auto exact = idx.brute_force(q, kK, even);
      auto approx = idx.search(q, kK, /*ef=*/64, even);
      assert(exact.size() == (size_t)kK && approx.size() == (size_t)kK);
      for (auto& h : approx) assert(h.id % 2 == 0);  // Korrektheit
      for (size_t i = 1; i < approx.size(); ++i)
        assert(approx[i - 1].dist <= approx[i].dist + 1e-6f);
      sum_r += recall_at_k(approx, exact, kK);
    }
    std::printf("recall@10 filtered (even, ef=64): avg=%.4f\n", sum_r / 20);
    assert(sum_r / 20 > kRecallMin);
  }
  //    4b) Selektiver Filter (10%: id%10==0) mit groesserem ef.
  {
    auto tenth = [](int id) { return id % 10 == 0; };
    double sum_r = 0.0;
    for (int qi = 0; qi < 20; ++qi) {
      const auto& q = queries[qi];
      auto exact = idx.brute_force(q, kK, tenth);
      auto approx = idx.search(q, kK, /*ef=*/128, tenth);
      assert(exact.size() == (size_t)kK && approx.size() == (size_t)kK);
      for (auto& h : approx) assert(h.id % 10 == 0);
      sum_r += recall_at_k(approx, exact, kK);
    }
    std::printf("recall@10 filtered (10%%, ef=128): avg=%.4f\n", sum_r / 20);
    assert(sum_r / 20 > 0.85);
  }
  //    4c) Leerer Filter -> leer, ohne Crash.
  {
    auto none = [](int) { return false; };
    auto r = idx.search(queries[0], kK, 64, none);
    assert(r.empty());
    auto b = idx.brute_force(queries[0], kK, none);
    assert(b.empty());
  }

  // 5) Cosine-Metrik: Index umschaltbar, Recall ebenfalls hoch.
  {
    HnswIndex cidx(kDim, 16, 32, DistanceMetric::Cosine);
    for (auto& v : data) cidx.add(v);
    cidx.build();
    double sum_r = 0.0;
    for (int qi = 0; qi < 20; ++qi) {
      auto exact = cidx.brute_force(queries[qi], kK);
      auto approx = cidx.search(queries[qi], kK, 64);
      sum_r += recall_at_k(approx, exact, kK);
    }
    std::printf("recall@10 cosine (ef=64): avg=%.4f\n", sum_r / 20);
    assert(sum_r / 20 > kRecallMin);
    // set_metric wirkt zur Laufzeit
    cidx.set_metric(DistanceMetric::L2);
    assert(cidx.metric() == DistanceMetric::L2);
  }

  // 6) Latenz-Bench (chrono): 1000 Suchen, p50/p95 in ms.
  {
    std::vector<double> ms;
    ms.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
      const auto& q = queries[i % kQueries];
      auto t0 = std::chrono::steady_clock::now();
      volatile auto res = idx.search(q, kK, 32);
      auto t1 = std::chrono::steady_clock::now();
      ms.push_back(
          std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    double mean =
        std::accumulate(ms.begin(), ms.end(), 0.0) / ms.size();
    double p50 = percentile(ms, 0.50);
    double p95 = percentile(ms, 0.95);
    std::printf("latency search(k=10,ef=32,N=1000): mean=%.4fms p50=%.4fms "
                "p95=%.4fms (SLO p95<10ms)\n",
                mean, p50, p95);
    assert(p95 < 10.0);
  }

  std::printf("vector: all checks passed (N=%d, dim=%d, recall>%.2f)\n", kN,
              kDim, kRecallMin);
  return 0;
}
