// s21-vector: echter mehrschichtiger HNSW + Brute-Force Baseline.
// Abwaertskompatibel zu s08 (gleiche public API + neue Tunables).

#include "dbengine/vector/hnsw.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <utility>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

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

// --- Visited-Epochen (Overhead-Optimierung, keine Semantik-Aenderung) --------
// search_layer() laeuft pro Insert pro Layer; ein visited-Vektor mit
// O(n)-Nullinitialisierung waere O(N^2) ueber den Build, ebenso pro Query.
// Stattdessen traegt jeder Knoten eine Epochen-Marke; pro Call wird nur ein
// Zaehler erhoeht (amortisiert O(1), keine Allokation nach Warmup).
// thread_local: keine Member-Aenderung, const-Leser bleiben nebenlaeufig
// sicher, Traversierungsreihenfolge bit-identisch zum alten Code.
struct VisitedPool {
  std::vector<int> tag;
  int cur = 0;
};

inline VisitedPool& visited_pool() {
  thread_local VisitedPool pool;
  return pool;
}

// Bereitet Marken fuer n Knoten vor; Aufrufer prueft `tags[v] == mark` und
// markiert via `tags[v] = mark`.
inline std::pair<int*, int> acquire_visited(int n) {
  VisitedPool& pool = visited_pool();
  if (static_cast<int>(pool.tag.size()) < n) pool.tag.resize(n, 0);
  if (pool.cur == std::numeric_limits<int>::max()) {
    std::fill(pool.tag.begin(), pool.tag.end(), 0);
    pool.cur = 0;
  }
  ++pool.cur;
  return {pool.tag.data(), pool.cur};
}

}  // namespace

// --- AVX2-Distanzkerne mit Runtime-Dispatch ---------------------------------
// Design: je ein skalarer Fallback (float-Akkumulation, gleiche Schleifen-
// assoziierung wie zuvor) + ein AVX2-Pfad (8 floats/Iter, unaligned loads,
// Horizontal-Summe via _mm256-Extract, Rest skalar). Die AVX2-Funktionen
// tragen __attribute__((target("avx2"))), sodass kein -march-Flag noetig ist.
// Auswahl einmalig via __builtin_cpu_supports("avx2"), gecacht in einem
// Function-local static (C++11: thread-safe Initialisierung).
namespace detail {
namespace {

float l2_scalar(const float* a, const float* b, int n) {
  float acc = 0.0f;
  for (int i = 0; i < n; ++i) {
    const float d = a[i] - b[i];
    acc += d * d;
  }
  return acc;
}

DotNorms dot_norms_scalar(const float* a, const float* b, int n) {
  float dot = 0.0f, na = 0.0f, nb = 0.0f;
  for (int i = 0; i < n; ++i) {
    dot += a[i] * b[i];
    na += a[i] * a[i];
    nb += b[i] * b[i];
  }
  return {dot, na, nb};
}

#if defined(__x86_64__)
__attribute__((target("avx2"))) float hsum_m256(__m256 v) {
  const __m128 lo = _mm256_castps256_ps128(v);
  const __m128 hi = _mm256_extractf128_ps(v, 1);
  __m128 s = _mm_add_ps(lo, hi);
  __m128 sh = _mm_movehl_ps(s, s);
  s = _mm_add_ps(s, sh);
  sh = _mm_shuffle_ps(s, s, 1);
  s = _mm_add_ss(s, sh);
  return _mm_cvtss_f32(s);
}

__attribute__((target("avx2"))) float l2_avx2(const float* a, const float* b,
                                              int n) {
  __m256 vsum = _mm256_setzero_ps();
  int i = 0;
  const int n8 = n & ~7;
  for (; i < n8; i += 8) {
    const __m256 va = _mm256_loadu_ps(a + i);
    const __m256 vb = _mm256_loadu_ps(b + i);
    const __m256 d = _mm256_sub_ps(va, vb);
    // Absichtlich mul+add statt FMA: gleiche Assoziierung wie skalar.
    vsum = _mm256_add_ps(vsum, _mm256_mul_ps(d, d));
  }
  float acc = hsum_m256(vsum);
  for (; i < n; ++i) {
    const float d = a[i] - b[i];
    acc += d * d;
  }
  return acc;
}

__attribute__((target("avx2"))) DotNorms
dot_norms_avx2(const float* a, const float* b, int n) {
  __m256 vdot = _mm256_setzero_ps();
  __m256 vna = _mm256_setzero_ps();
  __m256 vnb = _mm256_setzero_ps();
  int i = 0;
  const int n8 = n & ~7;
  for (; i < n8; i += 8) {
    const __m256 va = _mm256_loadu_ps(a + i);
    const __m256 vb = _mm256_loadu_ps(b + i);
    vdot = _mm256_add_ps(vdot, _mm256_mul_ps(va, vb));
    vna = _mm256_add_ps(vna, _mm256_mul_ps(va, va));
    vnb = _mm256_add_ps(vnb, _mm256_mul_ps(vb, vb));
  }
  DotNorms out{hsum_m256(vdot), hsum_m256(vna), hsum_m256(vnb)};
  for (; i < n; ++i) {
    out.dot += a[i] * b[i];
    out.na += a[i] * a[i];
    out.nb += b[i] * b[i];
  }
  return out;
}
#endif

[[maybe_unused]] bool use_avx2() {
  static const bool v = [] {
#if defined(__x86_64__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
#else
    return false;
#endif
  }();
  return v;
}

}  // namespace

float l2_squared_kernel(const float* a, const float* b, int n) {
  if (a == nullptr || b == nullptr || n <= 0) return 0.0f;
#if defined(__x86_64__)
  if (use_avx2()) return l2_avx2(a, b, n);
#endif
  return l2_scalar(a, b, n);
}

DotNorms dot_norms_kernel(const float* a, const float* b, int n) {
  if (a == nullptr || b == nullptr || n <= 0) return {};
#if defined(__x86_64__)
  if (use_avx2()) return dot_norms_avx2(a, b, n);
#endif
  return dot_norms_scalar(a, b, n);
}

}  // namespace detail

