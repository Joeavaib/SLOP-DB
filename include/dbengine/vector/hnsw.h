#pragma once

// s08-vector: filterbarer HNSW-lite (Layer-0) + Brute-Force Baseline.
//
// Design (Differenzierungs-Feature, Sorgfalt):
//   - VECTOR Typ: dbengine::vector::Vector = std::vector<float> (float32, C++20).
//   - Distanz: L2 (euklidisch) + Cosine (1 - cos). Dispatch via DistanceMetric,
//     zur Laufzeit umschaltbar, pro Query gleich.
//   - HNSW-lite: Single-Layer (Layer0), jeder Knoten max. M=16 Nachbarn
//     (fully-connected k=16 Graph, exakt via Brute-Force beim build()).
//     Greedy/Beam-Search mit ef (runtime-tunbar pro search()-Call).
//     Kein FAISS, keine externen Deps.
//   - Filter+ANN gemeinsam (kein Pre/Post-Filter):
//     search() evaluiert filter_fn waehrend der Graph-Traversierung in EINEM
//     Durchgang. Unpassende Knoten werden trotzdem expandiert (Frontier),
//     aber nur passende in die Top-(ef)->Top-k Ergebnismenge aufgenommen.
//     Dadurch bleibt der Graph auch bei selektiven Filtern traversierbar
//     (kein Disconnect wie bei Pre-Filter-Subgraph, kein Recall-Verlust wie
//     bei Post-Filter). Terminierung: Beam stoppt erst, wenn die naechste
//     Frontier-Distanz schlechter als das schlechteste gefilterte Ergebnis
//     ist UND ef gefilterte Treffer vorliegen.
//   - Quantisierung: SQ8-Stub (siehe unten). V1 rechnet float32 exakt;
//     der Stub reserviert das Interface fuer Scalar-Quantization (int8 + ADC)
//     + Disk-Tiering (s10-columnar).
//
// Thread-Safety: Lese-Operationen (search/brute_force) sind const & neben-
// laeufig sicher nach build(). add()/build() sind nicht nebenlaeufig sicher.

#include <cstddef>
#include <functional>
#include <random>
#include <vector>

namespace dbengine::vector {

using Vector = std::vector<float>;

// Filter-Praedikat ueber interne IDs (0..size()-1). nullptr = kein Filter.
using FilterFn = std::function<bool(int)>;

enum class DistanceMetric { L2, Cosine };

struct SearchHit {
  int id = -1;
  float dist = 0.0f;  // L2: euklidische Distanz; Cosine: 1 - cos_sim in [0,2]
};

// --- Distanzfunktionen (exakt, float32) ------------------------------------
float l2_squared(const Vector& a, const Vector& b);
float l2_distance(const Vector& a, const Vector& b);
float cosine_distance(const Vector& a, const Vector& b);
inline float dispatch_distance(const Vector& a, const Vector& b,
                               DistanceMetric m) {
  return (m == DistanceMetric::L2) ? l2_distance(a, b)
                                   : cosine_distance(a, b);
}
// Quadrierter Vergleichspfad (squared-hotpath): L2 ohne sqrt pro Kandidat
// (l2_squared-Basis), Cosine unveraendert (sqrt in Norm noetig).
// Ordnungssaequivalent zu dispatch_distance fuer L2 (sqrt monoton),
// inkl. deterministischem (dist,id)-Tie-Break auf quadrierter Basis.
inline float dispatch_distance2(const Vector& a, const Vector& b,
                                DistanceMetric m) {
  return (m == DistanceMetric::L2) ? l2_squared(a, b)
                                   : cosine_distance(a, b);
}

// --- AVX2-Distanzkerne mit Runtime-Dispatch (s-perf) --------------------------
// Rohe Zeiger-Kerne (float-Akkumulation): AVX2-Pfad wo verfuegbar, sonst
// skalarer Fallback. Auswahl einmalig via __builtin_cpu_supports (cached,
// thread-safe). Keine API-Aenderung: detail-Namespace, gleiche Semantik,
// nur schneller.
namespace detail {
struct DotNorms {
  float dot = 0.0f;
  float na = 0.0f;
  float nb = 0.0f;
};
float l2_squared_kernel(const float* a, const float* b, int n);
DotNorms dot_norms_kernel(const float* a, const float* b, int n);
}  // namespace detail

// ---------------------------------------------------------------------------
// SQ8-Quantisierung (STUB fuer V1).
// Idee (Qdrant/pgvector-Vorbild): pro Dimension min/max -> uint8, Suche via
// Asymmetric Distance Computation (ADC, Lookup-Tabellen), Re-Rank der Top-ef
// mit float32. Hier nur Interface + Doku, damit s10 (Disk-Tiering) und spaetere
// PQ/BQ/RQ-Schritte andocken koennen. Aktive Suche nutzt float32 exakt.
// ---------------------------------------------------------------------------
class SQ8Quantizer {
 public:
  struct Params {
    // Pro Dimension: scale = (max-min)/255, zero-point = min.
    Vector mins;
    Vector scales;  // size == dim, 0.0f => konstante Dimension
    bool fitted = false;
  };

  SQ8Quantizer() = default;

  // TODO(s08-followup): fit() ueber Dataset (min/max pro Dim),
  // encode(Vec)->vector<uint8_t>, decode(), ADC-Tabellen pro Query.
  // Aktuell: nicht implementiert, wirft bei Nutzung logic_error.
  void fit(const std::vector<Vector>& data);
  [[nodiscard]] bool fitted() const;

