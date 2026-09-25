// Hybrid Search: BM25-lite + dense-Fusion (RRF / Weighted), Top-K Heap.
// Unabhaengig von s08: dense-Scores kommen als Parameter herein.

#include "dbengine/search/hybrid.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace dbengine::search {
namespace {

// Min-Heap Comparator fuer Top-K: schlechtestes Element oben.
// "Schlechter" = kleinerer Score, bei Gleichstand groessere ID
// (damit Tie-Break ID-aufsteigend im Endergebnis deterministisch bleibt).
struct MinHeapCmp {
  bool operator()(const HybridHit& a, const HybridHit& b) const {
    if (a.fused != b.fused) return a.fused > b.fused;  // groesser = "kleiner" im Min-Heap-Sinn... (priority_queue top = groesstes nach Cmp)
    return a.id < b.id;  // groessere ID soll eher verworfen werden -> oben liegen
  }
};

struct MinPairCmp {
  bool operator()(const std::pair<uint32_t, double>& a,
                  const std::pair<uint32_t, double>& b) const {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;
  }
};

bool hitDesc(const HybridHit& a, const HybridHit& b) {
  if (a.fused != b.fused) return a.fused > b.fused;
  return a.id < b.id;
}

}  // namespace

uint64_t tokenHash(std::string_view tok) {
  // FNV-1a 64-bit, deterministisch ueber Plattformen hinweg.
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : tok) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

