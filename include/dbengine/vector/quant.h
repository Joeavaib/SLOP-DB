#pragma once

// s16-quant: Vektor-Scale — SQ8 echt, PQ-Stub (2 Subspaces), Disk-Spill (mmap),
// ef-Autotune. Erweitert den Vektor-Layer, bricht hnsw.h NICHT (eigener
// Namespace dbengine::vector::quant, keine Aenderung an hnsw.h).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dbengine/vector/hnsw.h"

namespace dbengine::vector::quant {

// --- SQ8 echt (min/max pro dim, uint8 + ADC) -------------------------------
class Sq8Quantizer {
 public:
  Sq8Quantizer() = default;
  explicit Sq8Quantizer(int dim) : dim_(dim) {}

  void fit(const std::vector<Vector>& data);
  [[nodiscard]] bool fitted() const { return fitted_; }
  [[nodiscard]] int dim() const { return dim_; }
  [[nodiscard]] const Vector& mins() const { return mins_; }
  [[nodiscard]] const Vector& scales() const { return scales_; }

  [[nodiscard]] std::vector<uint8_t> encode(const Vector& v) const;
  [[nodiscard]] Vector decode(const std::vector<uint8_t>& code) const;
  [[nodiscard]] std::vector<std::vector<uint8_t>> encode_all(
      const std::vector<Vector>& data) const;

  // Asymmetric Distance Computation: L2-squared zwischen float-Query und
  // quantisiertem Code, ohne vorheriges decode() (Lookup on-the-fly).
  [[nodiscard]] float adc_l2_squared(
      const Vector& query, const std::vector<uint8_t>& code) const;
  [[nodiscard]] float adc_l2_distance(
      const Vector& query, const std::vector<uint8_t>& code) const;

  // Max. absoluter Elementfehler |x - decode(encode(x))| ueber Dataset.
  [[nodiscard]] float max_abs_error(
      const std::vector<Vector>& data) const;

 private:
  int dim_ = 0;
  Vector mins_;
  Vector scales_;  // (max-min)/255, 0.0f => konstante Dimension
  bool fitted_ = false;
};
using SQ8Quantizer = Sq8Quantizer;

// --- PQ-Stub (2 Subspaces, je 256 Centroide, k-means light) ----------------
class PqQuantizer {
 public:
  static constexpr int kSubspaces = 2;
  static constexpr int kCentroids = 256;
  using Code = std::array<uint8_t, kSubspaces>;

  PqQuantizer() = default;
  explicit PqQuantizer(int dim) : dim_(dim) { setup_subdims(); }

  // Light k-means: init via Random-Sample, `iters` Lloyd-Iterationen ueber
  // max. 2048 Samples (deterministisch via seed). dim muss gerade teilbar
  // sein oder Rest geht in letzte Subspace.
  void fit(const std::vector<Vector>& data, int iters = 8,
           unsigned seed = 42u);
  [[nodiscard]] bool fitted() const { return fitted_; }
  [[nodiscard]] int dim() const { return dim_; }

  [[nodiscard]] Code encode(const Vector& v) const;
  [[nodiscard]] Vector decode(const Code& code) const;
  [[nodiscard]] float adc_l2_squared(const Vector& query,
                                     const Code& code) const;

 private:
  void setup_subdims();
  [[nodiscard]] Vector subvec(const Vector& v, int m) const;

  int dim_ = 0;
  int subdims_[kSubspaces] = {0, 0};
  int suboffs_[kSubspaces] = {0, 0};
  // codebooks_[m][c][d_sub]
  std::vector<Vector> codebooks_[kSubspaces];
  bool fitted_ = false;
};

// --- PQ-N echt (M Subspaces konfigurierbar, je 256 Centroide) --------------
// Abwaertskompatibel: PqQuantizer (M=2 fix) bleibt erhalten. Neu: PqNQuantizer
// mit M=1..32 (empfohlen 2/4/8/16), echten ADC-Lookup-Tabellen
// (build_adc_table pro Query, O(M*256*subdim)), deterministischem k-means.
class PqNQuantizer {
 public:
  static constexpr int kCentroids = 256;

  PqNQuantizer() = default;
  PqNQuantizer(int dim, int num_subspaces) { init(dim, num_subspaces); }

  void init(int dim, int num_subspaces);
  void fit(const std::vector<Vector>& data, int iters = 8,
           unsigned seed = 42u);
  [[nodiscard]] bool fitted() const { return fitted_; }
  [[nodiscard]] int dim() const { return dim_; }
  [[nodiscard]] int num_subspaces() const { return m_; }

  [[nodiscard]] std::vector<uint8_t> encode(const Vector& v) const;
  [[nodiscard]] Vector decode(const std::vector<uint8_t>& code) const;

  // ADC-Tabelle: table[m][c] = ||q_sub(m) - centroid(m,c)||^2.
  using AdcTable = std::vector<std::vector<float>>;
  [[nodiscard]] AdcTable build_adc_table(const Vector& query) const;
  [[nodiscard]] float adc_with_table(const AdcTable& t,
                                     const std::vector<uint8_t>& code) const;
  [[nodiscard]] float adc_l2_squared(const Vector& query,
                                     const std::vector<uint8_t>& code) const;

 private:
  void setup_subdims();
  [[nodiscard]] Vector subvec(const Vector& v, int m) const;

