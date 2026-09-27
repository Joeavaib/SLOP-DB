// s16-quant Implementierung: SQ8 echt + PQ-Stub(2) + DiskSpill(mmap) +
// ef-Autotune + ADC-Rerank-Suche.

#include "dbengine/vector/quant.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace dbengine::vector::quant {

namespace {
constexpr char kMagic[8] = {'D', 'B', 'E', 'V', 'S', 'P', '1', '\0'};
struct FileHeader {
  char magic[8];
  uint64_t dim;
  uint64_t count;
};

float sub_l2_squared(const float* a, const float* b, int n) {
  // Heisser Pfad (PQ-Lloyd, IVF-Coarse, ADC-Tabellen): AVX2-Kern teilen.
  return dbengine::vector::detail::l2_squared_kernel(a, b, n);
}

// Sample-Budget (Prozent-Regel): min(N, max(2048, N/10)).
// Kleine N: alles; grosse N: 10 % statt fixem Cap. Deterministisch
// (Aufrufer shuffelt via Seed und nimmt die ersten nuse).
size_t sample_budget(size_t n) {
  const size_t ten_pct = n / 10;
  const size_t cap = std::max<size_t>(2048, ten_pct);
  return std::min(n, cap);
}
}  // namespace

// --- Sq8Quantizer -----------------------------------------------------------
void Sq8Quantizer::fit(const std::vector<Vector>& data) {
  if (data.empty()) throw std::invalid_argument("Sq8Quantizer::fit: empty");
  const int dim = static_cast<int>(data[0].size());
  if (dim <= 0) throw std::invalid_argument("Sq8Quantizer::fit: dim<=0");
  for (const auto& v : data)
    if (static_cast<int>(v.size()) != dim)
      throw std::invalid_argument("Sq8Quantizer::fit: dim mismatch");
  Vector mn(dim, std::numeric_limits<float>::infinity());
  Vector mx(dim, -std::numeric_limits<float>::infinity());
  for (const auto& v : data)
    for (int d = 0; d < dim; ++d) {
      mn[d] = std::min(mn[d], v[d]);
      mx[d] = std::max(mx[d], v[d]);
    }
  mins_ = mn;
  scales_.assign(dim, 0.0f);
  for (int d = 0; d < dim; ++d) {
    const float range = mx[d] - mn[d];
    scales_[d] = (range <= 0.0f) ? 0.0f : range / 255.0f;
  }
  dim_ = dim;
  fitted_ = true;
}

std::vector<uint8_t> Sq8Quantizer::encode(const Vector& v) const {
  if (!fitted_) throw std::logic_error("Sq8Quantizer::encode: not fitted");
  if (static_cast<int>(v.size()) != dim_)
    throw std::invalid_argument("Sq8Quantizer::encode: dim mismatch");
  std::vector<uint8_t> code(dim_);
  for (int d = 0; d < dim_; ++d) {
    if (scales_[d] == 0.0f) {
      code[d] = 0;
      continue;
    }
    float t = (v[d] - mins_[d]) / scales_[d];
    t = std::clamp(t, 0.0f, 255.0f);
    code[d] = static_cast<uint8_t>(std::lround(t));
  }
  return code;
}

Vector Sq8Quantizer::decode(const std::vector<uint8_t>& code) const {
  if (!fitted_) throw std::logic_error("Sq8Quantizer::decode: not fitted");
  if (static_cast<int>(code.size()) != dim_)
    throw std::invalid_argument("Sq8Quantizer::decode: dim mismatch");
  Vector v(dim_);
  for (int d = 0; d < dim_; ++d)
    v[d] = (scales_[d] == 0.0f) ? mins_[d]
                                : mins_[d] + scales_[d] * code[d];
  return v;
}

std::vector<std::vector<uint8_t>> Sq8Quantizer::encode_all(
    const std::vector<Vector>& data) const {
  std::vector<std::vector<uint8_t>> out;
  out.reserve(data.size());
  for (const auto& v : data) out.push_back(encode(v));
  return out;
}

float Sq8Quantizer::adc_l2_squared(const Vector& query,
                                   const std::vector<uint8_t>& code) const {
  if (!fitted_) throw std::logic_error("Sq8Quantizer::adc: not fitted");
  if (static_cast<int>(query.size()) != dim_ ||
      static_cast<int>(code.size()) != dim_)
    throw std::invalid_argument("Sq8Quantizer::adc: dim mismatch");
  double acc = 0.0;
  for (int d = 0; d < dim_; ++d) {
    const float dec =
        (scales_[d] == 0.0f) ? mins_[d] : mins_[d] + scales_[d] * code[d];
    const double diff = static_cast<double>(query[d]) - dec;
    acc += diff * diff;
  }
  return static_cast<float>(acc);
}

float Sq8Quantizer::adc_l2_distance(const Vector& query,
                                    const std::vector<uint8_t>& code) const {
  return static_cast<float>(std::sqrt(adc_l2_squared(query, code)));
}

float Sq8Quantizer::max_abs_error(const std::vector<Vector>& data) const {
  if (!fitted_) throw std::logic_error("Sq8Quantizer::max_abs_error: no fit");
  float mx = 0.0f;
  for (const auto& v : data) {
    Vector dec = decode(encode(v));
    for (int d = 0; d < dim_; ++d)
      mx = std::max(mx, std::fabs(v[d] - dec[d]));
  }
  return mx;
}

// --- PqQuantizer ------------------------------------------------------------
void PqQuantizer::setup_subdims() {
  if (dim_ <= 0) return;
  const int half = dim_ / kSubspaces;
  const int rest = dim_ - half * (kSubspaces - 1);
  // Erste M-1 Subspaces je `half`, letzter bekommt Rest (deckt ungerade ab).
  for (int m = 0; m < kSubspaces - 1; ++m) {
    subdims_[m] = half;
    suboffs_[m] = m * half;
  }
  subdims_[kSubspaces - 1] = rest;
  suboffs_[kSubspaces - 1] = half * (kSubspaces - 1);
}

Vector PqQuantizer::subvec(const Vector& v, int m) const {
  Vector s(subdims_[m]);
  for (int i = 0; i < subdims_[m]; ++i) s[i] = v[suboffs_[m] + i];
  return s;
}