float l2_squared(const Vector& a, const Vector& b) {
  if (a.size() != b.size()) throw std::invalid_argument("l2: dim mismatch");
  return detail::l2_squared_kernel(a.data(), b.data(),
                                   static_cast<int>(a.size()));
}

float l2_distance(const Vector& a, const Vector& b) {
  return static_cast<float>(std::sqrt(static_cast<double>(l2_squared(a, b))));
}

float cosine_distance(const Vector& a, const Vector& b) {
  if (a.size() != b.size())
    throw std::invalid_argument("cosine: dim mismatch");
  const int n = static_cast<int>(a.size());
  const detail::DotNorms dn =
      detail::dot_norms_kernel(a.data(), b.data(), n);
  if (dn.na == 0.0f || dn.nb == 0.0f) return 1.0f;
  const double cos_sim = static_cast<double>(dn.dot) /
                         (std::sqrt(static_cast<double>(dn.na)) *
                          std::sqrt(static_cast<double>(dn.nb)));
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
    const Vector& /*q*/, std::vector<SearchHit>& cand, int mm) const {
  // HNSW select-neighbors-heuristic (Paper Alg. 3, Diversitaet):
  // cand = (dist zum Insert, id). Nach (dist, id) sortiert wird ein Kandidat
  // nur aufgenommen, wenn er naeher am Insert liegt als an allen bereits
  // Gewaehlten, d.h. dist(q,e) <= min_s dist(e,s). Sonst dupliziert er nur
  // eine bereits abgedeckte Richtung (redundante Kante). Unterdeckung wird
  // mit den naechsten Verworfenen aufgefuellt, sodass Grad-Caps (mm) und
  // Kantenzahl erhalten bleiben. Effekt: gleiche Kantenanzahl, aber
  // richtungs-divers -> kuerzere Navigationspfade, weniger Beam-Expansion
  // pro Query bei gleichem Recall. Die zusaetzlichen dist(e,s)-Rechnungen
  // fallen nur beim Build an, nicht pro Query.
  // Deterministisch: sortierte Iteration, strikter <-Vergleich (Tie behalt
  // den Kandidaten), gleiche Metrik wie die Suche. Level-Sampling, Seed-42-
  // Reihenfolge und Grad-Caps bleiben unberuehrt.
  // In-place auf dem owned Kandidatenvektor des Aufrufers (keine Kopie-Flut).
  // Danach gilt: cand[k] == (sel[k], dist(neu, sel[k])) in Heuristik-
  // Reihenfolge, sodass insert_node() die Scores weiterverwenden kann.
  // Achtung: reorders cand (Aufrufer sichert Bedarf vorher).
  if (mm <= 0 || cand.empty()) {
    cand.clear();
    return {};
  }
  std::sort(cand.begin(), cand.end(), [](const SearchHit& a, const SearchHit& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.id < b.id;
  });
  if (static_cast<int>(cand.size()) <= mm) {
    std::vector<int> out;
    out.reserve(cand.size());
    for (const auto& h : cand) out.push_back(h.id);
    return out;
  }
  const std::vector<SearchHit> sorted = cand;
  std::vector<SearchHit> picked;
  picked.reserve(static_cast<size_t>(mm));
  std::vector<char> is_picked(sorted.size(), 0);
  for (size_t i = 0; i < sorted.size() && static_cast<int>(picked.size()) < mm;
       ++i) {
    bool keep = true;
    for (const auto& p : picked) {
      const float d_es =
          dispatch_distance(data_[sorted[i].id], data_[p.id], metric_);
      if (d_es < sorted[i].dist) {
        keep = false;
        break;
      }
    }
    if (keep) {
      picked.push_back(sorted[i]);
      is_picked[i] = 1;
    }
  }
  // Auffuellen mit naechstem Rest (Grad erhalten, Recall-neutral).
  for (size_t i = 0; i < sorted.size() && static_cast<int>(picked.size()) < mm;
       ++i) {
    if (!is_picked[i]) {
      picked.push_back(sorted[i]);
      is_picked[i] = 1;
    }
  }
  cand = picked;
  std::vector<int> out;
  out.reserve(picked.size());
  for (const auto& h : picked) out.push_back(h.id);
  return out;
}