  int dim_ = 0;
  int m_ = 0;
  std::vector<int> subdims_;
  std::vector<int> suboffs_;
  // codebooks_[m][c] = Zentroid (Laenge subdims_[m])
  std::vector<std::vector<Vector>> codebooks_;
  bool fitted_ = false;
};

// --- IVF-Coarse + PQ + Re-Rank (IVFADC-light, ohne Residual-Training) --------
// Coarse-Quantizer: k-means ueber volle Vektoren (nlist Centroide).
// Inverted Lists: pro Coarse-Zelle alle (id, pq-code).
// Suche: nprobe naechste Zellen -> ADC-Score via PQ-Tabelle -> Top-ef_rerank
// -> exaktes Re-Rank mit float32. Deterministisch (seed), STL-only.
class IvfPqIndex {
 public:
  struct Posting {
    int id = -1;
    std::vector<uint8_t> code;
  };

  IvfPqIndex() = default;
  IvfPqIndex(int dim, int num_subspaces, int nlist)
      : dim_(dim), nlist_(nlist), pq_(dim, num_subspaces) {}

  // Trainiert coarse (k-means, iters) + PQ auf data. Danach build().
  void train(const std::vector<Vector>& data, int coarse_iters = 10,
             int pq_iters = 8, unsigned seed = 42u);
  // Baut inverted lists aus data (IDs = Position). Braucht train() vorher.
  void build(const std::vector<Vector>& data);
  [[nodiscard]] bool trained() const { return trained_; }
  [[nodiscard]] size_t size() const { return ntotal_; }
  [[nodiscard]] int nlist() const { return nlist_; }

  // Suche: nprobe Zellen, ADC-Coarse, Re-Rank exakt auf ef_rerank.
  // base_data: volle Vektoren fuer Re-Rank (muss zu build()-IDs passen).
  [[nodiscard]] std::vector<SearchHit> search(
      const Vector& query, const std::vector<Vector>& base_data, int k,
      int nprobe = 8, int ef_rerank = 200) const;

  // Zugriff fuer Diagnose/Bench.
  [[nodiscard]] const std::vector<Vector>& coarse() const { return coarse_; }
  [[nodiscard]] const PqNQuantizer& pq() const { return pq_; }
  [[nodiscard]] size_t list_size(int c) const { return invlists_[c].size(); }

 private:
  [[nodiscard]] std::vector<int> pick_probes(const Vector& query,
                                             int nprobe) const;

  int dim_ = 0;
  int nlist_ = 0;
  PqNQuantizer pq_;
  std::vector<Vector> coarse_;
  std::vector<std::vector<Posting>> invlists_;
  size_t ntotal_ = 0;
  bool trained_ = false;
  bool built_ = false;
};

// --- DiskSpill (mmap file fuer Vektoren, float32) ---------------------------
// Dateilayout: Header{magic[8], dim u64, count u64} + count*dim floats (LE).
// create(): Datei anlegen + RW-mmap; open(): existierende Datei RO/RW-mmapen.
class DiskSpill {
 public:
  DiskSpill() = default;
  ~DiskSpill() { close(); }
  DiskSpill(const DiskSpill&) = delete;
  DiskSpill& operator=(const DiskSpill&) = delete;
  DiskSpill(DiskSpill&& other) noexcept;
  DiskSpill& operator=(DiskSpill&& other) noexcept;

  void create(const std::string& path, int dim, size_t count);
  void open(const std::string& path);
  void close();
  void sync() const;

  [[nodiscard]] bool is_open() const { return base_ != nullptr; }
  [[nodiscard]] int dim() const { return dim_; }
  [[nodiscard]] size_t size() const { return count_; }

  void set(size_t id, const Vector& v);
  [[nodiscard]] Vector get(size_t id) const;

 private:
  void* base_ = nullptr;
  size_t mapped_len_ = 0;
  int fd_ = -1;
  int dim_ = 0;
  size_t count_ = 0;
  float* payload_ = nullptr;
};

// --- ef-Autotune ------------------------------------------------------------
// Waehlt ef nach k und Selektivitaet (Anteil passender Filter).
// Formel: ef = ceil(k / sel * kOverprovision=2.0), mindestens max(k, ef_min),
// gedeckelt auf ef_max bzw. n (falls n>=0). sel wird auf [1e-4, 1.0] geclampt.
int autotune_ef(int k, double selectivity, int n = -1, int ef_min = 32,
                int ef_max = 1024);
// Komfort: Selektivitaet aus matched/total ableiten.
int autotune_ef_for_filter(int k, size_t matched, size_t total, int n = -1,
                           int ef_min = 32, int ef_max = 1024);

// --- Quantisierte Suche: ADC-Coarse + exaktes Re-Rank -----------------------
// Gibt Top-k IDs (nach exakter Distanz) zurueck: coarse via SQ8-ADC auf
// ef_rerank Kandidaten, Re-Rank mit float32. data/codes muessen korrespondieren.
std::vector<SearchHit> quantized_search_rerank(
    const Vector& query, const std::vector<Vector>& data,
    const std::vector<std::vector<uint8_t>>& codes, const Sq8Quantizer& q,
    int k, int ef_rerank = 100);

}  // namespace dbengine::vector::quant