void PqQuantizer::fit(const std::vector<Vector>& data, int iters,
                      unsigned seed) {
  if (data.empty()) throw std::invalid_argument("PqQuantizer::fit: empty");
  const int dim = static_cast<int>(data[0].size());
  if (dim < kSubspaces)
    throw std::invalid_argument("PqQuantizer::fit: dim < subspaces");
  for (const auto& v : data)
    if (static_cast<int>(v.size()) != dim)
      throw std::invalid_argument("PqQuantizer::fit: dim mismatch");
  dim_ = dim;
  setup_subdims();

  // Sample-Subset fuer Speed (deterministisch).
  std::vector<const Vector*> pool;
  pool.reserve(data.size());
  for (const auto& v : data) pool.push_back(&v);
  std::mt19937 rng(seed);
  std::shuffle(pool.begin(), pool.end(), rng);
  const size_t nuse = std::min<size_t>(pool.size(), 2048);
  std::vector<const Vector*> use(pool.begin(), pool.begin() + nuse);

  for (int m = 0; m < kSubspaces; ++m) {
    const int sd = subdims_[m];
    codebooks_[m].assign(kCentroids, Vector(sd, 0.0f));
    // Init: erste kCentroids Samples (zyklisch falls zu wenig).
    for (int c = 0; c < kCentroids; ++c)
      codebooks_[m][c] = subvec(*use[c % nuse], m);
    // Lloyd.
    std::vector<int> assign(nuse, 0);
    std::vector<Vector> acc(kCentroids, Vector(sd, 0.0f));
    std::vector<int> cnt(kCentroids, 0);
    for (int it = 0; it < iters; ++it) {
      for (auto& a : acc) std::fill(a.begin(), a.end(), 0.0f);
      std::fill(cnt.begin(), cnt.end(), 0);
      for (size_t i = 0; i < nuse; ++i) {
        Vector s = subvec(*use[i], m);
        int best = 0;
        float bestd = std::numeric_limits<float>::infinity();
        for (int c = 0; c < kCentroids; ++c) {
          float d = sub_l2_squared(s.data(), codebooks_[m][c].data(), sd);
          if (d < bestd) {
            bestd = d;
            best = c;
          }
        }
        assign[i] = best;
        for (int d = 0; d < sd; ++d) acc[best][d] += s[d];
        cnt[best]++;
      }
      for (int c = 0; c < kCentroids; ++c) {
        if (cnt[c] == 0) continue;  // leeres Cluster behalten
        for (int d = 0; d < sd; ++d)
          codebooks_[m][c][d] = acc[c][d] / cnt[c];
      }
    }
  }
  fitted_ = true;
}

PqQuantizer::Code PqQuantizer::encode(const Vector& v) const {
  if (!fitted_) throw std::logic_error("PqQuantizer::encode: not fitted");
  if (static_cast<int>(v.size()) != dim_)
    throw std::invalid_argument("PqQuantizer::encode: dim mismatch");
  Code code{};
  for (int m = 0; m < kSubspaces; ++m) {
    Vector s = subvec(v, m);
    int best = 0;
    float bestd = std::numeric_limits<float>::infinity();
    for (int c = 0; c < kCentroids; ++c) {
      float d =
          sub_l2_squared(s.data(), codebooks_[m][c].data(), subdims_[m]);
      if (d < bestd) {
        bestd = d;
        best = c;
      }
    }
    code[m] = static_cast<uint8_t>(best);
  }
  return code;
}

Vector PqQuantizer::decode(const Code& code) const {
  if (!fitted_) throw std::logic_error("PqQuantizer::decode: not fitted");
  Vector v(dim_, 0.0f);
  for (int m = 0; m < kSubspaces; ++m) {
    const Vector& c = codebooks_[m][code[m]];
    for (int i = 0; i < subdims_[m]; ++i) v[suboffs_[m] + i] = c[i];
  }
  return v;
}

float PqQuantizer::adc_l2_squared(const Vector& query,
                                  const Code& code) const {
  Vector dec = decode(code);
  if (static_cast<int>(query.size()) != dim_)
    throw std::invalid_argument("PqQuantizer::adc: dim mismatch");
  return sub_l2_squared(query.data(), dec.data(), dim_);
}

// --- PqNQuantizer (M konfigurierbar, echte ADC-Tabellen) ---------------------
void PqNQuantizer::init(int dim, int num_subspaces) {
  if (dim <= 0) throw std::invalid_argument("PqNQuantizer::init: dim<=0");
  if (num_subspaces <= 0 || num_subspaces > 32)
    throw std::invalid_argument("PqNQuantizer::init: M in 1..32");
  if (dim < num_subspaces)
    throw std::invalid_argument("PqNQuantizer::init: dim < M");
  dim_ = dim;
  m_ = num_subspaces;
  setup_subdims();
  codebooks_.assign(m_, {});
  fitted_ = false;
}

void PqNQuantizer::setup_subdims() {
  subdims_.assign(m_, 0);
  suboffs_.assign(m_, 0);
  const int base = dim_ / m_;
  const int rem = dim_ % m_;
  int off = 0;
  for (int i = 0; i < m_; ++i) {
    subdims_[i] = base + (i < rem ? 1 : 0);
    suboffs_[i] = off;
    off += subdims_[i];
  }
}

Vector PqNQuantizer::subvec(const Vector& v, int m) const {
  Vector s(subdims_[m]);
  for (int i = 0; i < subdims_[m]; ++i) s[i] = v[suboffs_[m] + i];
  return s;
}

void PqNQuantizer::fit(const std::vector<Vector>& data, int iters,
                       unsigned seed) {
  if (m_ <= 0) throw std::logic_error("PqNQuantizer::fit: init missing");
  if (data.empty()) throw std::invalid_argument("PqNQuantizer::fit: empty");
  const int dim = static_cast<int>(data[0].size());
  if (dim != dim_)
    throw std::invalid_argument("PqNQuantizer::fit: dim mismatch");
  for (const auto& v : data)
    if (static_cast<int>(v.size()) != dim_)
      throw std::invalid_argument("PqNQuantizer::fit: dim mismatch");

  std::vector<const Vector*> pool;
  pool.reserve(data.size());
  for (const auto& v : data) pool.push_back(&v);
  std::mt19937 rng(seed);
  std::shuffle(pool.begin(), pool.end(), rng);
  // Prozent-Regel: min(N, max(2048, N/10)) statt fix 2048.
  const size_t nuse = sample_budget(pool.size());
  std::vector<const Vector*> use(pool.begin(), pool.begin() + nuse);

  codebooks_.assign(m_, {});
  for (int m = 0; m < m_; ++m) {
    const int sd = subdims_[m];
    codebooks_[m].assign(kCentroids, Vector(sd, 0.0f));
    for (int c = 0; c < kCentroids; ++c)
      codebooks_[m][c] = subvec(*use[c % nuse], m);
    std::vector<Vector> acc(kCentroids, Vector(sd, 0.0f));
    std::vector<int> cnt(kCentroids, 0);
    for (int it = 0; it < iters; ++it) {
      for (auto& a : acc) std::fill(a.begin(), a.end(), 0.0f);
      std::fill(cnt.begin(), cnt.end(), 0);
      for (size_t i = 0; i < nuse; ++i) {
        Vector s = subvec(*use[i], m);
        int best = 0;
        float bestd = std::numeric_limits<float>::infinity();
        for (int c = 0; c < kCentroids; ++c) {
          float d = sub_l2_squared(s.data(), codebooks_[m][c].data(), sd);
          if (d < bestd) {
            bestd = d;
            best = c;
          }
        }
        for (int d = 0; d < sd; ++d) acc[best][d] += s[d];
        cnt[best]++;
      }
      for (int c = 0; c < kCentroids; ++c) {
        if (cnt[c] == 0) continue;
        for (int d = 0; d < sd; ++d)
          codebooks_[m][c][d] = acc[c][d] / cnt[c];
      }
    }
  }
  fitted_ = true;
}

