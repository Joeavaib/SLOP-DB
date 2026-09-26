// s21-vector: echter mehrschichtiger HNSW + Brute-Force Baseline.
// Abwaertskompatibel zu s08 (gleiche public API + neue Tunables).

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

double ml_for_m(int m) {
  if (m <= 1) return 1.0;
  return 1.0 / std::log(static_cast<double>(m));
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
  if (na == 0.0 || nb == 0.0) return 1.0f;
  const double cos_sim = dot / (std::sqrt(na) * std::sqrt(nb));
  const double clamped = std::clamp(cos_sim, -1.0, 1.0);
  return static_cast<float>(1.0 - clamped);
}

// --- SQ8-Stub (unveraendert s08) --------------------------------------------
void SQ8Quantizer::fit(const std::vector<Vector>&) {
  throw std::logic_error(
      "SQ8Quantizer::fit: Scalar-Quantization (SQ8) ist in V1 ein Stub. "
      "Suche nutzt float32 exakt. Siehe hnsw.h Doku.");
}

bool SQ8Quantizer::fitted() const { return params_.fitted; }

// --- HnswIndex --------------------------------------------------------------
HnswIndex::HnswIndex(int dim, int m, int ef_default, DistanceMetric metric)
    : dim_(dim),
      m_(m),
      ef_default_(ef_default),
      metric_(metric),
      ml_(ml_for_m(m)) {
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
  if (m == m_) return;
  m_ = m;
  ml_ = ml_for_m(m_);
  // Graph-Topologie haengt von M ab -> Rebuild erzwingen.
  links_.clear();
  levels_.clear();
  max_level_ = -1;
  built_ = false;
}
int HnswIndex::m() const { return m_; }

void HnswIndex::set_ef_construction(int ef) {
  if (ef <= 0) throw std::invalid_argument("ef_construction must be > 0");
  ef_construction_ = ef;
}
int HnswIndex::ef_construction() const { return ef_construction_; }

void HnswIndex::set_rng_seed(unsigned seed) { rng_.seed(seed); }
int HnswIndex::max_level() const { return max_level_; }

int HnswIndex::level(int id) const {
  if (id < 0 || id >= static_cast<int>(levels_.size()))
    throw std::out_of_range("level: bad id");
  return levels_[id];
}

int HnswIndex::random_level() {
  std::uniform_real_distribution<double> u(0.0, 1.0);
  double r = u(rng_);
  if (r <= 0.0) r = 1e-9;
  if (r >= 1.0) r = 1.0 - 1e-9;
  int l = static_cast<int>(std::floor(-std::log(r) * ml_));
  if (l < 0) l = 0;
  if (l > 8) l = 8;  // Cap: begrenzt Speicher, reicht bis >>1M Knoten.
  return l;
}

int HnswIndex::add(const Vector& v) {
  if (static_cast<int>(v.size()) != dim_)
    throw std::invalid_argument("HnswIndex::add: dim mismatch");
  const int id = static_cast<int>(data_.size());
  data_.push_back(v);
  norms_.push_back(vec_norm(v));
  const int lv = random_level();
  levels_.push_back(lv);
  links_.emplace_back(static_cast<size_t>(lv) + 1);
  if (!built_) {
    // Deferred: build() fuegt ein. Erster Knoten setzt Entry-Provisorium.
    if (id == 0) {
      entry_ = 0;
      max_level_ = lv;
    } else if (lv > max_level_) {
      max_level_ = lv;
      entry_ = id;  // Provisorium bis insert-Reihenfolge in build()
    }
    return id;
  }
  insert_node(id);
  return id;
}

void HnswIndex::clear() {
  data_.clear();
  norms_.clear();
  levels_.clear();
  links_.clear();
  entry_ = 0;
  max_level_ = -1;
  built_ = false;
}

std::vector<int> HnswIndex::select_neighbors(
    const Vector& /*q*/, const std::vector<SearchHit>& cand, int mm) const {
  // Naechste-Nachbarn-Heuristik (dist, id deterministisch). Einfache
  // Closest-Auswahl reicht fuer uniformes Recall-Ziel; HNSW-Heuristik
  // (diversity) als Follow-up moeglich.
  std::vector<SearchHit> s = cand;
  if (static_cast<int>(s.size()) > mm) {
    std::nth_element(s.begin(), s.begin() + mm, s.end(),
                     [](const SearchHit& a, const SearchHit& b) {
                       if (a.dist != b.dist) return a.dist < b.dist;
                       return a.id < b.id;
                     });
    s.resize(mm);
  }
  std::sort(s.begin(), s.end(), [](const SearchHit& a, const SearchHit& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.id < b.id;
  });
  std::vector<int> out;
  out.reserve(s.size());
  for (auto& h : s) out.push_back(h.id);
  return out;
}