void HnswIndex::shrink_layer(int id, int lc, int max_m) {
  auto& nb = links_[id][lc];
  if (static_cast<int>(nb.size()) <= max_m) return;
  if (max_m <= 0) {
    nb.clear();
    return;
  }
  // Symmetrische Diversitaets-Heuristik (Paper Alg. 3, wie select_neighbors):
  // Reziproke Kanten duerfen nicht auf Top-mm-naechste gestutzt werden — das
  // loescht genau die Fernkanten, die Spread-Probes legen (geclustert
  // ef-Sweep flach). Stattdessen: ein Distanz-Scan mit gecachten Scores,
  // sortiert nach (dist, id), Kandidat nur wenn dist(q,e) <= min_s dist(e,s)
  // (strikter <-Vergleich: Tie behaelt den Kandidaten, deterministisch,
  // gleiche Metrik wie Suche/Insert), Auffuellung bis max_m mit naechsten
  // Verworfenen (Grad-Caps erhalten). Paar-Distanzen dist(e,s) nur beim Build.
  // nb danach in Heuristik-Reihenfolge (erste = naechste).
  std::vector<SearchHit> scored;
  scored.reserve(nb.size());
  for (int v : nb) scored.push_back({v, dist_to_stored(data_[id], v)});
  std::sort(scored.begin(), scored.end(), [](const SearchHit& a,
                                             const SearchHit& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.id < b.id;
  });
  std::vector<SearchHit> picked;
  picked.reserve(static_cast<size_t>(max_m));
  std::vector<char> is_picked(scored.size(), 0);
  for (size_t i = 0; i < scored.size() && static_cast<int>(picked.size()) < max_m;
       ++i) {
    bool keep = true;
    for (const auto& p : picked) {
      const float d_es =
          dispatch_distance(data_[scored[i].id], data_[p.id], metric_);
      if (d_es < scored[i].dist) {
        keep = false;
        break;
      }
    }
    if (keep) {
      picked.push_back(scored[i]);
      is_picked[i] = 1;
    }
  }
  for (size_t i = 0; i < scored.size() && static_cast<int>(picked.size()) < max_m;
       ++i) {
    if (!is_picked[i]) {
      picked.push_back(scored[i]);
      is_picked[i] = 1;
    }
  }
  nb.clear();
  nb.reserve(picked.size());
  for (const auto& h : picked) nb.push_back(h.id);
}