std::vector<uint8_t> PqNQuantizer::encode(const Vector& v) const {
  if (!fitted_) throw std::logic_error("PqNQuantizer::encode: not fitted");
  if (static_cast<int>(v.size()) != dim_)
    throw std::invalid_argument("PqNQuantizer::encode: dim mismatch");
  std::vector<uint8_t> code(m_);
  for (int m = 0; m < m_; ++m) {
    Vector s = subvec(v, m);
    int best = 0;
    float bestd = std::numeric_limits<float>::infinity();
    for (int c = 0; c < kCentroids; ++c) {
      float d =
          sub_l2_squared(s.data(), codebooks_[m][c].data(), subdims_[m]);
      if (d < bestd) {
        bestd = d;
        best = c;
      }
    }
    code[m] = static_cast<uint8_t>(best);
  }
  return code;
}

Vector PqNQuantizer::decode(const std::vector<uint8_t>& code) const {
  if (!fitted_) throw std::logic_error("PqNQuantizer::decode: not fitted");
  if (static_cast<int>(code.size()) != m_)
    throw std::invalid_argument("PqNQuantizer::decode: code size mismatch");
  Vector v(dim_, 0.0f);
  for (int m = 0; m < m_; ++m) {
    const Vector& c = codebooks_[m][code[m]];
    for (int i = 0; i < subdims_[m]; ++i) v[suboffs_[m] + i] = c[i];
  }
  return v;
}

PqNQuantizer::AdcTable PqNQuantizer::build_adc_table(
    const Vector& query) const {
  if (!fitted_) throw std::logic_error("PqNQuantizer::adc_table: not fitted");
  if (static_cast<int>(query.size()) != dim_)
    throw std::invalid_argument("PqNQuantizer::adc_table: dim mismatch");
  AdcTable t(m_, std::vector<float>(kCentroids, 0.0f));
  for (int m = 0; m < m_; ++m) {
    Vector qs = subvec(query, m);
    for (int c = 0; c < kCentroids; ++c)
      t[m][c] =
          sub_l2_squared(qs.data(), codebooks_[m][c].data(), subdims_[m]);
  }
  return t;
}

float PqNQuantizer::adc_with_table(
    const AdcTable& t, const std::vector<uint8_t>& code) const {
  if (static_cast<int>(code.size()) != m_ ||
      static_cast<int>(t.size()) != m_)
    throw std::invalid_argument("PqNQuantizer::adc_with_table: size mismatch");
  double acc = 0.0;
  for (int m = 0; m < m_; ++m) acc += t[m][code[m]];
  return static_cast<float>(acc);
}

float PqNQuantizer::adc_l2_squared(
    const Vector& query, const std::vector<uint8_t>& code) const {
  AdcTable t = build_adc_table(query);
  return adc_with_table(t, code);
}

const std::vector<std::vector<Vector>>& PqNQuantizer::codebooks() const {
  return codebooks_;
}

void PqNQuantizer::set_codebooks(
    const std::vector<std::vector<Vector>>& cb) {
  if (m_ <= 0) throw std::logic_error("PqNQuantizer::set_codebooks: no init");
  if (static_cast<int>(cb.size()) != m_)
    throw std::invalid_argument("PqNQuantizer::set_codebooks: M mismatch");
  for (int m = 0; m < m_; ++m) {
    if (static_cast<int>(cb[m].size()) != kCentroids)
      throw std::invalid_argument(
          "PqNQuantizer::set_codebooks: centroid count mismatch");
    for (int c = 0; c < kCentroids; ++c)
      if (static_cast<int>(cb[m][c].size()) != subdims_[m])
        throw std::invalid_argument(
            "PqNQuantizer::set_codebooks: subdim mismatch");
  }
  codebooks_ = cb;
  fitted_ = true;
}

// --- IvfPqIndex ---------------------------------------------------------------
int IvfPqIndex::default_nlist(size_t n) {
  if (n == 0) return 1;
  const double raw = 4.0 * std::sqrt(static_cast<double>(n));
  long nl = static_cast<long>(std::lround(raw));
  if (nl < 1) nl = 1;
  if (nl > 4096) nl = 4096;
  if (static_cast<size_t>(nl) > n) nl = static_cast<long>(n);
  if (nl < 1) nl = 1;
  return static_cast<int>(nl);
}

int IvfPqIndex::default_nprobe(int nlist) {
  if (nlist <= 1) return 1;
  // ~nlist/8, aufgerundet, mind. 1, max. nlist.
  int np = (nlist + 7) / 8;
  if (np < 1) np = 1;
  if (np > nlist) np = nlist;
  return np;
}

IvfPqIndex::IvfDefaults IvfPqIndex::autotune(size_t n) {
  IvfDefaults d;
  d.nlist = default_nlist(n);
  d.nprobe = default_nprobe(d.nlist);
  return d;
}

void IvfPqIndex::train(const std::vector<Vector>& data, int coarse_iters,
                       int pq_iters, unsigned seed) {
  if (dim_ <= 0 || nlist_ <= 0)
    throw std::logic_error("IvfPqIndex::train: dim/nlist missing");
  if (data.empty()) throw std::invalid_argument("IvfPqIndex::train: empty");
  for (const auto& v : data)
    if (static_cast<int>(v.size()) != dim_)
      throw std::invalid_argument("IvfPqIndex::train: dim mismatch");
  if (static_cast<int>(data.size()) < nlist_)
    throw std::invalid_argument("IvfPqIndex::train: N < nlist");

  // Coarse k-means (deterministisch): init via geshuffelte Samples.
  std::vector<const Vector*> pool;
  for (const auto& v : data) pool.push_back(&v);
  std::mt19937 rng(seed);
  std::shuffle(pool.begin(), pool.end(), rng);
  coarse_.assign(nlist_, Vector(dim_, 0.0f));
  for (int c = 0; c < nlist_; ++c) coarse_[c] = *pool[c % pool.size()];

  // Prozent-Regel: min(N, max(2048, N/10)) statt fix 4096.
  const size_t nuse = sample_budget(data.size());
  std::vector<const Vector*> use(pool.begin(), pool.begin() + nuse);
  std::vector<int> assign(nuse, 0);
  std::vector<Vector> acc(nlist_, Vector(dim_, 0.0f));
  std::vector<int> cnt(nlist_, 0);
  for (int it = 0; it < coarse_iters; ++it) {
    for (auto& a : acc) std::fill(a.begin(), a.end(), 0.0f);
    std::fill(cnt.begin(), cnt.end(), 0);
    for (size_t i = 0; i < nuse; ++i) {
      int best = 0;
      float bestd = std::numeric_limits<float>::infinity();
      for (int c = 0; c < nlist_; ++c) {
        float d = sub_l2_squared(use[i]->data(), coarse_[c].data(), dim_);
        if (d < bestd) {
          bestd = d;
          best = c;
        }
      }
      assign[i] = best;
      for (int d = 0; d < dim_; ++d) acc[best][d] += (*use[i])[d];
      cnt[best]++;
    }
    for (int c = 0; c < nlist_; ++c) {
      if (cnt[c] == 0) continue;
      for (int d = 0; d < dim_; ++d) coarse_[c][d] = acc[c][d] / cnt[c];
    }
  }
  // Residuales PQ-Training (IVFADC): PQ auf (Daten - zugeordnetes
  // Coarse-Zentroid) fitten. Zuordnung ueber volles N (exaktes Nearest),
  // PQ-Subsampling passiert deterministisch in PqNQuantizer::fit (seed+1).
  std::vector<Vector> residuals;
  residuals.reserve(data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    int best = 0;
    float bestd = std::numeric_limits<float>::infinity();
    for (int c = 0; c < nlist_; ++c) {
      float d = sub_l2_squared(data[i].data(), coarse_[c].data(), dim_);
      if (d < bestd) {
        bestd = d;
        best = c;
      }
    }
    Vector r(dim_);
    for (int d = 0; d < dim_; ++d) r[d] = data[i][d] - coarse_[best][d];
    residuals.push_back(std::move(r));
  }
  pq_.fit(residuals, pq_iters, seed + 1);
  trained_ = true;
  built_ = false;
}

