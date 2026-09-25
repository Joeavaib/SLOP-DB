// s16-quant Tests: SQ8 Fehler <1e-2, Recall@10 mit Quant >0.9 auf 10k 64-dim,
// DiskSpill roundtrip (mmap), PQ-Stub(2) smoke, ef-Autotune. assert-basiert,
// CTest-Name "quant".

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
#include "dbengine/vector/quant.h"

using dbengine::vector::HnswIndex;
using dbengine::vector::SearchHit;
using dbengine::vector::Vector;
using dbengine::vector::quant::autotune_ef;
using dbengine::vector::quant::autotune_ef_for_filter;
using dbengine::vector::quant::DiskSpill;
using dbengine::vector::quant::PqQuantizer;
using dbengine::vector::quant::quantized_search_rerank;
using dbengine::vector::quant::Sq8Quantizer;

namespace {

constexpr int kN = 10000;
constexpr int kDim = 64;
constexpr int kK = 10;
constexpr int kQueries = 20;
constexpr int kEfRerank = 200;

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

std::vector<SearchHit> brute_force(const std::vector<Vector>& data,
                                   const Vector& q, int k) {
  std::vector<SearchHit> all;
  all.reserve(data.size());
  for (size_t i = 0; i < data.size(); ++i)
    all.push_back({(int)i, dbengine::vector::l2_distance(q, data[i])});
  std::nth_element(all.begin(), all.begin() + k, all.end(),
                   [](const SearchHit& a, const SearchHit& b) {
                     return a.dist < b.dist;
                   });
  all.resize(k);
  std::sort(all.begin(), all.end(), [](const SearchHit& a, const SearchHit& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.id < b.id;
  });
  return all;
}

}  // namespace

int main() {
  // 1) SQ8 fit/encode/decode: max abs Elementfehler < 1e-2.
  auto data = make_dataset(42u, kN, kDim);
  Sq8Quantizer q;
  assert(!q.fitted());
  q.fit(data);
  assert(q.fitted() && q.dim() == kDim);
  float max_err = q.max_abs_error(data);
  std::printf("sq8 max_abs_error=%.6f (need <0.01)\n", max_err);
  assert(max_err < 1e-2f);
  // Konstante Dimension: scale==0, roundtrip exakt.
  {
    std::vector<Vector> cdata(16, Vector(4, 0.5f));
    Sq8Quantizer qc;
    qc.fit(cdata);
    auto code = qc.encode(cdata[0]);
    auto dec = qc.decode(code);
    for (float x : dec) assert(std::fabs(x - 0.5f) < 1e-6f);
  }

  // 2) Recall@10 mit Quant (ADC + Re-Rank) > 0.9 auf 10k 64-dim.
  auto codes = q.encode_all(data);
  assert(codes.size() == data.size());
  auto queries = make_dataset(999u, kQueries, kDim);
  double sum_r = 0.0, min_r = 1.0;
  std::vector<double> lat_ms;
  lat_ms.reserve(kQueries);
  for (auto& qq : queries) {
    auto exact = brute_force(data, qq, kK);
    auto t0 = std::chrono::steady_clock::now();
    auto approx = quantized_search_rerank(qq, data, codes, q, kK, kEfRerank);
    auto t1 = std::chrono::steady_clock::now();
    lat_ms.push_back(
        std::chrono::duration<double, std::milli>(t1 - t0).count());
    assert(approx.size() == (size_t)kK);
    double r = recall_at_k(approx, exact, kK);
    sum_r += r;
    min_r = std::min(min_r, r);
  }
  double avg_r = sum_r / kQueries;
  std::sort(lat_ms.begin(), lat_ms.end());
  double mean_ms = std::accumulate(lat_ms.begin(), lat_ms.end(), 0.0) /
                   lat_ms.size();
  double p50 = lat_ms[lat_ms.size() / 2];
  std::printf("quant recall@10 (N=%d,dim=%d,ef_rerank=%d): avg=%.4f min=%.4f "
              "(need >0.90)\n",
              kN, kDim, kEfRerank, avg_r, min_r);
  std::printf("quant latency: mean=%.3fms p50=%.3fms n=%d\n", mean_ms, p50,
              kQueries);
  assert(avg_r > 0.9);

  // 3) DiskSpill roundtrip (mmap file).
  {
    const std::string path = "/tmp/dbengine_quant_spill.bin";
    std::remove(path.c_str());
    DiskSpill spill;
    spill.create(path, kDim, 512);
    for (int i = 0; i < 512; ++i) spill.set(i, data[i]);
    spill.sync();
    spill.close();
    DiskSpill rd;
    rd.open(path);
    assert(rd.is_open() && rd.dim() == kDim && rd.size() == 512);
    for (int i = 0; i < 512; ++i) {
      Vector v = rd.get(i);
      assert(v.size() == (size_t)kDim);
      for (int d = 0; d < kDim; ++d)
        assert(std::fabs(v[d] - data[i][d]) < 1e-6f);
    }
    rd.close();
    std::remove(path.c_str());
    std::printf("diskspill roundtrip ok (512 x %d, mmap)\n", kDim);
  }

  // 4) PQ-Stub (2 Subspaces) smoke: fit/encode/decode laeuft, dim ok.
  {
    auto small = make_dataset(7u, 2000, kDim);
    PqQuantizer pq(kDim);
    pq.fit(small, /*iters=*/5, /*seed=*/42u);
    assert(pq.fitted() && pq.dim() == kDim);
    auto code = pq.encode(small[0]);
    Vector dec = pq.decode(code);
    assert(dec.size() == (size_t)kDim);
    float d = pq.adc_l2_squared(small[0], code);
    assert(std::isfinite(d) && d >= 0.0f);
    std::printf("pq-stub(2 subspaces,256 centroids) ok, adc_d2=%.4f\n", d);
  }

  // 5) ef-Autotune: groessere Selektivitaet => kleineres ef; monoton.
  {
    int ef_full = autotune_ef(10, 1.0, kN);
    int ef_half = autotune_ef(10, 0.5, kN);
    int ef_sel = autotune_ef(10, 0.1, kN);
    std::printf("autotune ef: sel=1.0->%d, 0.5->%d, 0.1->%d\n", ef_full,
                ef_half, ef_sel);
    assert(ef_full <= ef_half && ef_half <= ef_sel);
    assert(ef_full >= 10 && ef_sel <= 1024);
    int ef_f = autotune_ef_for_filter(10, 1000, 10000, 10000);
    assert(ef_f == autotune_ef(10, 0.1, 10000));
    // hnsw.h bleibt benutzbar (nicht gebrochen): kleiner Index baut+sucht.
    HnswIndex idx(kDim, 16, 32);
    for (int i = 0; i < 10; ++i) idx.add(data[i]);
    idx.build();
    auto hits = idx.search(queries[0], 3, ef_full);
    assert(hits.size() == 3);
  }

  std::printf("quant: all checks passed (N=%d dim=%d recall=%.4f)\n", kN, kDim,
              avg_r);
  return 0;
}