void HnswIndex::shrink_layer_after_add(int id, int lc, int max_m, int fresh_id,
                                       float fresh_dist) {
  auto& nb = links_[id][lc];
  if (static_cast<int>(nb.size()) <= max_m) return;
  if (max_m <= 0) {
    nb.clear();
    return;
  }
  // Wie shrink_layer (symmetrische Alg.-3-Heuristik), aber die frisch gelegte
  // Kante (id<->fresh_id) bringt ihren Score aus der Kandidatensuche mit
  // (Metrik symmetrisch) und wird nicht neu berechnet; Rest via
  // dist_to_stored (ein Scan, gecachte Scores).
  std::vector<SearchHit> scored;
  scored.reserve(nb.size());
  for (int v : nb) {
    if (v == fresh_id)
      scored.push_back({v, fresh_dist});
    else
      scored.push_back({v, dist_to_stored(data_[id], v)});
  }
  std::sort(scored.begin(), scored.end(), [](const SearchHit& a,
                                             const SearchHit& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.id < b.id;
  });
  std::vector<SearchHit> picked;
  picked.reserve(static_cast<size_t>(max_m));
  std::vector<char> is_picked(scored.size(), 0);
  for (size_t i = 0; i < scored.size() && static_cast<int>(picked.size()) < max_m;
       ++i) {
    bool keep = true;
    for (const auto& p : picked) {
      const float d_es =
          dispatch_distance(data_[scored[i].id], data_[p.id], metric_);
      if (d_es < scored[i].dist) {
        keep = false;
        break;
      }
    }
    if (keep) {
      picked.push_back(scored[i]);
      is_picked[i] = 1;
    }
  }
  for (size_t i = 0; i < scored.size() && static_cast<int>(picked.size()) < max_m;
       ++i) {
    if (!is_picked[i]) {
      picked.push_back(scored[i]);
      is_picked[i] = 1;
    }
  }
  nb.clear();
  nb.reserve(picked.size());
  for (const auto& h : picked) nb.push_back(h.id);
}