void IvfPqIndex::build(const std::vector<Vector>& data) {
  if (!trained_) throw std::logic_error("IvfPqIndex::build: train missing");
  invlists_.assign(nlist_, {});
  Vector resid(dim_);
  for (size_t i = 0; i < data.size(); ++i) {
    int best = 0;
    float bestd = std::numeric_limits<float>::infinity();
    for (int c = 0; c < nlist_; ++c) {
      float d = sub_l2_squared(data[i].data(), coarse_[c].data(), dim_);
      if (d < bestd) {
        bestd = d;
        best = c;
      }
    }
    // Residual-Code speichern (Format-Wechsel: inkompatibel zu
    // nicht-residualen Codes; in-memory, kein Persistenz-Format betroffen).
    for (int d = 0; d < dim_; ++d) resid[d] = data[i][d] - coarse_[best][d];
    Posting p;
    p.id = static_cast<int>(i);
    p.code = pq_.encode(resid);
    invlists_[best].push_back(std::move(p));
  }
  ntotal_ = data.size();
  built_ = true;
}

std::vector<int> IvfPqIndex::pick_probes(const Vector& query,
                                        int nprobe) const {
  std::vector<std::pair<float, int>> scored;
  scored.reserve(nlist_);
  for (int c = 0; c < nlist_; ++c)
    scored.emplace_back(
        sub_l2_squared(query.data(), coarse_[c].data(), dim_), c);
  if (nprobe > nlist_) nprobe = nlist_;
  std::nth_element(scored.begin(), scored.begin() + nprobe, scored.end(),
                   [](const auto& a, const auto& b) {
                     return a.first < b.first;
                   });
  std::sort(scored.begin(), scored.begin() + nprobe,
            [](const auto& a, const auto& b) {
              if (a.first != b.first) return a.first < b.first;
              return a.second < b.second;
            });
  std::vector<int> out;
  out.reserve(nprobe);
  for (int i = 0; i < nprobe; ++i) out.push_back(scored[i].second);
  return out;
}

std::vector<SearchHit> IvfPqIndex::search(
    const Vector& query, const std::vector<Vector>& base_data, int k,
    int nprobe, int ef_rerank) const {
  if (!trained_ || !built_)
    throw std::logic_error("IvfPqIndex::search: train+build missing");
  if (static_cast<int>(query.size()) != dim_)
    throw std::invalid_argument("IvfPqIndex::search: dim mismatch");
  if (k <= 0) return {};
  if (nprobe <= 0) nprobe = 1;
  if (nprobe > nlist_) nprobe = nlist_;
  auto probes = pick_probes(query, nprobe);
  // Residual-ADC: pro Sonde Tabelle aus (Query - Sonden-Zentroid), da Codes
  // Residuen sind: ||q - (c + r)||^2 = ||(q - c) - r||^2.
  std::vector<std::pair<float, int>> scored;
  size_t total = 0;
  for (int c : probes) total += invlists_[c].size();
  scored.reserve(total);
  Vector rq(dim_);
  for (int c : probes) {
    for (int d = 0; d < dim_; ++d) rq[d] = query[d] - coarse_[c][d];
    auto table = pq_.build_adc_table(rq);
    for (const auto& p : invlists_[c])
      scored.emplace_back(pq_.adc_with_table(table, p.code), p.id);
  }
  int ef = std::min<int>(static_cast<int>(scored.size()), ef_rerank);
  if (ef < k) ef = std::min<int>(static_cast<int>(scored.size()), k);
  if (ef <= 0) return {};
  std::nth_element(scored.begin(), scored.begin() + ef, scored.end(),
                   [](const auto& a, const auto& b) {
                     return a.first < b.first;
                   });
  scored.resize(ef);
  std::vector<SearchHit> cand;
  cand.reserve(ef);
  for (auto& [approx_d, id] : scored)
    cand.push_back({id, l2_distance(query, base_data[id])});
  if (static_cast<int>(cand.size()) > k) {
    std::nth_element(cand.begin(), cand.begin() + k, cand.end(),
                     [](const SearchHit& a, const SearchHit& b) {
                       if (a.dist != b.dist) return a.dist < b.dist;
                       return a.id < b.id;
                     });
    cand.resize(k);
  }
  std::sort(cand.begin(), cand.end(),
            [](const SearchHit& a, const SearchHit& b) {
              if (a.dist != b.dist) return a.dist < b.dist;
              return a.id < b.id;
            });
  return cand;
}

// --- DiskSpill --------------------------------------------------------------
DiskSpill::DiskSpill(DiskSpill&& other) noexcept
    : base_(other.base_),
      mapped_len_(other.mapped_len_),
      fd_(other.fd_),
      dim_(other.dim_),
      count_(other.count_),
      payload_(other.payload_) {
  other.base_ = nullptr;
  other.mapped_len_ = 0;
  other.fd_ = -1;
  other.dim_ = 0;
  other.count_ = 0;
  other.payload_ = nullptr;
}

DiskSpill& DiskSpill::operator=(DiskSpill&& other) noexcept {
  if (this != &other) {
    close();
    base_ = other.base_;
    mapped_len_ = other.mapped_len_;
    fd_ = other.fd_;
    dim_ = other.dim_;
    count_ = other.count_;
    payload_ = other.payload_;
    other.base_ = nullptr;
    other.mapped_len_ = 0;
    other.fd_ = -1;
    other.dim_ = 0;
    other.count_ = 0;
    other.payload_ = nullptr;
  }
  return *this;
}

