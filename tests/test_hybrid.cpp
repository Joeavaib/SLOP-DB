// s09-hybrid Tests: 100 Docs, BM25+dense Fusion, RRF deterministisch.
// Stil: assert-basiert ohne GTest (Zero-Ops), CTest-Name "hybrid".

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "dbengine/search/hybrid.h"

using dbengine::search::DenseScore;
using dbengine::search::FusionMode;
using dbengine::search::HybridConfig;
using dbengine::search::HybridIndex;

namespace {

HybridIndex build100(std::vector<DenseScore>* denseOut = nullptr) {
  HybridIndex idx;
  // 100 Docs: Basis-Text fuer alle, damit DF/avgdl realistisch sind.
  for (uint32_t i = 0; i < 100; ++i) {
    std::string text = "doc record " + std::to_string(i) +
                       " generic data table index storage common terms";
    if (i == 7) {
      // BM25-Top: Query-Terme doppelt + dense hoch.
      text += " database vector search database vector search engine hybrid fusion rank";
    } else if (i == 42) {
      text += " database vector search engine hybrid fusion rank";
    } else if (i == 15 || i == 23) {
      text += " database search engine";
    } else if (i == 99) {
      // Dense-Verfuehrer: kein Query-Term, aber hoechster dense-Score.
      text += " unrelated weather cooking recipe";
    }
    idx.addDocument(i, text);
  }
  idx.build();
  if (denseOut != nullptr) {
    denseOut->clear();
    for (uint32_t i = 0; i < 100; ++i) {
      float s = 0.10f + static_cast<float>(i % 10) * 0.01f;  // 0.10..0.19 deterministisch
      denseOut->push_back({i, s});
    }
    for (auto& d : *denseOut) {
      if (d.id == 7) d.score = 0.90f;
      if (d.id == 42) d.score = 0.88f;
      if (d.id == 15) d.score = 0.85f;
      if (d.id == 99) d.score = 0.99f;  // dense-only Sieger
    }
  }
  return idx;
}

bool sameOrder(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
  return a == b;
}

std::vector<uint32_t> idsOf(const std::vector<std::pair<uint32_t, double>>& v) {
  std::vector<uint32_t> o;
  for (auto& p : v) o.push_back(p.first);
  return o;
}

}  // namespace