void HnswIndex::shrink_layer(int id, int lc, int max_m) {
  auto& nb = links_[id][lc];
  if (static_cast<int>(nb.size()) <= max_m) return;
  std::vector<std::pair<float, int>> scored;
  scored.reserve(nb.size());
  for (int v : nb) scored.emplace_back(dist_to_stored(data_[id], v), v);
  std::nth_element(scored.begin(), scored.begin() + max_m, scored.end(),
                   [](const auto& a, const auto& b) {
                     if (a.first != b.first) return a.first < b.first;
                     return a.second < b.second;
                   });
  std::sort(scored.begin(), scored.begin() + max_m,
            [](const auto& a, const auto& b) {
              if (a.first != b.first) return a.first < b.first;
              return a.second < b.second;
            });
  nb.clear();
  for (int t = 0; t < max_m; ++t) nb.push_back(scored[t].second);
  std::sort(nb.begin(), nb.end(), [&](int a, int b) {
    const float da = dist_to_stored(data_[id], a);
    const float db = dist_to_stored(data_[id], b);
    if (da != db) return da < db;
    return a < b;
  });
}

std::vector<SearchHit> HnswIndex::search_layer(const Vector& q, int entry_id,
                                              int ef, int lc) const {
  // Beam-Search auf genau einem Layer lc (ungefiltert, fuer Build + Descent).
  using Cand = std::pair<float, int>;
  std::priority_queue<Cand, std::vector<Cand>, std::greater<Cand>> frontier;
  std::priority_queue<Cand> top;
  std::vector<char> visited(data_.size(), 0);
  const float d0 = dist_to_stored(q, entry_id);
  frontier.emplace(d0, entry_id);
  visited[entry_id] = 1;
  top.emplace(d0, entry_id);
  while (!frontier.empty()) {
    const auto [d_u, u] = frontier.top();
    if (static_cast<int>(top.size()) >= ef && d_u > top.top().first) break;
    frontier.pop();
    if (u < 0 || u >= static_cast<int>(links_.size())) continue;
    if (lc >= static_cast<int>(links_[u].size())) continue;
    for (int v : links_[u][lc]) {
      if (v < 0 || v >= static_cast<int>(data_.size())) continue;
      if (visited[v]) continue;
      visited[v] = 1;
      const float d_v = dist_to_stored(q, v);
      frontier.emplace(d_v, v);
      if (static_cast<int>(top.size()) < ef) {
        top.emplace(d_v, v);
      } else if (d_v < top.top().first ||
                 (d_v == top.top().first && v < top.top().second)) {
        top.pop();
        top.emplace(d_v, v);
      }
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
  return out;
}

int HnswIndex::greedy_closest(const Vector& q, int entry_id, int lc) const {
  int cur = entry_id;
  float cur_d = dist_to_stored(q, cur);
  bool improved = true;
  while (improved) {
    improved = false;
    if (cur < 0 || cur >= static_cast<int>(links_.size())) break;
    if (lc >= static_cast<int>(links_[cur].size())) break;
    for (int v : links_[cur][lc]) {
      const float d = dist_to_stored(q, v);
      if (d < cur_d || (d == cur_d && v < cur)) {
        cur_d = d;
        cur = v;
        improved = true;
      }
    }
  }
  return cur;
}

void HnswIndex::insert_node(int id) {
  const int lv = levels_[id];
  if (data_.size() == 1) {
    entry_ = id;
    max_level_ = lv;
    built_ = true;
    return;
  }
  int cur = entry_;
  // 1) Greedy-Descent von Top bis lv+1.
  for (int lc = max_level_; lc > lv; --lc) cur = greedy_closest(data_[id], cur, lc);
  // 2) Pro Layer <= lv: Kandidaten via Beam, bidirektional verlinken.
  const int top_lc = std::min(lv, max_level_);
  for (int lc = top_lc; lc >= 0; --lc) {
    auto cand = search_layer(data_[id], cur, ef_construction_, lc);
    // Eigenen Knoten aus Kandidaten entfernen (falls Entry==id unmoeglich,
    // aber Distanz-0-Artefakt vermeiden).
    cand.erase(std::remove_if(cand.begin(), cand.end(),
                              [&](const SearchHit& h) { return h.id == id; }),
               cand.end());
    const int max_m = (lc == 0) ? 2 * m_ : m_;
    auto sel = select_neighbors(data_[id], cand, max_m);
    // Mindestens 1 Nachbar garantieren (Graph nie isoliert): naechsten per
    // Scan, falls Beam leer (z.B. ef klein / Einfuege-Reihenfolge).
    if (sel.empty() && !cand.empty()) sel.push_back(cand.front().id);
    if (sel.empty()) {
      // Fallback: naechsten existierenden Knoten linear suchen (selten).
      int best = -1;
      float best_d = 0.0f;
      for (int j = 0; j < id; ++j) {
        const float d = dist_to_stored(data_[id], j);
        if (best < 0 || d < best_d) {
          best = j;
          best_d = d;
        }
      }
      if (best >= 0) sel.push_back(best);
    }
    links_[id][lc] = sel;
    for (int nb : sel) {
      auto& back = links_[nb][lc];
      if (std::find(back.begin(), back.end(), id) == back.end())
        back.push_back(id);
      shrink_layer(nb, lc, max_m);
    }
    if (!cand.empty()) cur = cand.front().id;
    else if (!sel.empty()) cur = sel.front();
  }
  if (lv > max_level_) {
    max_level_ = lv;
    entry_ = id;
  }
  built_ = true;
}

void HnswIndex::build() {
  const int n = static_cast<int>(data_.size());
  if (n == 0) {
    links_.clear();
    max_level_ = -1;
    built_ = true;
    return;
  }
  // Falls bereits vollstaendig gebaut (links_ deckt alle Knoten ab), No-Op.
  // Sonst: frisch deterministisch aufbauen (Level neu samplen mit fixem Seed
  // fuer reproduzierbare Tests), dann inkrementell einfuegen: O(N log N).
  bool complete = built_ && static_cast<int>(links_.size()) == n;
  if (complete) {
    bool ok = true;
    for (int i = 0; i < n && ok; ++i)
      if (static_cast<int>(links_[i].size()) != levels_[i] + 1) ok = false;
    if (ok) return;
  }
  // Deterministischer Neuaufbau: Seed zuruecksetzen, Level neu ziehen.
  rng_.seed(42);
  levels_.assign(n, 0);
  links_.clear();
  links_.reserve(n);
  for (int i = 0; i < n; ++i) {
    levels_[i] = random_level();
    links_.emplace_back(static_cast<size_t>(levels_[i]) + 1);
  }
  entry_ = 0;
  max_level_ = levels_[0];
  // Ersten Knoten initialisieren.
  built_ = true;
  if (n == 1) {
    entry_ = 0;
    return;
  }
  // Knoten 0: Entry.
  entry_ = 0;
  max_level_ = levels_[0];
  for (int i = 1; i < n; ++i) insert_node(i);
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
  if (id < 0 || id >= static_cast<int>(links_.size()))
    throw std::out_of_range("neighbor_count: bad id");
  if (links_[id].empty()) return 0;
  return links_[id][0].size();
}

float HnswIndex::dist(const Vector& a, const Vector& b) const {
  return dispatch_distance(a, b, metric_);
}

float HnswIndex::dist_to_stored(const Vector& q, int id) const {
  if (metric_ == DistanceMetric::L2) {
    return l2_distance(q, data_[id]);
  }
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
  if (!built_) return brute_force(query, k, filter);

  int ef_search = (ef <= 0) ? ef_default_ : ef;
  if (ef_search < k) ef_search = k;
  if (ef_search > n) ef_search = n;

  if (n <= 64) return brute_force(query, k, filter);

  // 1) Descent ueber obere Layer (ungefiltert) zum guten Layer0-Entry.
  int cur = entry_;
  for (int lc = max_level_; lc >= 1; --lc) cur = greedy_closest(query, cur, lc);

  // 2) Layer0-Beam mit gemeinsamer Filter-Evaluierung (s08-Semantik).
  using Cand = std::pair<float, int>;
  std::priority_queue<Cand, std::vector<Cand>, std::greater<Cand>> frontier;
  std::priority_queue<Cand> top;
  std::vector<char> visited(n, 0);

  auto consider = [&](int id, float d) {
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

  // Seeds: descendierter Entry + 3 fixe zusaetzliche (robust auf uniform).
  const int seeds[4] = {cur, n / 4, n / 2, (3 * n) / 4};
  for (int s : seeds) {
    if (s < 0 || s >= n || visited[s]) continue;
    visited[s] = 1;
    const float d = dist_to_stored(query, s);
    frontier.emplace(d, s);
    consider(s, d);
  }

  while (!frontier.empty()) {
    const auto [d_u, u] = frontier.top();
    if (static_cast<int>(top.size()) >= ef_search && d_u > top.top().first)
      break;
    frontier.pop();
    if (u < 0 || u >= static_cast<int>(links_.size())) continue;
    if (links_[u].empty()) continue;
    for (int v : links_[u][0]) {
      if (v < 0 || v >= n || visited[v]) continue;
      visited[v] = 1;
      const float d_v = dist_to_stored(query, v);
      frontier.emplace(d_v, v);
      consider(v, d_v);
    }
  }

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