void DiskSpill::create(const std::string& path, int dim, size_t count) {
  close();
  if (dim <= 0) throw std::invalid_argument("DiskSpill::create: dim<=0");
  const size_t len = sizeof(FileHeader) + count * dim * sizeof(float);
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    throw std::runtime_error(std::string("DiskSpill::create open: ") +
                             std::strerror(errno));
  if (::ftruncate(fd, static_cast<off_t>(len)) != 0) {
    ::close(fd);
    throw std::runtime_error("DiskSpill::create ftruncate failed");
  }
  void* base = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    ::close(fd);
    throw std::runtime_error("DiskSpill::create mmap failed");
  }
  auto* h = static_cast<FileHeader*>(base);
  std::memcpy(h->magic, kMagic, 8);
  h->dim = static_cast<uint64_t>(dim);
  h->count = static_cast<uint64_t>(count);
  base_ = base;
  mapped_len_ = len;
  fd_ = fd;
  dim_ = dim;
  count_ = count;
  payload_ = reinterpret_cast<float*>(static_cast<char*>(base) +
                                     sizeof(FileHeader));
}

void DiskSpill::open(const std::string& path) {
  close();
  int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0)
    throw std::runtime_error(std::string("DiskSpill::open: ") +
                             std::strerror(errno));
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw std::runtime_error("DiskSpill::open fstat failed");
  }
  if (st.st_size < static_cast<off_t>(sizeof(FileHeader))) {
    ::close(fd);
    throw std::runtime_error("DiskSpill::open: file too small");
  }
  void* base = ::mmap(nullptr, static_cast<size_t>(st.st_size),
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    ::close(fd);
    throw std::runtime_error("DiskSpill::open mmap failed");
  }
  auto* h = static_cast<FileHeader*>(base);
  if (std::memcmp(h->magic, kMagic, 8) != 0) {
    ::munmap(base, static_cast<size_t>(st.st_size));
    ::close(fd);
    throw std::runtime_error("DiskSpill::open: bad magic");
  }
  const size_t expect =
      sizeof(FileHeader) + h->count * h->dim * sizeof(float);
  if (expect != static_cast<size_t>(st.st_size)) {
    ::munmap(base, static_cast<size_t>(st.st_size));
    ::close(fd);
    throw std::runtime_error("DiskSpill::open: size mismatch");
  }
  base_ = base;
  mapped_len_ = static_cast<size_t>(st.st_size);
  fd_ = fd;
  dim_ = static_cast<int>(h->dim);
  count_ = static_cast<size_t>(h->count);
  payload_ = reinterpret_cast<float*>(static_cast<char*>(base) +
                                     sizeof(FileHeader));
}