std::vector<SearchHit> HnswIndex::search_layer(const Vector& q, int entry_id,
                                              int ef, int lc) const {
  // Beam-Search auf genau einem Layer lc (ungefiltert, fuer Build + Descent).
  // Gleiche Traversierung wie zuvor, aber ohne Per-Call-Overhead:
  // Epochen-Visited (kein vector<char>(n)-Alloc/Zero) + Heap-Vektoren mit
  // Reserve statt zwei priority_queue-Objekten (gleiche push/pop-Heapfolge,
  // daher bit-identische Besuchsreihenfolge).
  using Cand = std::pair<float, int>;
  const int n = static_cast<int>(data_.size());
  if (n == 0 || entry_id < 0 || entry_id >= n) return {};
  auto [seen, mark] = acquire_visited(n);
  std::vector<Cand> frontier;
  frontier.reserve(static_cast<size_t>(2 * ef + 8));
  std::vector<Cand> top;
  top.reserve(static_cast<size_t>(ef + 1));
  const float d0 = dist_to_stored(q, entry_id);
  frontier.emplace_back(d0, entry_id);
  seen[entry_id] = mark;
  top.emplace_back(d0, entry_id);
  auto is_better = [](const Cand& a, const Cand& b) {
    if (a.first != b.first) return a.first < b.first;
    return a.second < b.second;
  };
  while (!frontier.empty()) {
    const float d_u = frontier.front().first;
    const int u = frontier.front().second;
    if (static_cast<int>(top.size()) >= ef && d_u > top.front().first) break;
    std::pop_heap(frontier.begin(), frontier.end(), std::greater<Cand>());
    frontier.pop_back();
    if (u < 0 || u >= static_cast<int>(links_.size())) continue;
    if (lc >= static_cast<int>(links_[u].size())) continue;
    for (int v : links_[u][lc]) {
      if (v < 0 || v >= n) continue;
      if (seen[v] == mark) continue;
      seen[v] = mark;
      const float d_v = dist_to_stored(q, v);
      frontier.emplace_back(d_v, v);
      std::push_heap(frontier.begin(), frontier.end(), std::greater<Cand>());
      if (static_cast<int>(top.size()) < ef) {
        top.emplace_back(d_v, v);
        std::push_heap(top.begin(), top.end(), std::less<Cand>());
      } else if (is_better(Cand(d_v, v), top.front())) {
        std::pop_heap(top.begin(), top.end(), std::less<Cand>());
        top.back() = Cand(d_v, v);
        std::push_heap(top.begin(), top.end(), std::less<Cand>());
      }
    }
  }
  std::vector<SearchHit> out;
  out.reserve(top.size());
  for (const auto& c : top) out.push_back({c.second, c.first});
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
    // Eigenen Knoten aus Kandidaten entfernen (defensiv; neuer Knoten hat
    // noch keine eingehenden Kanten und ist daher i.d.R. nicht enthalten).
    cand.erase(std::remove_if(cand.begin(), cand.end(),
                              [&](const SearchHit& h) { return h.id == id; }),
               cand.end());
    // Deterministische Spread-Probes gegen Cluster-Trapping (kein RNG):
    // Der Build-Beam erreicht Cross-Cluster-Knoten nie (Henne-Ei: ohne
    // existierende Fernkanten findet der Beam keine), also enthaelt der
    // Graph sonst keine Flucht-Kanten. K=8 index-gestreute bereits
    // existierende Knoten pid = id*(j+1)/(K+1) scoren, die besten
    // (naechsten) bis zu 3 davon, die nicht schon im Beam-Pool sind und
    // auf diesem Layer existieren, vor select_neighbors in den Pool
    // aufnehmen. Die diversifizierte Heuristik behaelt echte Fernkanten
    // (fern von bereits Gewaehlten => akzeptiert). Caps (max_m),
    // Level-Sampling, Suchpfad unveraendert. Kosten: <=K Distanzen/Layer.
    if (id > 0) {
      constexpr int kSpreadK = 8;
      constexpr int kSpreadTake = 3;
      int probe_ids[kSpreadK];
      int nprobe = 0;
      for (int j = 0; j < kSpreadK; ++j) {
        const int pid = static_cast<int>(
            (static_cast<long long>(id) * (j + 1)) / (kSpreadK + 1));
        if (pid < 0 || pid >= id) continue;
        if (pid >= static_cast<int>(levels_.size())) continue;
        if (levels_[pid] < lc) continue;  // nur auf diesem Layer existent
        bool dup = false;
        for (int t = 0; t < nprobe; ++t) {
          if (probe_ids[t] == pid) {
            dup = true;
            break;
          }
        }
        if (dup) continue;
        bool in_cand = false;
        for (const auto& h : cand) {
          if (h.id == pid) {
            in_cand = true;
            break;
          }
        }
        if (in_cand) continue;
        probe_ids[nprobe++] = pid;
      }
      if (nprobe > 0) {
        std::pair<float, int> scored[kSpreadK];
        for (int t = 0; t < nprobe; ++t)
          scored[t] = {dist_to_stored(data_[id], probe_ids[t]), probe_ids[t]};
        std::sort(scored, scored + nprobe, [](const auto& a, const auto& b) {
          if (a.first != b.first) return a.first < b.first;
          return a.second < b.second;
        });
        const int take = nprobe < kSpreadTake ? nprobe : kSpreadTake;
        for (int t = 0; t < take; ++t)
          cand.push_back(SearchHit{scored[t].second, scored[t].first});
      }
    }
    const int max_m = (lc == 0) ? 2 * m_ : m_;
    // Besten merken VOR select (select arbeitet in-place auf cand; der
    // Front-Eintrag ueberlebt die Stutzung, da die Auswahl die kleinsten
    // (dist, id) behaelt).
    const int best_cand = cand.empty() ? -1 : cand.front().id;
    // Danach gilt: cand[k] == (sel[k], dist(neu, sel[k])), (dist, id)-sortiert.
    auto sel = select_neighbors(data_[id], cand, max_m);
    // Mindestens 1 Nachbar garantieren (Graph nie isoliert): naechsten per
    // Scan, falls Beam leer (z.B. ef klein / Einfuege-Reihenfolge).
    if (sel.empty() && best_cand >= 0) sel.push_back(best_cand);
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
    for (size_t k = 0; k < sel.size(); ++k) {
      const int nb = sel[k];
      auto& back = links_[nb][lc];
      if (std::find(back.begin(), back.end(), id) == back.end())
        back.push_back(id);
      // Gecachten Score der frischen Kante mitschleppen, wo verfuegbar.
      if (k < cand.size() && cand[k].id == nb)
        shrink_layer_after_add(nb, lc, max_m, id, cand[k].dist);
      else
        shrink_layer(nb, lc, max_m);
    }
    if (best_cand >= 0) cur = best_cand;
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
  const Vector& b = data_[id];
  const int n = static_cast<int>(b.size());
  if (metric_ == DistanceMetric::L2) {
    if (q.size() != b.size())
      throw std::invalid_argument("l2: dim mismatch");
    const float d2 = detail::l2_squared_kernel(q.data(), b.data(), n);
    return static_cast<float>(std::sqrt(static_cast<double>(d2)));
  }
  // Cosine: dot + ||q||^2 aus einem Kernel-Durchgang, ||b||^2 aus dem
  // gecachten norms_-Eintrag (gleiche Formel wie zuvor: sqrt(nq*nb)).
  const detail::DotNorms dn =
      detail::dot_norms_kernel(q.data(), b.data(), n);
  const double nb = static_cast<double>(norms_[id]) * norms_[id];
  const double nq = static_cast<double>(dn.na);
  if (nq == 0.0 || nb == 0.0) return 1.0f;
  const double cos_sim = static_cast<double>(dn.dot) / std::sqrt(nq * nb);
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

  // 1) Deterministische Entry-Diversifizierung gegen Cluster-Trapping
  // (obere Layer ungefiltert): statt einem Single-Pfad-Descent vom Entry
  // werden 4 fix gestreute Starts (Entry + n/4, n/2, 3n/4 — reine
  // Index-Funktion aus n/entry_, kein RNG pro Query) je per greedy_closest
  // von Top bis Layer 1 abgestiegen; die besten 3 distinkten Kandidaten
  // (nach (dist, id)) werden Layer0-Seeds. Ein Pfad allein commitet sich auf
  // oberer (grober) Ebene auf ein falsches Cluster-Basin; der Layer0-Beam
  // mit ef-Abbruch (`d_u > worst`) kann danach das richtige Cluster nie
  // erreichen (Refill greift nur bei top<k). Diverse Basins stellen sicher,
  // dass (mindestens) ein Seed im Query-Cluster landet; uniform bleibt der
  // alte Entry-Pfad dabei (nur erweiterte Frontier, gleiche Abbruch- und
  // Filter-Semantik). Kosten: pro Zusatz-Start nur Upper-Layer-Greedy
  // (wenige Distanzrechnungen, obere Layer duenn) + 1 Scoring-Distanz.
  int descended[4] = {-1, -1, -1, -1};
  {
    const int raw[4] = {entry_, (entry_ + n / 4) % n, (entry_ + n / 2) % n,
                        (entry_ + (3 * n) / 4) % n};
    for (int i = 0; i < 4; ++i) {
      bool dup = false;
      for (int j = 0; j < i; ++j) {
        if (raw[j] == raw[i]) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      int c = raw[i];
      for (int lc = max_level_; lc >= 1; --lc)
        c = greedy_closest(query, c, lc);
      descended[i] = c;
    }
  }
  // Beste 3 distinkte Kandidaten nach (dist, id).
  std::pair<float, int> ranked[4];
  int nranked = 0;
  for (int i = 0; i < 4; ++i) {
    if (descended[i] < 0) continue;
    bool dup = false;
    for (int j = 0; j < i; ++j) {
      if (descended[j] == descended[i]) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    ranked[nranked++] = {dist_to_stored(query, descended[i]), descended[i]};
  }
  std::sort(ranked, ranked + nranked, [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;
    return a.second < b.second;
  });
  int seeds[3] = {-1, -1, -1};
  float seed_d[3] = {0.0f, 0.0f, 0.0f};
  int nseeds = nranked < 3 ? nranked : 3;
  for (int i = 0; i < nseeds; ++i) {
    seeds[i] = ranked[i].second;
    seed_d[i] = ranked[i].first;
  }
  // Kollaps-Fallback (defensiv, z.B. alle Descents konvergiert): rohen
  // Gegenueber zum besten Seed auffuellen, damit der Beam nie auf genau
  // einem Basin festhaengt. Reine Index-Funktion, deterministisch.
  if (nseeds >= 1 && nseeds < 3) {
    const int fb = (seeds[0] + n / 2) % n;
    bool known = false;
    for (int i = 0; i < nseeds; ++i) {
      if (seeds[i] == fb) {
        known = true;
        break;
      }
    }
    if (!known) {
      seeds[nseeds] = fb;
      seed_d[nseeds] = dist_to_stored(query, fb);
      ++nseeds;
    }
  }

  // 1b) Flache Coarse-Probes zur Query-Zeit (kein Descent, kein RNG).
  // Diagnose: Entry-Descent + Fallbacks kollabieren auf oberen duennen Layern
  // ins gleiche falsche Basin; der Layer0-Beam terminiert voll-aber-falsch
  // (ef-Abbruch greift, Refill nur bei top<k). P=256 deterministische
  // Spread-Knoten pid = (entry_ + j*n/P) % n (j=0..P-1, reine Index-Funktion)
  // werden flach per dist_to_stored gescort; die besten B=3 distinkten (nach
  // (dist, id), exkl. bestehender Seeds) kommen als zusaetzliche Layer0-Seeds
  // in die Union (Dedup final via visited-Epoche beim Einsetzen). Kein
  // greedy_closest fuer Probes: Upper-Layer sind zu duenn, um quer zu routen.
  // P=256, weil Cluster id-interleaved sind: Trefferquote aufs Query-Cluster
  // (156/10k) = 1-(1-0.0156)^256 ~= 98 %. Kosten: P SIMD-Distanzen/Query
  // (~10us, p95-SLO hat weiter >100x Headroom).
  // Bestehende Seeds/Descent/Filter/Refill/ef-Abbruch unveraendert.
  int probe_seeds[3] = {-1, -1, -1};
  float probe_d[3] = {0.0f, 0.0f, 0.0f};
  int nprobes = 0;
  {
    constexpr int kQueryProbeP = 256;
    constexpr int kQueryProbeB = 3;
    std::pair<float, int> scored[kQueryProbeP];
    int uniq[kQueryProbeP];
    int nuniq = 0;
    for (int j = 0; j < kQueryProbeP; ++j) {
      const int pid = static_cast<int>(
          (static_cast<long long>(entry_) +
           (static_cast<long long>(j) * n) / kQueryProbeP) %
          n);
      if (pid < 0 || pid >= n) continue;
      bool dup = false;
      for (int t = 0; t < nuniq; ++t) {
        if (uniq[t] == pid) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      uniq[nuniq++] = pid;
    }
    for (int t = 0; t < nuniq; ++t)
      scored[t] = {dist_to_stored(query, uniq[t]), uniq[t]};
    std::sort(scored, scored + nuniq, [](const auto& a, const auto& b) {
      if (a.first != b.first) return a.first < b.first;
      return a.second < b.second;
    });
    for (int t = 0; t < nuniq && nprobes < kQueryProbeB; ++t) {
      const int pid = scored[t].second;
      bool known = false;
      for (int s = 0; s < nseeds; ++s) {
        if (seeds[s] == pid) {
          known = true;
          break;
        }
      }
      for (int s = 0; s < nprobes && !known; ++s) {
        if (probe_seeds[s] == pid) known = true;
      }
      if (known) continue;
      probe_seeds[nprobes] = pid;
      probe_d[nprobes] = scored[t].first;
      ++nprobes;
    }
  }

  // 2) Layer0-Beam mit gemeinsamer Filter-Evaluierung (s08-Semantik).
  // Wie search_layer: Epochen-Visited + Heap-Vektoren mit Reserve.
  // Filter-Praedikat einmalig auf bool materialisiert (kein
  // std::function-Bool-Check pro Knoten).
  using Cand = std::pair<float, int>;
  auto [seen, mark] = acquire_visited(n);
  std::vector<Cand> frontier;
  frontier.reserve(static_cast<size_t>(2 * ef_search + 8));
  std::vector<Cand> top;
  top.reserve(static_cast<size_t>(ef_search + 1));
  const bool use_filter = static_cast<bool>(filter);

  auto consider = [&](int id, float d) {
    if (!use_filter || filter(id)) {
      if (static_cast<int>(top.size()) < ef_search) {
        top.emplace_back(d, id);
        std::push_heap(top.begin(), top.end(), std::less<Cand>());
      } else if (d < top.front().first ||
                 (d == top.front().first && id < top.front().second)) {
        std::pop_heap(top.begin(), top.end(), std::less<Cand>());
        top.back() = Cand(d, id);
        std::push_heap(top.begin(), top.end(), std::less<Cand>());
      }
    }
  };

  // Seeds: diversifizierte Descent-Ergebnisse (oben, max. 3) als
  // Layer0-Start-Frontier. Refill-Garantie unten unveraendert (liefert nie
  // < k, solange >= k Treffer existieren).
  for (int si = 0; si < nseeds; ++si) {
    const int s = seeds[si];
    if (s < 0 || s >= n || seen[s] == mark) continue;
    seen[s] = mark;
    const float d = seed_d[si];
    frontier.emplace_back(d, s);
    std::push_heap(frontier.begin(), frontier.end(), std::greater<Cand>());
    consider(s, d);
  }
  // Union mit flachen Coarse-Probes (1b, max. 3): gleiche Einsetz-Semantik,
  // Dedup via visited-Epoche (deckt auch Seeds-Ueberlappung ab). Filter-,
  // Abbruch- und Refill-Logik unveraendert.
  for (int pi = 0; pi < nprobes; ++pi) {
    const int s = probe_seeds[pi];
    if (s < 0 || s >= n || seen[s] == mark) continue;
    seen[s] = mark;
    const float d = probe_d[pi];
    frontier.emplace_back(d, s);
    std::push_heap(frontier.begin(), frontier.end(), std::greater<Cand>());
    consider(s, d);
  }

  while (!frontier.empty()) {
    const float d_u = frontier.front().first;
    const int u = frontier.front().second;
    // ef-Abbruch bewusst unveraendert (`>` statt `>=`): bei Gleichstand
    // (d_u == worst) kann der kleinere id top noch verbessern (Tie-Break
    // id). Ein `>=` waere nicht aequivalent und wuerde Recall/Tie-
    // Determinismus riskieren. Audit-Ergebnis: keine schaerfere Bedingung
    // beweisbar aequivalent -> Semantik bleibt, Speedup kommt aus
    // Heuristik-Graph + Seeds.
    if (static_cast<int>(top.size()) >= ef_search && d_u > top.front().first)
      break;
    std::pop_heap(frontier.begin(), frontier.end(), std::greater<Cand>());
    frontier.pop_back();
    if (u < 0 || u >= static_cast<int>(links_.size())) continue;
    if (links_[u].empty()) continue;
    for (int v : links_[u][0]) {
      if (v < 0 || v >= n || seen[v] == mark) continue;
      seen[v] = mark;
      const float d_v = dist_to_stored(query, v);
      frontier.emplace_back(d_v, v);
      std::push_heap(frontier.begin(), frontier.end(), std::greater<Cand>());
      consider(v, d_v);
    }
  }

  if (static_cast<int>(top.size()) < k) {
    for (int i = 0; i < n && static_cast<int>(top.size()) < ef_search; ++i) {
      if (seen[i] == mark) continue;
      seen[i] = mark;
      if (use_filter && !filter(i)) continue;
      top.emplace_back(dist_to_stored(query, i), i);
      std::push_heap(top.begin(), top.end(), std::less<Cand>());
    }
  }

  std::vector<SearchHit> out;
  out.reserve(top.size());
  for (const auto& c : top) out.push_back({c.second, c.first});
  std::sort(out.begin(), out.end(), [](const SearchHit& a, const SearchHit& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.id < b.id;
  });
  if (static_cast<int>(out.size()) > k) out.resize(k);
  return out;
}

}  // namespace dbengine::vector
