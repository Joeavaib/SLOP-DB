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
// Sample-Regel: fit nutzt min(N, max(2048, N/10)) Samples (deterministisch
// via seed geshuffelt); alte Aufrufe ohne explizite Sample-Zahl profitieren
// automatisch (kleine N: alles; grosse N: 10 % statt fix 2048).
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

  // Codebook-Zugriff fuer Persistenz (DiskAnnIndex-Header): read-only Getter
  // plus validierender Setter (braucht init() vorher, setzt fitted_=true).
  // Additiv, keine Aenderung am k-means/ADC-Verhalten.
  [[nodiscard]] const std::vector<std::vector<Vector>>& codebooks() const;
  void set_codebooks(const std::vector<std::vector<Vector>>& cb);

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

// --- IVF-Coarse + Residual-PQ + Re-Rank (IVFADC-light) ----------------------
// Coarse-Quantizer: k-means ueber volle Vektoren (nlist Centroide,
// Sample-Regel min(N, max(2048, N/10)), deterministisch via seed).
// Residual-PQ: PQ wird auf Residuen (Daten - zugeordnetes Coarse-Zentroid)
// trainiert; inverted lists speichern pro Posting den Residual-Code.
// Suche: nprobe naechste Zellen -> pro Sonde ADC-Tabelle aus
// (Query - Sonden-Zentroid) -> Top-ef_rerank -> exaktes Re-Rank mit float32.
// Deterministisch (seed), STL-only.
//
// FORMAT-WECHSEL (Codes inkompatibel zu frueheren nicht-residualen Codes):
// Vorher: pq_.encode(vollvektor); Tabelle aus Query. Jetzt: pq_.encode(v -
// coarse[assign(v)]); Tabelle aus (query - coarse[sonde]). Alte Codes duerfen
// NICHT wiederverwendet werden (neu train()+build() noetig). Kein
// Persistenz-Format betroffen: Index ist in-memory (kein File-Layout,
// anders als DiskSpill); nur train/build/search muessen zusammenpassen.
// PqNQuantizer::decode(code) liefert daher eine Residual-Rekonstruktion
// (Vollvektor = coarse + decode), kein Vollvektor direkt.
class IvfPqIndex {
 public:
  struct Posting {
    int id = -1;
    std::vector<uint8_t> code;  // Residual-Code (s. Format-Wechsel oben)
  };

  // Default-Parameter (Faustregeln, dokumentiert):
  //   nlist  ~= 4*sqrt(N), gedeckelt auf [1, 4096] und <= N (N>=1).
  //   nprobe ~= nlist/8 (aufgerundet, mind. 1, max. nlist).
  // Hoeherer Recall-Bedarf -> nprobe groesser waehlen (z.B. nlist/4 bis
  // nlist/2); nprobe=nlist = Vollscan aller Listen (exaktes Re-Rank dominiert).
  struct IvfDefaults {
    int nlist = 0;
    int nprobe = 0;
  };
  static int default_nlist(size_t n);
  static int default_nprobe(int nlist);
  static IvfDefaults autotune(size_t n);

  IvfPqIndex() = default;
  IvfPqIndex(int dim, int num_subspaces, int nlist)
      : dim_(dim), nlist_(nlist), pq_(dim, num_subspaces) {}

  // Trainiert coarse (k-means, iters, Sample-Regel min(N, max(2048, N/10)))
  // + Residual-PQ auf (Daten - zugeordnetes Coarse-Zentroid). Danach build().
  void train(const std::vector<Vector>& data, int coarse_iters = 10,
             int pq_iters = 8, unsigned seed = 42u);
  // Baut inverted lists aus data (IDs = Position, Codes = Residuen).
  // Braucht train() vorher.
  void build(const std::vector<Vector>& data);
  [[nodiscard]] bool trained() const { return trained_; }
  [[nodiscard]] size_t size() const { return ntotal_; }
  [[nodiscard]] int nlist() const { return nlist_; }

  // Suche: nprobe Zellen, ADC-Coarse auf Residual-Tabellen
  // (Query - Sonden-Zentroid), Re-Rank exakt auf ef_rerank.
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

// --- DiskANN-Lite (Vektoren + PQ-Codes + Graph auf Disk, PQ-RAM, Re-Rank) --
// Layout (4 Files unter gemeinsamem Prefix, z.B. prefix="/tmp/ann"):
//   prefix.hdr   : Header{magic[8]="DBEANN1\\0", dim u64, m_pq u64,
//                  m_graph u64, count u64} + Codebooks float32 LE
//                  (m_pq x 256 x subdim[m] floats, subdims via
//                  PqNQuantizer-Regel base+rem aus dim/m_pq, Summe=dim,
//                  total 256*dim floats).
//   prefix.vec   : DiskSpill(dim, count) volle float32-Vektoren (Re-Rank).
//   prefix.pq    : DiskSpill(m_pq, count) PQ-Codes als float-Cast
//                  (Byte 0..255 -> float, exakt <2^24, Record=fix m_pq).
//   prefix.graph : DiskSpill(m_graph, count) Adjazenz als float-Cast
//                  (Nachbar-ID -> float, -1.0f = Padding, Record=fix m_graph).
// Graph: eigene Beam-Links = exakte M-NN (brute-force L2, (dist,id)-Tie-Break,
//   deterministisch, kein RNG; HnswIndex nur als Alternative dokumentiert,
//   nicht noetig -> kein Eingriff in hnsw.h). M (=m_graph) konfigurierbar.
// Suche: Beam ueber Graph mit PQ-ADC (mmap-Reads via DiskSpill::get, kein
//   decode, eine ADC-Tabelle pro Query), Top-ef_rerank exakt re-ranken
//   (volle Vektoren via DiskSpill). Deterministisch (Seed nur im PQ-fit).
class DiskAnnIndex {
 public:
  DiskAnnIndex() = default;
  DiskAnnIndex(int dim, int num_subspaces, int m_graph) {
    init(dim, num_subspaces, m_graph);
  }