void DiskSpill::close() {
  if (base_ != nullptr) {
    ::munmap(base_, mapped_len_);
    base_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  mapped_len_ = 0;
  dim_ = 0;
  count_ = 0;
  payload_ = nullptr;
}

void DiskSpill::sync() const {
  if (base_ != nullptr) ::msync(base_, mapped_len_, MS_SYNC);
}

void DiskSpill::set(size_t id, const Vector& v) {
  if (base_ == nullptr) throw std::logic_error("DiskSpill::set: closed");
  if (id >= count_) throw std::out_of_range("DiskSpill::set: bad id");
  if (static_cast<int>(v.size()) != dim_)
    throw std::invalid_argument("DiskSpill::set: dim mismatch");
  std::memcpy(payload_ + id * dim_, v.data(), dim_ * sizeof(float));
}

Vector DiskSpill::get(size_t id) const {
  if (base_ == nullptr) throw std::logic_error("DiskSpill::get: closed");
  if (id >= count_) throw std::out_of_range("DiskSpill::get: bad id");
  Vector v(dim_);
  std::memcpy(v.data(), payload_ + id * dim_, dim_ * sizeof(float));
  return v;
}

// --- ef-Autotune ------------------------------------------------------------
int autotune_ef(int k, double selectivity, int n, int ef_min, int ef_max) {
  if (k <= 0) throw std::invalid_argument("autotune_ef: k<=0");
  if (ef_min <= 0) ef_min = 1;
  if (ef_max < ef_min) ef_max = ef_min;
  double sel = selectivity;
  if (!(sel > 0.0)) sel = 1e-4;
  if (sel > 1.0) sel = 1.0;
  if (sel < 1e-4) sel = 1e-4;
  constexpr double kOver = 2.0;
  long ef = static_cast<long>(std::ceil(static_cast<double>(k) / sel * kOver));
  if (ef < k) ef = k;
  if (ef < ef_min) ef = ef_min;
  if (ef > ef_max) ef = ef_max;
  if (n >= 0 && ef > n) ef = n;
  if (ef < 1) ef = 1;
  return static_cast<int>(ef);
}

int autotune_ef_for_filter(int k, size_t matched, size_t total, int n,
                           int ef_min, int ef_max) {
  double sel = 1.0;
  if (total > 0) sel = static_cast<double>(matched) / total;
  if (matched == 0) sel = 1e-4;
  const int nn = (n >= 0) ? n : static_cast<int>(total);
  return autotune_ef(k, sel, nn < 0 ? -1 : nn, ef_min, ef_max);
}

// --- ADC-Coarse + Re-Rank ---------------------------------------------------
std::vector<SearchHit> quantized_search_rerank(
    const Vector& query, const std::vector<Vector>& data,
    const std::vector<std::vector<uint8_t>>& codes, const Sq8Quantizer& q,
    int k, int ef_rerank) {
  if (k <= 0) return {};
  if (data.size() != codes.size())
    throw std::invalid_argument("quantized_search_rerank: size mismatch");
  const size_t n = data.size();
  int ef = static_cast<int>(std::min<size_t>(n, ef_rerank));
  if (ef < k) ef = std::min<size_t>(n, k);
  // Coarse: ADC L2-squared ueber alle Codes.
  std::vector<std::pair<float, int>> scored;
  scored.reserve(n);
  for (size_t i = 0; i < n; ++i)
    scored.emplace_back(q.adc_l2_squared(query, codes[i]),
                        static_cast<int>(i));
  std::nth_element(scored.begin(), scored.begin() + ef, scored.end(),
                   [](const auto& a, const auto& b) {
                     return a.first < b.first;
                   });
  scored.resize(ef);
  // Re-Rank exakt.
  std::vector<SearchHit> cand;
  cand.reserve(ef);
  for (auto& [approx_d, id] : scored)
    cand.push_back({id, l2_distance(query, data[id])});
  if (static_cast<int>(cand.size()) > k) {
    std::nth_element(cand.begin(), cand.begin() + k, cand.end(),
                     [](const SearchHit& a, const SearchHit& b) {
                       if (a.dist != b.dist) return a.dist < b.dist;
                       return a.id < b.id;
                     });
    cand.resize(k);
  }
  std::sort(cand.begin(), cand.end(),
            [](const SearchHit& a, const SearchHit& b) {
              if (a.dist != b.dist) return a.dist < b.dist;
              return a.id < b.id;
            });
  return cand;
}

// --- DiskAnnIndex (DiskANN-Lite) -------------------------------------------
namespace {
constexpr char kAnnMagic[8] = {'D', 'B', 'E', 'A', 'N', 'N', '1', '\0'};
struct AnnHeader {
  char magic[8];
  uint64_t dim;
  uint64_t mpq;
  uint64_t mgraph;
  uint64_t count;
};
inline std::string ann_sibling(const std::string& prefix, const char* ext) {
  return prefix + ext;
}
}  // namespace

void DiskAnnIndex::init(int dim, int num_subspaces, int m_graph) {
  if (dim <= 0) throw std::invalid_argument("DiskAnnIndex::init: dim<=0");
  if (num_subspaces <= 0 || num_subspaces > 32)
    throw std::invalid_argument("DiskAnnIndex::init: M in 1..32");
  if (dim < num_subspaces)
    throw std::invalid_argument("DiskAnnIndex::init: dim < M");
  if (m_graph <= 0 || m_graph > 512)
    throw std::invalid_argument("DiskAnnIndex::init: m_graph in 1..512");
  close();
  dim_ = dim;
  m_pq_ = num_subspaces;
  m_graph_ = m_graph;
  count_ = 0;
  entry_ = 0;
  prefix_.clear();
  pq_.init(dim, num_subspaces);
}

void DiskAnnIndex::build(const std::vector<Vector>& data,
                         const std::string& prefix, int pq_iters,
                         unsigned seed) {
  if (dim_ <= 0 || m_pq_ <= 0 || m_graph_ <= 0)
    throw std::logic_error("DiskAnnIndex::build: init missing");
  if (data.empty()) throw std::invalid_argument("DiskAnnIndex::build: empty");
  if (prefix.empty())
    throw std::invalid_argument("DiskAnnIndex::build: empty prefix");
  for (const auto& v : data)
    if (static_cast<int>(v.size()) != dim_)
      throw std::invalid_argument("DiskAnnIndex::build: dim mismatch");
  if (pq_iters <= 0) throw std::invalid_argument("DiskAnnIndex: pq_iters<=0");

  pq_.fit(data, pq_iters, seed);
  const size_t n = data.size();

  // Codes in RAM vorbereiten.
  std::vector<std::vector<uint8_t>> codes;
  codes.reserve(n);
  for (const auto& v : data) codes.push_back(pq_.encode(v));

  // Graph-Hybrid (deterministisch, (dist,id)-Tie-Break):
  //   N <= DiskAnnIndex::kExactThreshold: exakte M-NN wie bisher (O(N^2)).
  //   N darueber: HnswIndex-ANN (HNSW ueber denselben Daten, Seed+2, dann
  //   Top-(M+1)-Query pro Knoten mit ef-Tuning, Self filtern, Top-M als
  //   Adjazenz; exaktes Auffuellen bei Luecken). Build ~O(N log N).
  std::vector<std::vector<int>> adj(n);
  if (n <= DiskAnnIndex::kExactThreshold) {
    std::vector<std::pair<float, int>> dists;
    for (size_t i = 0; i < n; ++i) {
      dists.clear();
      dists.reserve(n > 0 ? n - 1 : 0);
      for (size_t j = 0; j < n; ++j) {
        if (j == i) continue;
        const float d2 = sub_l2_squared(data[i].data(), data[j].data(), dim_);
        dists.emplace_back(d2, static_cast<int>(j));
      }
      const int want = std::min<int>(m_graph_, static_cast<int>(dists.size()));
      if (want > 0) {
        if (want < static_cast<int>(dists.size())) {
          std::nth_element(dists.begin(), dists.begin() + want, dists.end(),
                           [](const auto& a, const auto& b) {
                             if (a.first != b.first) return a.first < b.first;
                             return a.second < b.second;
                           });
          dists.resize(static_cast<size_t>(want));
        }
        std::sort(dists.begin(), dists.end(),
                  [](const auto& a, const auto& b) {
                    if (a.first != b.first) return a.first < b.first;
                    return a.second < b.second;
                  });
        adj[i].reserve(static_cast<size_t>(m_graph_));
        for (const auto& pr : dists) adj[i].push_back(pr.second);
      }
    }
  } else {
    dbengine::vector::HnswIndex hnsw(
        dim_, 16, 32, dbengine::vector::DistanceMetric::L2);
    hnsw.set_rng_seed(seed + 2);
    int efc = 2 * m_graph_;
    if (efc < 64) efc = 64;
    if (efc > 512) efc = 512;
    hnsw.set_ef_construction(efc);
    for (const auto& v : data) hnsw.add(v);
    hnsw.build();
    const int want0 =
        std::min<int>(m_graph_, static_cast<int>(n > 0 ? n - 1 : 0));
    const int kq = want0 + 1;
    const long autoe = static_cast<long>(
        dbengine::vector::HnswIndex::auto_ef(n, kq, 32));
    long ef_ann = 8L * static_cast<long>(kq);
    if (ef_ann < 256) ef_ann = 256;
    if (ef_ann < autoe) ef_ann = autoe;
    if (ef_ann < kq) ef_ann = kq;
    if (ef_ann > static_cast<long>(n)) ef_ann = static_cast<long>(n);
    if (ef_ann > 2048) ef_ann = 2048;
    if (ef_ann < 1) ef_ann = 1;
    const int ef_ann_i = static_cast<int>(ef_ann);
    std::vector<std::pair<float, int>> dists;
    dists.reserve(n > 0 ? n - 1 : 0);
    for (size_t i = 0; i < n; ++i) {
      adj[i].reserve(static_cast<size_t>(m_graph_));
      if (want0 <= 0) continue;
      std::vector<dbengine::vector::SearchHit> hits =
          hnsw.search(data[i], kq, ef_ann_i);
      std::sort(hits.begin(), hits.end(),
                [](const dbengine::vector::SearchHit& a,
                   const dbengine::vector::SearchHit& b) {
                  if (a.dist != b.dist) return a.dist < b.dist;
                  return a.id < b.id;
                });
      for (const auto& h : hits) {
        if (h.id == static_cast<int>(i)) continue;
        if (static_cast<int>(adj[i].size()) >= want0) break;
        if (h.id < 0 || static_cast<size_t>(h.id) >= n) continue;
        adj[i].push_back(h.id);
      }
      // Luecken-Garantie: exakt auffuellen (tritt bei grossem ef praktisch
      // nie auf; deterministisch, (dist,id)-geordnet).
      if (static_cast<int>(adj[i].size()) < want0) {
        std::vector<char> taken(n, 0);
        taken[i] = 1;
        for (int id : adj[i]) taken[static_cast<size_t>(id)] = 1;
        dists.clear();
        for (size_t j = 0; j < n; ++j) {
          if (taken[j] != 0) continue;
          const float d2 =
              sub_l2_squared(data[i].data(), data[j].data(), dim_);
          dists.emplace_back(d2, static_cast<int>(j));
        }
        const int need = want0 - static_cast<int>(adj[i].size());
        const int take = std::min<int>(need, static_cast<int>(dists.size()));
        if (take > 0) {
          if (take < static_cast<int>(dists.size())) {
            std::nth_element(
                dists.begin(), dists.begin() + take, dists.end(),
                [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return a.second < b.second;
                });
            dists.resize(static_cast<size_t>(take));
          }
          std::sort(dists.begin(), dists.end(),
                    [](const auto& a, const auto& b) {
                      if (a.first != b.first) return a.first < b.first;
                      return a.second < b.second;
                    });
          for (const auto& pr : dists) adj[i].push_back(pr.second);
        }
      }
    }
  }

  // Persistenz: 3 DiskSpills (fixe Records dim_/m_pq_/m_graph_).
  close();
  const std::string vec_path = ann_sibling(prefix, ".vec");
  const std::string pq_path = ann_sibling(prefix, ".pq");
  const std::string graph_path = ann_sibling(prefix, ".graph");
  const std::string hdr_path = ann_sibling(prefix, ".hdr");
  vec_.create(vec_path, dim_, n);
  for (size_t i = 0; i < n; ++i) vec_.set(i, data[i]);
  codes_.create(pq_path, m_pq_, n);
  Vector coderow(static_cast<size_t>(m_pq_));
  for (size_t i = 0; i < n; ++i) {
    for (int m = 0; m < m_pq_; ++m)
      coderow[static_cast<size_t>(m)] =
          static_cast<float>(codes[i][static_cast<size_t>(m)]);
    codes_.set(i, coderow);
  }
  graph_.create(graph_path, m_graph_, n);
  Vector grow(static_cast<size_t>(m_graph_), -1.0f);
  for (size_t i = 0; i < n; ++i) {
    std::fill(grow.begin(), grow.end(), -1.0f);
    for (size_t kk = 0; kk < adj[i].size(); ++kk)
      grow[kk] = static_cast<float>(adj[i][kk]);
    graph_.set(i, grow);
  }
  vec_.sync();
  codes_.sync();
  graph_.sync();

  // Header + Codebooks (float32).
  std::ofstream out(hdr_path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("DiskAnnIndex::build: hdr write open");
  AnnHeader hh{};
  std::memcpy(hh.magic, kAnnMagic, 8);
  hh.dim = static_cast<uint64_t>(dim_);
  hh.mpq = static_cast<uint64_t>(m_pq_);
  hh.mgraph = static_cast<uint64_t>(m_graph_);
  hh.count = static_cast<uint64_t>(n);
  out.write(reinterpret_cast<const char*>(&hh), sizeof(hh));
  const auto& cb = pq_.codebooks();
  for (int m = 0; m < m_pq_; ++m)
    for (int c = 0; c < PqNQuantizer::kCentroids; ++c) {
      const Vector& cent = cb[static_cast<size_t>(m)][static_cast<size_t>(c)];
      out.write(reinterpret_cast<const char*>(cent.data()),
                static_cast<std::streamsize>(cent.size() * sizeof(float)));
    }
  out.flush();
  if (!out) throw std::runtime_error("DiskAnnIndex::build: hdr write failed");
  out.close();

  count_ = n;
  entry_ = 0;
  prefix_ = prefix;
  open_ = true;
}

void DiskAnnIndex::open(const std::string& prefix) {
  if (prefix.empty())
    throw std::invalid_argument("DiskAnnIndex::open: empty prefix");
  close();
  const std::string hdr_path = ann_sibling(prefix, ".hdr");
  std::ifstream in(hdr_path, std::ios::binary);
  if (!in) throw std::runtime_error("DiskAnnIndex::open: hdr missing");
  AnnHeader hh{};
  in.read(reinterpret_cast<char*>(&hh), sizeof(hh));
  if (!in || std::memcmp(hh.magic, kAnnMagic, 8) != 0)
    throw std::runtime_error("DiskAnnIndex::open: bad header");
  const int dim = static_cast<int>(hh.dim);
  const int mpq = static_cast<int>(hh.mpq);
  const int mgraph = static_cast<int>(hh.mgraph);
  const size_t count = static_cast<size_t>(hh.count);
  if (dim <= 0 || mpq <= 0 || mpq > 32 || dim < mpq || mgraph <= 0 ||
      mgraph > 512 || count == 0)
    throw std::runtime_error("DiskAnnIndex::open: bad dims");
  std::vector<int> subdims(static_cast<size_t>(mpq), 0);
  {
    const int base = dim / mpq;
    const int rem = dim % mpq;
    for (int i = 0; i < mpq; ++i)
      subdims[static_cast<size_t>(i)] = base + (i < rem ? 1 : 0);
  }
  std::vector<std::vector<Vector>> cb(static_cast<size_t>(mpq));
  for (int m = 0; m < mpq; ++m) {
    cb[static_cast<size_t>(m)].assign(
        PqNQuantizer::kCentroids,
        Vector(static_cast<size_t>(subdims[static_cast<size_t>(m)]), 0.0f));
    for (int c = 0; c < PqNQuantizer::kCentroids; ++c) {
      Vector& cent = cb[static_cast<size_t>(m)][static_cast<size_t>(c)];
      in.read(reinterpret_cast<char*>(cent.data()),
              static_cast<std::streamsize>(cent.size() * sizeof(float)));
      if (!in) throw std::runtime_error("DiskAnnIndex::open: codebook short");
    }
  }
  {
    char extra = 0;
    in.read(&extra, 1);
    if (in.gcount() != 0)
      throw std::runtime_error("DiskAnnIndex::open: hdr trailing bytes");
  }
  in.close();

  PqNQuantizer pq;
  pq.init(dim, mpq);
  pq.set_codebooks(cb);

  DiskSpill vec;
  DiskSpill code;
  DiskSpill gr;
  vec.open(ann_sibling(prefix, ".vec"));
  code.open(ann_sibling(prefix, ".pq"));
  gr.open(ann_sibling(prefix, ".graph"));
  if (vec.dim() != dim || vec.size() != count)
    throw std::runtime_error("DiskAnnIndex::open: vec mismatch");
  if (code.dim() != mpq || code.size() != count)
    throw std::runtime_error("DiskAnnIndex::open: pq mismatch");
  if (gr.dim() != mgraph || gr.size() != count)
    throw std::runtime_error("DiskAnnIndex::open: graph mismatch");

  dim_ = dim;
  m_pq_ = mpq;
  m_graph_ = mgraph;
  count_ = count;
  entry_ = 0;
  prefix_ = prefix;
  pq_ = std::move(pq);
  vec_ = std::move(vec);
  codes_ = std::move(code);
  graph_ = std::move(gr);
  open_ = true;
}

void DiskAnnIndex::close() {
  vec_.close();
  codes_.close();
  graph_.close();
  open_ = false;
}

std::vector<int> DiskAnnIndex::read_adj_row(int id) const {
  Vector row = graph_.get(static_cast<size_t>(id));
  std::vector<int> out;
  out.reserve(static_cast<size_t>(m_graph_));
  for (float f : row) {
    if (f < -0.5f) continue;
    const long v = std::lround(f);
    if (v < 0 || v >= static_cast<long>(count_)) continue;
    out.push_back(static_cast<int>(v));
  }
  return out;
}

std::vector<uint8_t> DiskAnnIndex::read_code_row(int id) const {
  Vector row = codes_.get(static_cast<size_t>(id));
  std::vector<uint8_t> code(static_cast<size_t>(m_pq_));
  for (int m = 0; m < m_pq_; ++m) {
    long v = std::lround(row[static_cast<size_t>(m)]);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    code[static_cast<size_t>(m)] = static_cast<uint8_t>(v);
  }
  return code;
}

float DiskAnnIndex::adc_of(int id,
                            const PqNQuantizer::AdcTable& table) const {
  std::vector<uint8_t> code = read_code_row(id);
  return pq_.adc_with_table(table, code);
}

std::vector<int> DiskAnnIndex::neighbors(int id) const {
  if (!open_) throw std::logic_error("DiskAnnIndex::neighbors: closed");
  if (id < 0 || id >= static_cast<int>(count_))
    throw std::out_of_range("DiskAnnIndex::neighbors: bad id");
  return read_adj_row(id);
}

Vector DiskAnnIndex::get_vector(int id) const {
  if (!open_) throw std::logic_error("DiskAnnIndex::get_vector: closed");
  if (id < 0 || id >= static_cast<int>(count_))
    throw std::out_of_range("DiskAnnIndex::get_vector: bad id");
  return vec_.get(static_cast<size_t>(id));
}

std::vector<uint8_t> DiskAnnIndex::get_code(int id) const {
  if (!open_) throw std::logic_error("DiskAnnIndex::get_code: closed");
  if (id < 0 || id >= static_cast<int>(count_))
    throw std::out_of_range("DiskAnnIndex::get_code: bad id");
  return read_code_row(id);
}

std::vector<SearchHit> DiskAnnIndex::search(const Vector& query, int k,
                                            int beam,
                                            int ef_rerank) const {
  if (!open_) throw std::logic_error("DiskAnnIndex::search: closed");
  if (static_cast<int>(query.size()) != dim_)
    throw std::invalid_argument("DiskAnnIndex::search: dim mismatch");
  if (k <= 0) return {};
  const int n = static_cast<int>(count_);
  if (n == 0) return {};
  int ef_beam = beam;
  if (ef_beam < k) ef_beam = k;
  if (ef_beam > n) ef_beam = n;
  if (ef_beam < 1) ef_beam = 1;
  int ef_rr = ef_rerank;
  if (ef_rr < k) ef_rr = k;
  if (ef_rr > n) ef_rr = n;
  if (ef_rr < 1) ef_rr = 1;

  // Eine ADC-Tabelle pro Query (RAM-Codebooks, kein decode).
  PqNQuantizer::AdcTable table = pq_.build_adc_table(query);

  using Cand = std::pair<float, int>;
  auto cmp_min = [](const Cand& a, const Cand& b) {
    if (a.first != b.first) return a.first > b.first;
    return a.second > b.second;
  };
  auto cmp_max = [](const Cand& a, const Cand& b) {
    if (a.first != b.first) return a.first < b.first;
    return a.second < b.second;
  };
  std::vector<char> seen(static_cast<size_t>(n), 0);
  std::vector<Cand> frontier;
  frontier.reserve(static_cast<size_t>(2 * ef_beam + 8));
  std::vector<Cand> top;
  top.reserve(static_cast<size_t>(ef_beam + 1));
  std::vector<Cand> scored;
  scored.reserve(static_cast<size_t>(n));

  const int entry = (entry_ >= 0 && entry_ < n) ? entry_ : 0;
  const float d0 = adc_of(entry, table);
  seen[static_cast<size_t>(entry)] = 1;
  frontier.emplace_back(d0, entry);
  std::push_heap(frontier.begin(), frontier.end(), cmp_min);
  top.emplace_back(d0, entry);
  std::push_heap(top.begin(), top.end(), cmp_max);
  scored.emplace_back(d0, entry);

  std::vector<int> nbrs;
  while (!frontier.empty()) {
    const float d_u = frontier.front().first;
    if (static_cast<int>(top.size()) >= ef_beam && d_u > top.front().first)
      break;
    std::pop_heap(frontier.begin(), frontier.end(), cmp_min);
    const Cand cur = frontier.back();
    frontier.pop_back();
    const int u = cur.second;
    nbrs = read_adj_row(u);  // mmap-Read, fixe Record-Groesse
    for (int v : nbrs) {
      if (v < 0 || v >= n || seen[static_cast<size_t>(v)] != 0) continue;
      seen[static_cast<size_t>(v)] = 1;
      const float d_v = adc_of(v, table);  // kein decode
      scored.emplace_back(d_v, v);
      frontier.emplace_back(d_v, v);
      std::push_heap(frontier.begin(), frontier.end(), cmp_min);
      if (static_cast<int>(top.size()) < ef_beam) {
        top.emplace_back(d_v, v);
        std::push_heap(top.begin(), top.end(), cmp_max);
      } else {
        const Cand& worst = top.front();
        if (d_v < worst.first || (d_v == worst.first && v < worst.second)) {
          std::pop_heap(top.begin(), top.end(), cmp_max);
          top.back() = Cand(d_v, v);
          std::push_heap(top.begin(), top.end(), cmp_max);
        }
      }
    }
  }
  // Refill-Garantie: falls Beam zu wenige Knoten erreichte (disconnect),
  // mit ADC-Linearscan auf ef_rr auffuellen (weiterhin kein decode).
  if (static_cast<int>(scored.size()) < ef_rr) {
    for (int i = 0; i < n && static_cast<int>(scored.size()) < ef_rr; ++i) {
      if (seen[static_cast<size_t>(i)] != 0) continue;
      seen[static_cast<size_t>(i)] = 1;
      scored.emplace_back(adc_of(i, table), i);
    }
  }
  int ef = std::min<int>(static_cast<int>(scored.size()), ef_rr);
  if (ef < k) ef = std::min<int>(static_cast<int>(scored.size()), k);
  if (ef <= 0) return {};
  std::nth_element(scored.begin(), scored.begin() + ef, scored.end(),
                   [](const Cand& a, const Cand& b) {
                     if (a.first != b.first) return a.first < b.first;
                     return a.second < b.second;
                   });
  scored.resize(static_cast<size_t>(ef));
  // Exaktes Re-Rank mit vollen Vektoren via DiskSpill.
  std::vector<SearchHit> cand;
  cand.reserve(static_cast<size_t>(ef));
  for (const auto& pr : scored) {
    Vector v = vec_.get(static_cast<size_t>(pr.second));
    cand.push_back({pr.second, l2_distance(query, v)});
  }
  if (static_cast<int>(cand.size()) > k) {
    std::nth_element(cand.begin(), cand.begin() + k, cand.end(),
                     [](const SearchHit& a, const SearchHit& b) {
                       if (a.dist != b.dist) return a.dist < b.dist;
                       return a.id < b.id;
                     });
    cand.resize(static_cast<size_t>(k));
  }
  std::sort(cand.begin(), cand.end(),
            [](const SearchHit& a, const SearchHit& b) {
              if (a.dist != b.dist) return a.dist < b.dist;
              return a.id < b.id;
            });
  return cand;
}

}  // namespace dbengine::vector::quant