 private:
  Params params_;
};

// ---------------------------------------------------------------------------
// HnswIndex: echter mehrschichtiger HNSW (s21), abwaertskompatibel zu s08.
//   - Level-Sampling: l = floor(-ln(U) * mL), mL = 1/ln(M).
//   - M getrennt: Level0 Mmax0 = 2*M, obere Layer Mmax = M.
//   - efConstruction runtime-tunbar (Default 64), ef Suchzeit pro Query.
//   - Inkrementelles Insert: add() bei gebautem Index fuegt sofort ein
//     (kein O(N^2)-Rebuild); build() fuegt nur fehlende Knoten ein.
//   - Filter+ANN gemeinsam auf Layer0 (wie s08), obere Layer ungefiltert
//     als Entry-Navigation.
// ---------------------------------------------------------------------------
class HnswIndex {
 public:
  explicit HnswIndex(int dim, int m = 16, int ef_default = 32,
                     DistanceMetric metric = DistanceMetric::L2);

  // --- Konfiguration (runtime-tunbar, F4.3) ---
  void set_metric(DistanceMetric m);
  [[nodiscard]] DistanceMetric metric() const;
  void set_ef_default(int ef);
  [[nodiscard]] int ef_default() const;
  void set_m(int m);
  [[nodiscard]] int m() const;
  void set_ef_construction(int ef);
  [[nodiscard]] int ef_construction() const;
  void set_rng_seed(unsigned seed);
  [[nodiscard]] int max_level() const;
  [[nodiscard]] int level(int id) const;

  // --- Schreibpfad ---
  // Gibt interne ID (0..size()-1) zurueck. Dim-Mismatch -> invalid_argument.
  int add(const Vector& v);
  void clear();

  // Baut den Mehrschicht-Graphen inkrementell (O(N log N)): deterministisch
  // mit Seed 42, Level-Sampling, efConstruction-Beam pro Insert.
  // Nach build() ist sofortiges add() ohne Rebuild moeglich.
  void build();
  [[nodiscard]] bool built() const;

  // --- Lesepfad ---
  [[nodiscard]] size_t size() const;
  [[nodiscard]] int dim() const;
  [[nodiscard]] const Vector& get(int id) const;
  [[nodiscard]] size_t neighbor_count(int id) const;

  // ANN-Suche (Beam, Breite ef). ef<=0 => ef_default_. Gibt bis zu k Hits
  // (aufsteigend nach Distanz). Filter wird GEMEINSAM evaluiert (s.o.).
  [[nodiscard]] std::vector<SearchHit> search(const Vector& query, int k,
                                             int ef = -1,
                                             FilterFn filter = nullptr) const;

  // Exakte Baseline (Full-Scan) mit gleichem Filter- und Distanzverhalten.
  // Dient Recall-Messung (ANN vs. brute_force) und kleinen N.
  [[nodiscard]] std::vector<SearchHit> brute_force(
      const Vector& query, int k, FilterFn filter = nullptr) const;

 private:
  [[nodiscard]] float dist(const Vector& a, const Vector& b) const;
  [[nodiscard]] float dist_to_stored(const Vector& q, int id) const;
  // Quadrierter Hotpath: L2 -> l2_squared_kernel ohne sqrt (Beam-Heaps,
  // select/shrink-Scores, greedy); Cosine -> identisch zu dist_to_stored
  // (sqrt in Norm noetig, kein Einsparpotenzial). Finale search()-Outputs
  // werden einmalig nach sqrt konvertiert (dist-Einheiten).
  [[nodiscard]] float dist2_to_stored(const Vector& q, int id) const;
  // --- s21 Mehrschicht-Interna ---
  int random_level();
  [[nodiscard]] std::vector<SearchHit> search_layer(const Vector& q,
                                                   int entry_id, int ef,
                                                   int lc) const;
  int greedy_closest(const Vector& q, int entry_id, int lc) const;
  void insert_node(int id);
  // select_neighbors arbeitet in-place auf dem uebergebenen (owned)
  // Kandidatenvektor (keine Kopie-Flut): HNSW-Diversitaets-Heuristik
  // (Paper Alg. 3) — Kandidat nur, wenn naeher am Insert als an bereits
  // Gewaehlten, Rest-Auffuellung mit Naechsten bis mm. Rueckgabe der IDs
  // in Heuristik-Reihenfolge; cand wird dabei sortiert/gestutzt und auf
  // sel-Reihenfolge gebracht (Score-Wiederverwendung im Insert).
  [[nodiscard]] std::vector<int> select_neighbors(
      const Vector& q, std::vector<SearchHit>& cand, int mm) const;
  void shrink_layer(int id, int lc, int max_m);
  // Wie shrink_layer, nutzt aber den aus der Kandidatensuche bekannten Score
  // der frischen Kante (fresh_id, fresh_dist) statt ihn neu zu berechnen.
  void shrink_layer_after_add(int id, int lc, int max_m, int fresh_id,
                              float fresh_dist);

  int dim_;
  int m_;           // max. Nachbarn nominal (obere Layer)
  int ef_default_;  // Default-Beam-Breite, runtime-tunbar pro Query via ef
  int ef_construction_ = 64;
  DistanceMetric metric_;
  double ml_ = 0.36;  // 1/ln(M), bei set_m neu berechnet

  std::vector<Vector> data_;
  std::vector<float> norms_;  // L2-Normen (fuer Cosine, parallel zu data_)
  std::vector<int> levels_;   // Level pro Knoten
  // links_[id][lc] = Nachbarn auf Layer lc (lc <= levels_[id])
  std::vector<std::vector<std::vector<int>>> links_;
  int entry_ = 0;
  int max_level_ = -1;
  bool built_ = false;
  std::mt19937 rng_{42};
};

}  // namespace dbengine::vector