std::vector<std::string> tokenize(std::string_view text) {
  std::vector<std::string> out;
  std::string cur;
  cur.reserve(16);
  for (char ch : text) {
    unsigned char u = static_cast<unsigned char>(ch);
    if (std::isalnum(u) != 0) {
      cur.push_back(static_cast<char>(std::tolower(u)));
    } else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::vector<uint64_t> tokenizeHashed(std::string_view text) {
  std::vector<uint64_t> out;
  std::string cur;
  cur.reserve(16);
  auto flush = [&] {
    if (!cur.empty()) {
      out.push_back(tokenHash(cur));
      cur.clear();
    }
  };
  for (char ch : text) {
    unsigned char u = static_cast<unsigned char>(ch);
    if (std::isalnum(u) != 0) {
      cur.push_back(static_cast<char>(std::tolower(u)));
    } else {
      flush();
    }
  }
  flush();
  return out;
}

HybridIndex::HybridIndex(HybridConfig cfg) : cfg_(cfg) {}

void HybridIndex::setConfig(const HybridConfig& cfg) { cfg_ = cfg; }

const HybridConfig& HybridIndex::config() const { return cfg_; }

void HybridIndex::addDocument(uint32_t id, const std::string& text) {
  DocEntry e;
  e.text = text;
  for (uint64_t h : tokenizeHashed(text)) {
    e.tf[h]++;
  }
  e.len = tokenizeHashed(text).size();
  // len ohne doppeltes Tokenisieren: tf-Summe == Tokenanzahl
  e.len = 0;
  for (const auto& [_, c] : e.tf) e.len += c;
  docs_[id] = std::move(e);
  dirty_ = true;
}

void HybridIndex::clear() {
  docs_.clear();
  df_.clear();
  totalLen_ = 0;
  avgdl_ = 0.0;
  dirty_ = true;
}

void HybridIndex::build() const {
  df_.clear();
  totalLen_ = 0;
  for (const auto& [id, doc] : docs_) {
    totalLen_ += doc.len;
    for (const auto& [term, _] : doc.tf) df_[term]++;
  }
  avgdl_ = docs_.empty() ? 0.0 : static_cast<double>(totalLen_) / docs_.size();
  dirty_ = false;
}

void HybridIndex::ensureBuilt() const {
  if (dirty_) build();
}

size_t HybridIndex::size() const { return docs_.size(); }

double HybridIndex::avgdl() const {
  ensureBuilt();
  return avgdl_;
}

double HybridIndex::idf(uint64_t term) const {
  ensureBuilt();
  const double n = static_cast<double>(docs_.size());
  auto it = df_.find(term);
  const double df = (it == df_.end()) ? 0.0 : static_cast<double>(it->second);
  return std::log(1.0 + (n - df + 0.5) / (df + 0.5));
}

std::vector<uint64_t> HybridIndex::uniqueTerms(std::vector<uint64_t> v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

double HybridIndex::bm25(uint32_t id, const std::vector<uint64_t>& qterms) const {
  ensureBuilt();
  auto dit = docs_.find(id);
  if (dit == docs_.end() || qterms.empty() || avgdl_ <= 0.0) return 0.0;
  const DocEntry& doc = dit->second;
  double score = 0.0;
  for (uint64_t t : qterms) {
    auto tfit = doc.tf.find(t);
    if (tfit == doc.tf.end()) continue;
    const double tf = static_cast<double>(tfit->second);
    const double idfV = idf(t);
    const double denom = tf + cfg_.k1 * (1.0 - cfg_.b + cfg_.b * doc.len / avgdl_);
    score += idfV * tf * (cfg_.k1 + 1.0) / denom;
  }
  return score;
}

std::vector<std::pair<uint32_t, double>> HybridIndex::bm25Search(
    std::string_view query, size_t topK) const {
  ensureBuilt();
  if (topK == 0 || docs_.empty()) return {};
  const std::vector<uint64_t> q = uniqueTerms(tokenizeHashed(query));

  // Top-K via Min-Heap (priority_queue, Top = schlechtestes der Top-K).
  std::priority_queue<std::pair<uint32_t, double>,
                      std::vector<std::pair<uint32_t, double>>, MinPairCmp>
      heap;
  for (const auto& [id, _] : docs_) {
    double s = bm25(id, q);
    if (heap.size() < topK) {
      heap.emplace(id, s);
    } else {
      const auto& worst = heap.top();
      if (s > worst.second || (s == worst.second && id < worst.first)) {
        heap.pop();
        heap.emplace(id, s);
      }
    }
  }
  std::vector<std::pair<uint32_t, double>> out;
  out.reserve(heap.size());
  while (!heap.empty()) {
    out.push_back(heap.top());
    heap.pop();
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;
  });
  return out;
}

std::vector<HybridHit> HybridIndex::search(
    std::string_view query, const std::vector<DenseScore>& dense, size_t topK) const {
  std::unordered_map<uint32_t, float> m;
  m.reserve(dense.size() * 2 + 1);
  for (const auto& d : dense) m[d.id] = d.score;
  return search(query, m, topK);
}

std::vector<HybridHit> HybridIndex::search(
    std::string_view query, const std::unordered_map<uint32_t, float>& dense,
    size_t topK) const {
  ensureBuilt();
  if (topK == 0 || docs_.empty()) return {};
  const std::vector<uint64_t> q = uniqueTerms(tokenizeHashed(query));

  // 1) BM25 fuer alle Docs.
  std::unordered_map<uint32_t, double> bm;
  bm.reserve(docs_.size() * 2 + 1);
  double bmMin = std::numeric_limits<double>::infinity();
  double bmMax = -std::numeric_limits<double>::infinity();
  for (const auto& [id, _] : docs_) {
    double s = bm25(id, q);
    bm[id] = s;
    bmMin = std::min(bmMin, s);
    bmMax = std::max(bmMax, s);
  }
  if (bmMin == std::numeric_limits<double>::infinity()) {
    bmMin = 0.0;
    bmMax = 0.0;
  }

  // 2) Dense-Min/Max ueber Kandidaten (fehlend = min -> norm 0).
  double dMin = std::numeric_limits<double>::infinity();
  double dMax = -std::numeric_limits<double>::infinity();
  for (const auto& [id, _] : docs_) {
    auto it = dense.find(id);
    if (it != dense.end()) {
      dMin = std::min(dMin, static_cast<double>(it->second));
      dMax = std::max(dMax, static_cast<double>(it->second));
    }
  }
  const bool hasAnyDense = (dMin != std::numeric_limits<double>::infinity());
  if (!hasAnyDense) {
    dMin = 0.0;
    dMax = 0.0;
  }
  const double bmRange = bmMax - bmMin;
  const double dRange = dMax - dMin;

  // 3) RRF-Raenge vorbereiten (nur im RRF-Modus).
  std::unordered_map<uint32_t, size_t> rankBm, rankDense;
  if (cfg_.mode == FusionMode::RRF) {
    std::vector<std::pair<uint32_t, double>> byBm;
    byBm.reserve(docs_.size());
    for (const auto& [id, s] : bm) byBm.emplace_back(id, s);
    std::sort(byBm.begin(), byBm.end(), [](const auto& a, const auto& b) {
      if (a.second != b.second) return a.second > b.second;
      return a.first < b.first;
    });
    for (size_t i = 0; i < byBm.size(); ++i) rankBm[byBm[i].first] = i + 1;  // 1-basiert

    std::vector<std::pair<uint32_t, double>> byDense;
    byDense.reserve(dense.size());
    for (const auto& [id, s] : dense) {
      if (docs_.find(id) != docs_.end()) byDense.emplace_back(id, s);
    }
    std::sort(byDense.begin(), byDense.end(), [](const auto& a, const auto& b) {
      if (a.second != b.second) return a.second > b.second;
      return a.first < b.first;
    });
    for (size_t i = 0; i < byDense.size(); ++i) rankDense[byDense[i].first] = i + 1;
  }

  // 4) Fusion + Top-K Heap.
  std::priority_queue<HybridHit, std::vector<HybridHit>, MinHeapCmp> heap;
  for (const auto& [id, s] : bm) {
    auto dit = dense.find(id);
    const bool hasD = (dit != dense.end());
    const double dVal = hasD ? static_cast<double>(dit->second) : dMin;

    double fused = 0.0;
    if (cfg_.mode == FusionMode::Weighted) {
      const double nBm = (bmRange > 0.0) ? (s - bmMin) / bmRange : 0.0;
      double nD = 0.0;
      if (hasD && dRange > 0.0) nD = (dVal - dMin) / dRange;
      fused = cfg_.w_bm25 * nBm + cfg_.w_dense * nD;
    } else {
      const double rB = static_cast<double>(rankBm[id]);
      fused = cfg_.w_bm25 / (cfg_.rrf_k + rB);
      auto rit = rankDense.find(id);
      if (rit != rankDense.end()) {
        fused += cfg_.w_dense / (cfg_.rrf_k + static_cast<double>(rit->second));
      }
    }

    HybridHit h{id, fused, s, dVal, hasD};
    if (heap.size() < static_cast<size_t>(topK)) {
      heap.push(h);
    } else {
      const HybridHit& worst = heap.top();
      if (fused > worst.fused || (fused == worst.fused && id < worst.id)) {
        heap.pop();
        heap.push(h);
      }
    }
  }

  std::vector<HybridHit> out;
  out.reserve(heap.size());
  while (!heap.empty()) {
    out.push_back(heap.top());
    heap.pop();
  }
  std::sort(out.begin(), out.end(), hitDesc);
  return out;
}

}  // namespace dbengine::search
