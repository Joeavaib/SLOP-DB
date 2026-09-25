// s16-quant Implementierung: SQ8 echt + PQ-Stub(2) + DiskSpill(mmap) +
// ef-Autotune + ADC-Rerank-Suche.

#include "dbengine/vector/quant.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
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
  double acc = 0.0;
  for (int i = 0; i < n; ++i) {
    const double d = static_cast<double>(a[i]) - b[i];
    acc += d * d;
  }
  return static_cast<float>(acc);
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

}  // namespace dbengine::vector::quant
