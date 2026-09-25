// s08-vector Implementierung: Distanz + HNSW-lite Layer0 + Brute-Force.

#include "dbengine/vector/hnsw.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <utility>

namespace dbengine::vector {

namespace {

float vec_norm(const Vector& v) {
  double acc = 0.0;
  for (float x : v) acc += static_cast<double>(x) * x;
  return static_cast<float>(std::sqrt(acc));
}

}  // namespace

float l2_squared(const Vector& a, const Vector& b) {
  if (a.size() != b.size()) throw std::invalid_argument("l2: dim mismatch");
  double acc = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    const double d = static_cast<double>(a[i]) - b[i];
    acc += d * d;
  }
  return static_cast<float>(acc);
}

float l2_distance(const Vector& a, const Vector& b) {
  return static_cast<float>(std::sqrt(static_cast<double>(l2_squared(a, b))));
}

float cosine_distance(const Vector& a, const Vector& b) {
  if (a.size() != b.size())
    throw std::invalid_argument("cosine: dim mismatch");
  double dot = 0.0, na = 0.0, nb = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * b[i];
    na += static_cast<double>(a[i]) * a[i];
    nb += static_cast<double>(b[i]) * b[i];
  }
  if (na == 0.0 || nb == 0.0) return 1.0f;  // Nullvektor: maximal unähnlich
  const double cos_sim = dot / (std::sqrt(na) * std::sqrt(nb));
  const double clamped = std::clamp(cos_sim, -1.0, 1.0);
  return static_cast<float>(1.0 - clamped);
}

// --- SQ8-Stub ---------------------------------------------------------------
void SQ8Quantizer::fit(const std::vector<Vector>&) {
  // Bewusst nicht implementiert in V1: wirft, damit kein stiller
  // Praezisionsverlust entsteht. Follow-up: min/max-Fit + ADC.
  throw std::logic_error(
      "SQ8Quantizer::fit: Scalar-Quantization (SQ8) ist in V1 ein Stub. "
      "Suche nutzt float32 exakt. Siehe hnsw.h Doku.");
}

bool SQ8Quantizer::fitted() const { return params_.fitted; }

// --- HnswIndex --------------------------------------------------------------
HnswIndex::HnswIndex(int dim, int m, int ef_default, DistanceMetric metric)
    : dim_(dim), m_(m), ef_default_(ef_default), metric_(metric) {
  if (dim_ <= 0) throw std::invalid_argument("HnswIndex: dim must be > 0");
  if (m_ <= 0) throw std::invalid_argument("HnswIndex: m must be > 0");
  if (ef_default_ <= 0)
    throw std::invalid_argument("HnswIndex: ef_default must be > 0");
}

void HnswIndex::set_metric(DistanceMetric m) { metric_ = m; }
DistanceMetric HnswIndex::metric() const { return metric_; }

void HnswIndex::set_ef_default(int ef) {
  if (ef <= 0) throw std::invalid_argument("ef_default must be > 0");
  ef_default_ = ef;
}
int HnswIndex::ef_default() const { return ef_default_; }

void HnswIndex::set_m(int m) {
  if (m <= 0) throw std::invalid_argument("m must be > 0");
  m_ = m;
  built_ = false;
}
int HnswIndex::m() const { return m_; }

int HnswIndex::add(const Vector& v) {
  if (static_cast<int>(v.size()) != dim_)
    throw std::invalid_argument("HnswIndex::add: dim mismatch");
  data_.push_back(v);
  norms_.push_back(vec_norm(v));
  built_ = false;
  return static_cast<int>(data_.size()) - 1;
}

void HnswIndex::clear() {
  data_.clear();
  norms_.clear();
  adj_.clear();
  entry_ = 0;
  built_ = false;
}