int main() {
  // Config-Defaults: k1=1.2, b=0.75.
  {
    HybridConfig c;
    assert(std::fabs(c.k1 - 1.2) < 1e-12);
    assert(std::fabs(c.b - 0.75) < 1e-12);
    assert(std::fabs(c.w_dense - 0.5) < 1e-12);
    assert(std::fabs(c.w_bm25 - 0.5) < 1e-12);
    assert(c.mode == FusionMode::Weighted);
    assert(c.rrf_k == 60);
  }

  // Token-Hash deterministisch + Tokenizer.
  {
    auto t = dbengine::search::tokenize("Database, VECTOR-search!");
    assert(t.size() == 3 && t[0] == "database" && t[1] == "vector" && t[2] == "search");
    assert(dbengine::search::tokenHash("abc") == dbengine::search::tokenHash("abc"));
  }

  std::vector<DenseScore> dense;
  HybridIndex idx = build100(&dense);
  assert(idx.size() == 100);
  assert(idx.avgdl() > 0.0);
  const std::string query = "database vector search";

  // 1) BM25 pur: Doc 7 (doppelte Termfrequenz) vor 42, beide klar vor Rest.
  {
    auto top = idx.bm25Search(query, 10);
    assert(top.size() == 10);
    assert(top[0].first == 7);
    assert(top[1].first == 42);
    // sortiert absteigend, Tie-Break ID-aufsteigend
    for (size_t i = 1; i < top.size(); ++i) {
      assert(top[i - 1].second >= top[i].second);
      if (top[i - 1].second == top[i].second) assert(top[i - 1].first < top[i].first);
    }
    // Determinismus: zweimal identisch
    auto again = idx.bm25Search(query, 10);
    assert(sameOrder(idsOf(top), idsOf(again)));
    std::printf("BM25 top3: %u(%.3f) %u(%.3f) %u(%.3f)\n", top[0].first, top[0].second,
                top[1].first, top[1].second, top[2].first, top[2].second);
  }

  // 2) Weighted-Fusion: Doc 99 (dense 0.99, BM25 0) darf NICHT Top-1 sein,
  //    weil BM25 ihn auf 0 zieht; 7 (beide Signale stark) gewinnt.
  {
    HybridConfig c;
    c.mode = FusionMode::Weighted;
    c.w_bm25 = 0.5;
    c.w_dense = 0.5;
    idx.setConfig(c);
    auto hits = idx.search(query, dense, 10);
    assert(hits.size() == 10);
    assert(hits[0].id == 7);
    // 42 unter Top-3
    bool has42 = false;
    for (size_t i = 0; i < 3; ++i)
      if (hits[i].id == 42) has42 = true;
    assert(has42);
    // 99 nicht in Top-3 (BM25=0 bremst)
    for (size_t i = 0; i < 3; ++i) assert(hits[i].id != 99);
    // Determinismus
    auto hits2 = idx.search(query, dense, 10);
    for (size_t i = 0; i < hits.size(); ++i) assert(hits[i].id == hits2[i].id);
    std::printf("Weighted top5:");
    for (size_t i = 0; i < 5; ++i)
      std::printf(" %u(f=%.4f,bm=%.3f,d=%.2f)", hits[i].id, hits[i].fused,
                  hits[i].bm25, hits[i].dense);
    std::printf("\n");
  }

  // 3) Gewichte wirken: dense-only -> 99 gewinnt; bm25-only -> 7 gewinnt.
  {
    HybridConfig c;
    c.mode = FusionMode::Weighted;
    c.w_dense = 1.0;
    c.w_bm25 = 0.0;
    idx.setConfig(c);
    auto dOnly = idx.search(query, dense, 5);
    assert(dOnly[0].id == 99);

    c.w_dense = 0.0;
    c.w_bm25 = 1.0;
    idx.setConfig(c);
    auto bOnly = idx.search(query, dense, 5);
    assert(bOnly[0].id == 7);
  }

  // 4) RRF-Fusion: deterministische Order, Top-1 in {7,42}, 99 nicht Top-1.
  {
    HybridConfig c;
    c.mode = FusionMode::RRF;
    c.w_dense = 0.5;
    c.w_bm25 = 0.5;
    c.rrf_k = 60;
    idx.setConfig(c);
    auto r1 = idx.search(query, dense, 10);
    auto r2 = idx.search(query, dense, 10);
    assert(r1.size() == 10 && r2.size() == 10);
    for (size_t i = 0; i < r1.size(); ++i) {
      assert(r1[i].id == r2[i].id);
      assert(std::fabs(r1[i].fused - r2[i].fused) < 1e-12);
    }
    assert(r1[0].id == 7 || r1[0].id == 42);
    assert(r1[0].id != 99);
    for (size_t i = 1; i < r1.size(); ++i) {
      assert(r1[i - 1].fused >= r1[i].fused);
      if (r1[i - 1].fused == r1[i].fused) assert(r1[i - 1].id < r1[i].id);
    }
    // RRF-Formel Spot-Check: fused == w_bm/(60+rankBm) + w_d/(60+rankD)
    // Doc 7 ist BM25-Rang 1; Dense-Rang 2 (99:0.99 vor 7:0.90).
    double expect7 = 0.5 / (60.0 + 1.0) + 0.5 / (60.0 + 2.0);
    bool found = false;
    for (auto& h : r1)
      if (h.id == 7) {
        assert(std::fabs(h.fused - expect7) < 1e-9);
        found = true;
      }
    assert(found);
    std::printf("RRF top5:");
    for (size_t i = 0; i < 5; ++i)
      std::printf(" %u(f=%.6f)", r1[i].id, r1[i].fused);
    std::printf("\n");
  }

  // 5) Top-K Heap: K=5 liefert genau 5, sortiert; K > N deckelt auf N.
  {
    HybridConfig c;
    c.mode = FusionMode::Weighted;
    idx.setConfig(c);
    auto k5 = idx.search(query, dense, 5);
    assert(k5.size() == 5);
    for (size_t i = 1; i < k5.size(); ++i) assert(k5[i - 1].fused >= k5[i].fused);
    auto kAll = idx.search(query, dense, 500);
    assert(kAll.size() == 100);
    auto k0 = idx.search(query, dense, 0);
    assert(k0.empty());
  }

  // 6) Robustheit: leere/unbekannte Query stuerzt nicht ab; map-Uebergabe geht.
  {
    auto e1 = idx.bm25Search("", 5);
    assert(e1.size() == 5);
    auto e2 = idx.search("zzzzqqqq", dense, 5);
    assert(e2.size() == 5);
    std::unordered_map<uint32_t, float> m;
    for (auto& d : dense) m[d.id] = d.score;
    auto e3 = idx.search(query, m, 5);
    assert(e3.size() == 5 && e3[0].id == 7);
    // fehlende dense-Eintraege -> has_dense=false, kein Crash
    std::unordered_map<uint32_t, float> sparse{{7, 0.9f}};
    auto e4 = idx.search(query, sparse, 5);
    assert(e4.size() == 5);
  }

  std::printf("hybrid: all 100-doc fusion checks passed\n");
  return 0;
}