  void init(int dim, int num_subspaces, int m_graph);

  // Baut Index aus data und persistiert unter prefix (+.hdr/.vec/.pq/.graph).
  // PQ-Training: pq_iters Lloyd-Iterationen, Sample-Regel aus PqNQuantizer,
  // deterministisch via seed. Graph: exakte M-NN (O(N^2), deterministisch).
  // Bleibt danach geoeffnet (mmap) fuer sofortige search().
  void build(const std::vector<Vector>& data, const std::string& prefix,
             int pq_iters = 8, unsigned seed = 42u);
  // Oeffnet bestehenden Index (Header + 3 DiskSpills, Codebooks aus Header).
  void open(const std::string& prefix);
  void close();

  [[nodiscard]] bool is_open() const { return open_; }
  [[nodiscard]] int dim() const { return dim_; }
  [[nodiscard]] int num_subspaces() const { return m_pq_; }
  [[nodiscard]] int m() const { return m_graph_; }
  [[nodiscard]] size_t size() const { return count_; }
  [[nodiscard]] const PqNQuantizer& pq() const { return pq_; }

  [[nodiscard]] std::vector<int> neighbors(int id) const;
  [[nodiscard]] Vector get_vector(int id) const;
  [[nodiscard]] std::vector<uint8_t> get_code(int id) const;

  // Beam (Breite beam) mit ADC, Re-Rank exakt auf ef_rerank.
  // beam/ef_rerank werden auf [k, N] geclampt; Rueckgabe Top-k exakt sortiert.
  [[nodiscard]] std::vector<SearchHit> search(const Vector& query, int k,
                                             int beam = 64,
                                             int ef_rerank = 100) const;

 private:
  [[nodiscard]] std::vector<uint8_t> read_code_row(int id) const;
  [[nodiscard]] std::vector<int> read_adj_row(int id) const;
  [[nodiscard]] float adc_of(int id,
                             const PqNQuantizer::AdcTable& table) const;

  int dim_ = 0;
  int m_pq_ = 0;
  int m_graph_ = 0;
  size_t count_ = 0;
  int entry_ = 0;
  bool open_ = false;
  std::string prefix_;
  PqNQuantizer pq_;
  DiskSpill vec_;
  DiskSpill codes_;
  DiskSpill graph_;
};

// --- Gross-Pfad-Empfehlung (HNSW vs. IVF, skaliert Recall/Latenz) --------
// Gemessen (uniform, k=10, HNSW Default-ef fix): Recall@10 0,94 (2k) ->
// 0,78 (8k) -> 0,34 (100k). HnswIndex::search() nutzt daher seitdem einen
// N-abhaengigen Autotune-Default (auto_ef: ceil(k*sqrt(N)/10), Floor
// ef_default, Cap 1024; kalibriert auf N~=1k ohne Overhead; 100k->ef~317,
// Recall-Erwartung >=0,8 bei ~10x Beam-Latenz). Fuer Latenz/Speicher bei
// grossen N trotzdem IVF bevorzugen:
//   ab N >= kHnswToIvfThreshold (50k): IvfPqIndex mit IvfPqIndex::autotune(N)
//   (nlist ~= 4*sqrt(N), nprobe = nlist/8) statt HNSW.
// Helper prefer_ivf_over_hnsw(n) verdrahtet genau diese Schwelle (deterministisch,
// O(1)); kein API-Bruch (rein additiv, keine Aenderung an HnswIndex-Signaturen).
constexpr size_t kHnswToIvfThreshold = 50000;
inline bool prefer_ivf_over_hnsw(size_t n) noexcept {
  return n >= kHnswToIvfThreshold;
}

// --- ef-Autotune ------------------------------------------------------------
// Waehlt ef nach k und Selektivitaet (Anteil passender Filter).
// Formel: ef = ceil(k / sel * kOverprovision=2.0), mindestens max(k, ef_min),
// gedeckelt auf ef_max bzw. n (falls n>=0). sel wird auf [1e-4, 1.0] geclampt.
// Hinweis: N-unabhaengig (bei sel=1 konstant 2k) — fuer UNGEFILTERTE HNSW-
// Suche skaliert HnswIndex::auto_ef zusaetzlich mit sqrt(N) (s. Gross-Pfad
// oben); bei FILTER selektivitaetsbedingt hiermit explizites ef waehlen
// (autotune_ef_for_filter) und an search(q,k,ef,filter) uebergeben.
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