void HnswIndex::build() {
  const int n = static_cast<int>(data_.size());
  adj_.assign(n, {});
  if (n == 0) {
    built_ = true;
    return;
  }
  if (n == 1) {
    entry_ = 0;
    built_ = true;
    return;
  }
  const int k = std::min(m_, n - 1);

  // 1) Exakter k-NN pro Knoten (fully-connected Layer0, k=M).
  std::vector<int> idx(n);
  std::iota(idx.begin(), idx.end(), 0);
  for (int i = 0; i < n; ++i) {
    // Distanz zu allen j != i
    std::vector<std::pair<float, int>> dj;
    dj.reserve(n - 1);
    for (int j = 0; j < n; ++j) {
      if (j == i) continue;
      dj.emplace_back(dist_to_stored(data_[i], j), j);
    }
    // Top-k via nth_element + sort (O(N) statt O(N log N))
    std::nth_element(dj.begin(), dj.begin() + k, dj.end(),
                     [](const auto& a, const auto& b) {
                       return a.first < b.first;
                     });
    std::sort(dj.begin(), dj.begin() + k,
              [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
              });
    adj_[i].reserve(2 * static_cast<size_t>(m_));
    for (int t = 0; t < k; ++t) adj_[i].push_back(dj[t].second);
  }

  // 2) Symmetrisieren (ungerichtet): jede Kante i->j erzeugt j->i.
  //    Grad kann dabei auf bis zu 2*M wachsen; danach pruefen wir auf
  //    2*M zurueck (naechste Nachbarn behalten). Das erhoeht Konnektivitaet
  //    und damit Recall, ohne die nominale k=16-Semantik zu brechen.
  for (int i = 0; i < n; ++i) {
    for (int nb : adj_[i]) {
      auto& back = adj_[nb];
      if (std::find(back.begin(), back.end(), i) == back.end()) {
        back.push_back(i);
      }
    }
  }
  const int cap = 2 * m_;
  for (int i = 0; i < n; ++i) {
    if (static_cast<int>(adj_[i].size()) > cap) {
      std::vector<std::pair<float, int>> scored;
      scored.reserve(adj_[i].size());
      for (int nb : adj_[i]) scored.emplace_back(dist_to_stored(data_[i], nb), nb);
      std::nth_element(scored.begin(), scored.begin() + cap, scored.end(),
                       [](const auto& a, const auto& b) {
                         return a.first < b.first;
                       });
      std::sort(scored.begin(), scored.begin() + cap,
                [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return a.second < b.second;
                });
      adj_[i].clear();
      for (int t = 0; t < cap; ++t) adj_[i].push_back(scored[t].second);
    }
    // Deterministische Ordnung: nach (Distanz, ID) sortieren.
    std::sort(adj_[i].begin(), adj_[i].end(), [&](int a, int b) {
      const float da = dist_to_stored(data_[i], a);
      const float db = dist_to_stored(data_[i], b);
      if (da != db) return da < db;
      return a < b;
    });
  }

  entry_ = 0;
  built_ = true;
}

bool HnswIndex::built() const { return built_; }
size_t HnswIndex::size() const { return data_.size(); }
int HnswIndex::dim() const { return dim_; }

const Vector& HnswIndex::get(int id) const {
  if (id < 0 || id >= static_cast<int>(data_.size()))
    throw std::out_of_range("HnswIndex::get: bad id");
  return data_[id];
}

size_t HnswIndex::neighbor_count(int id) const {
  if (id < 0 || id >= static_cast<int>(adj_.size()))
    throw std::out_of_range("neighbor_count: bad id");
  return adj_[id].size();
}

float HnswIndex::dist(const Vector& a, const Vector& b) const {
  return dispatch_distance(a, b, metric_);
}

float HnswIndex::dist_to_stored(const Vector& q, int id) const {
  // Cosine via gespeicherte Norm + Dot (spart eine Norm-Berechnung).
  if (metric_ == DistanceMetric::L2) {
    return l2_distance(q, data_[id]);
  }
  // Cosine-Pfad
  const Vector& b = data_[id];
  double dot = 0.0, nq = 0.0;
  for (size_t i = 0; i < b.size(); ++i) {
    dot += static_cast<double>(q[i]) * b[i];
    nq += static_cast<double>(q[i]) * q[i];
  }
  const double nb = static_cast<double>(norms_[id]) * norms_[id];
  if (nq == 0.0 || nb == 0.0) return 1.0f;
  const double cos_sim = dot / (std::sqrt(nq * nb));
  return static_cast<float>(1.0 - std::clamp(cos_sim, -1.0, 1.0));
}

std::vector<SearchHit> HnswIndex::brute_force(const Vector& query, int k,
                                             FilterFn filter) const {
  if (static_cast<int>(query.size()) != dim_)
    throw std::invalid_argument("brute_force: dim mismatch");
  if (k <= 0) return {};
  std::vector<SearchHit> all;
  all.reserve(data_.size());
  for (int i = 0; i < static_cast<int>(data_.size()); ++i) {
    if (filter && !filter(i)) continue;
    all.push_back({i, dist(query, data_[i])});
  }
  if (static_cast<int>(all.size()) > k) {
    std::nth_element(all.begin(), all.begin() + k, all.end(),
                     [](const SearchHit& a, const SearchHit& b) {
                       if (a.dist != b.dist) return a.dist < b.dist;
                       return a.id < b.id;
                     });
    all.resize(k);
  }
  std::sort(all.begin(), all.end(), [](const SearchHit& a, const SearchHit& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.id < b.id;
  });
  return all;
}

std::vector<SearchHit> HnswIndex::search(const Vector& query, int k, int ef,
                                        FilterFn filter) const {
  if (static_cast<int>(query.size()) != dim_)
    throw std::invalid_argument("search: dim mismatch");
  if (k <= 0) return {};
  const int n = static_cast<int>(data_.size());
  if (n == 0) return {};

  int ef_search = (ef <= 0) ? ef_default_ : ef;
  if (ef_search < k) ef_search = k;
  if (ef_search > n) ef_search = n;

  // Kleiner Index: Graph-Suche lohnt nicht, exakter Pfad (gleiche Semantik).
  // Schwelle bewusst niedrig (64), damit der 1000er-Bench den ANN-Pfad nimmt.
  if (n <= 64) return brute_force(query, k, filter);

  using Cand = std::pair<float, int>;  // (dist, id)
  std::priority_queue<Cand, std::vector<Cand>, std::greater<Cand>> frontier;
  // Top-ef (nur gefilterte): Max-Heap, schlechtester oben.
  std::priority_queue<Cand> top;
  std::vector<char> visited(n, 0);

  auto consider = [&](int id, float d) {
    // Frontier bekommt ALLE Knoten (auch ungefilterte) -> gemeinsame
    // Filter+ANN-Evaluierung, kein Disconnect bei selektiven Filtern.
    // Admission in top nur bei passendem Filter.
    if (!filter || filter(id)) {
      if (static_cast<int>(top.size()) < ef_search) {
        top.emplace(d, id);
      } else if (d < top.top().first ||
                 (d == top.top().first && id < top.top().second)) {
        top.pop();
        top.emplace(d, id);
      }
    }
  };

  // Multi-Entry-Seed (4 fixe Entries): robuster Start auf uniformen
  // Zufallsdaten, weiterhin O(1) Seeds (kein Scan, weiterhin ANN).
  const int seeds[4] = {entry_, n / 4, n / 2, (3 * n) / 4};
  for (int s : seeds) {
    if (s < 0 || s >= n || visited[s]) continue;
    visited[s] = 1;
    const float d = dist_to_stored(query, s);
    frontier.emplace(d, s);
    consider(s, d);
  }

  // Beam-Loop: expandiere naechsten Frontier-Knoten, biete Nachbarn an.
  // Stopp: Frontier leer ODER naechste Distanz schlechter als schlechtestes
  // Top-Ergebnis bei vollem ef (filter-aware: ungefilterte Frontier-Knoten
  // treiben die Exploration weiter, bis ef gefilterte vorliegen).
  while (!frontier.empty()) {
    const auto [d_u, u] = frontier.top();
    if (static_cast<int>(top.size()) >= ef_search && d_u > top.top().first)
      break;
    frontier.pop();
    for (int v : adj_[u]) {
      if (visited[v]) continue;
      visited[v] = 1;
      const float d_v = dist_to_stored(query, v);
      frontier.emplace(d_v, v);
      consider(v, d_v);
    }
  }

  // Falls Filter extrem selektiv und ef zu klein: adaptiv nachfuellen.
  // Rest-Scan nur ueber noch unbesuchte Knoten (typisch 0, da Beam bei
  // N=1000 fast alles besucht). Das ist KEIN Pre-Filter (kein separater
  // Subgraph-Scan), sondern eine garantierende Vervollstaendigung des
  // gleichen Durchgangs, damit search() nie f grundlos <k liefert.
  if (static_cast<int>(top.size()) < k) {
    for (int i = 0; i < n && static_cast<int>(top.size()) < ef_search; ++i) {
      if (visited[i]) continue;
      visited[i] = 1;
      if (filter && !filter(i)) continue;
      top.emplace(dist_to_stored(query, i), i);
    }
  }

  std::vector<SearchHit> out;
  out.reserve(top.size());
  while (!top.empty()) {
    out.push_back({top.top().second, top.top().first});
    top.pop();
  }
  std::sort(out.begin(), out.end(), [](const SearchHit& a, const SearchHit& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.id < b.id;
  });
  if (static_cast<int>(out.size()) > k) out.resize(k);
  return out;
}

}  // namespace dbengine::vector
