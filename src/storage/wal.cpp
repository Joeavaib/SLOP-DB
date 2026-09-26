// WAL-Implementierung: append (LSN), flush (fsync-Policy), replay, checkpoint,
// CRC32-lite. POSIX-Pfad (Linux): open/write/fdatasync/rename fuer Kontrolle
// ueber Crash-Safety. Ein fsync pro flush(), nicht pro Record.

#include "dbengine/storage/wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <sys/stat.h>
#include <sys/uio.h>

namespace dbengine::storage {
namespace {

// Little-endian (de)serialisierung — portabel, unabhaengig von Host-Endianness.
void put_u32le(char* p, uint32_t v) {
  p[0] = static_cast<char>(v & 0xFF);
  p[1] = static_cast<char>((v >> 8) & 0xFF);
  p[2] = static_cast<char>((v >> 16) & 0xFF);
  p[3] = static_cast<char>((v >> 24) & 0xFF);
}
void put_u64le(char* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}
uint32_t get_u32le(const char* p) {
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}
uint64_t get_u64le(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

bool read_full(int fd, void* buf, size_t n) {
  char* p = static_cast<char*>(buf);
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::read(fd, p + got, n - got);
    if (r == 0) return false;  // EOF -> torn
    if (r < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("WAL read: ") + std::strerror(errno));
    }
    got += static_cast<size_t>(r);
  }
  return true;
}

void fsync_file(int fd) {
  // fdatasync reicht (keine Metadaten-Aenderung ausser Size; Size ist kritisch,
  // fdatasync synced sie). Fallback fsync.
#ifdef __linux__
  if (::fdatasync(fd) == 0) return;
#endif
  if (::fsync(fd) != 0)
    throw std::runtime_error(std::string("WAL fsync: ") + std::strerror(errno));
}

void fsync_dir_of(const std::string& path) {
  // Nach rename() Directory fsyncen, damit der Rename selbst Kill--9-sicher ist.
  auto slash = path.find_last_of('/');
  std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
  if (dir.empty()) dir = ".";
  int dfd = ::open(dir.c_str(), O_RDONLY
#ifdef O_DIRECTORY
                                 | O_DIRECTORY
#endif
  );
  if (dfd < 0) return;  // best effort
  (void)::fsync(dfd);
  ::close(dfd);
}

// Ein writev()-Syscall pro Record (Header+Payload atomar im Syscall-Sinn,
// weiterhin ohne fsync). Loop nur fuer Partial-Writes/EINTR; Bytes/Format/CRC
// identisch zu vorher zwei write().
void writev_all(int fd, struct iovec* iov, int iovcnt) {
  while (iovcnt > 0) {
    ssize_t w = ::writev(fd, iov, iovcnt);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("WAL writev: ") + std::strerror(errno));
    }
    if (w == 0) continue;  // dürfte bei len>0 nicht passieren; erneut versuchen
    ssize_t left = w;
    while (left > 0 && iovcnt > 0) {
      size_t cur = iov[0].iov_len;
      if (static_cast<size_t>(left) >= cur) {
        left -= static_cast<ssize_t>(cur);
        ++iov;
        --iovcnt;
      } else {
        iov[0].iov_base = static_cast<char*>(iov[0].iov_base) + left;
        iov[0].iov_len = cur - static_cast<size_t>(left);
        left = 0;
      }
    }
  }
}

inline void write_record(int fd, const char hdr[20], const char* payload,
                         uint32_t len) {
  struct iovec iov[2];
  iov[0].iov_base = const_cast<char*>(hdr);
  iov[0].iov_len = 20;
  int cnt = 1;
  if (len) {
    iov[1].iov_base = const_cast<char*>(payload);
    iov[1].iov_len = len;
    cnt = 2;
  }
  writev_all(fd, iov, cnt);
}

}  // namespace

Wal::Wal(std::string path) : path_(std::move(path)) {}
Wal::~Wal() { close(); }

void Wal::crc32_table_init(uint32_t t[256]) {
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
}

uint32_t Wal::crc32(const void* data, size_t n, uint32_t seed) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    crc32_table_init(table);
    init = true;
  }
  uint32_t c = ~seed;
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

uint32_t Wal::record_crc(uint64_t lsn, uint32_t len, const char* payload) {
  char hdr[12];
  put_u64le(hdr, lsn);
  put_u32le(hdr + 8, len);
  uint32_t c = crc32(hdr, sizeof(hdr), 0);
  if (len && payload) {
    // Weiterketten: CRC ueber Payload mit Seed = bisheriger CRC.
    // crc32(seed) erwartet ~seed intern, daher Rekombination ueber raw-Form:
    // einfacher & korrekt: CRC ueber Konkatenation via zwei Schritte geht mit
    // dieser Implementierung nicht direkt — berechne CRC ueber hdr+payload
    // sequentiell neu: wir nutzen internen Loop ueber beide Teile.
    // Pragmatisch: CRC(hdr || payload) = f(f(hdr), payload). Da unsere
    // crc32(seed) als ~seed startet und ~c endet, gilt die Verkettung nicht
    // exakt — deshalb berechnen wir hier explizit ueber kopierten Puffer nur
    // fuer kleine Records direkt; fuer grosse chunkweise mit raw-State.
    // Einfachste korrekte Variante: CRC ueber hdr, dann weiter mit payload
    // im selben ~c-Raum:
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
      crc32_table_init(table);
      init = true;
    }
    uint32_t raw = ~c;  // zurueck in Arbeitsraum
    const auto* p = reinterpret_cast<const unsigned char*>(payload);
    for (uint32_t i = 0; i < len; ++i) raw = table[(raw ^ p[i]) & 0xFF] ^ (raw >> 8);
    return ~raw;
  }
  return c;
}

void Wal::write_all(int fd, const void* buf, size_t n) {
  const char* p = static_cast<const char*>(buf);
  size_t done = 0;
  while (done < n) {
    ssize_t w = ::write(fd, p + done, n - done);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("WAL write: ") + std::strerror(errno));
    }
    done += static_cast<size_t>(w);
  }
}

int64_t Wal::file_size(int fd) {
  struct stat st {};
  if (::fstat(fd, &st) != 0)
    throw std::runtime_error(std::string("WAL fstat: ") + std::strerror(errno));
  return static_cast<int64_t>(st.st_size);
}

Wal::ScanResult Wal::scan(int fd) {
  ScanResult out;
  if (::lseek(fd, 0, SEEK_SET) < 0)
    throw std::runtime_error(std::string("WAL lseek: ") + std::strerror(errno));
  int64_t pos = 0;
  for (;;) {
    char hdr[4 + 8 + 4 + 4];
    if (!read_full(fd, hdr, sizeof(hdr))) break;  // EOF / torn header -> stop
    if (get_u32le(hdr) != kMagic) break;          // Korrupt -> torn tail, stop
    uint64_t lsn = get_u64le(hdr + 4);
    uint32_t len = get_u32le(hdr + 12);
    uint32_t want = get_u32le(hdr + 16);
    if (len > kMaxPayload) break;  // Korrupt -> stop
    if (lsn == 0 || lsn < out.max_lsn) break;  // LSN muss steigen (0 ungueltig)
    std::string payload;
    payload.resize(len);
    if (len && !read_full(fd, payload.data(), len)) break;  // torn payload
    if (record_crc(lsn, len, payload.data()) != want) break;  // CRC -> stop
    out.max_lsn = lsn;
    pos += static_cast<int64_t>(sizeof(hdr) + len);
    out.valid_bytes = pos;
    out.records.push_back(WalRecord{lsn, std::move(payload)});
  }
  return out;
}

void Wal::ensure_open() {
  if (fd_ < 0) throw std::runtime_error("WAL not open: " + path_);
}

void Wal::open() {
  std::unique_lock<std::mutex> g(mu_);
  if (fd_ >= 0) return;
  int fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0)
    throw std::runtime_error(std::string("WAL open: ") + std::strerror(errno));
  // Recovery beim Start: scanne, setze next_lsn, kappe torn tail.
  ScanResult s = scan(fd);
  int64_t sz = file_size(fd);
  if (sz > s.valid_bytes) {
    // Torn tail kappen (abgerissener write nach Crash).
    if (::ftruncate(fd, s.valid_bytes) != 0) {
      ::close(fd);
      throw std::runtime_error(std::string("WAL ftruncate: ") + std::strerror(errno));
    }
    fsync_file(fd);
  }
  next_lsn_ = s.max_lsn + 1;
  durable_lsn_ = s.max_lsn;  // nach Truncate+fsync ist Dateiinhalt dauerhaft
  // Append-Position ans Ende.
  if (::lseek(fd, 0, SEEK_END) < 0) {
    ::close(fd);
    throw std::runtime_error(std::string("WAL lseek-end: ") + std::strerror(errno));
  }
  fd_ = fd;
  g.unlock();
  cv_.notify_all();
}

uint64_t Wal::append(std::string_view payload) {
  std::lock_guard<std::mutex> g(mu_);
  ensure_open();
  if (payload.size() > kMaxPayload) throw std::runtime_error("WAL payload too large");
  uint64_t lsn = next_lsn_++;
  uint32_t len = static_cast<uint32_t>(payload.size());
  char hdr[4 + 8 + 4 + 4];
  put_u32le(hdr, kMagic);
  put_u64le(hdr + 4, lsn);
  put_u32le(hdr + 12, len);
  put_u32le(hdr + 16, record_crc(lsn, len, payload.data()));
  write_record(fd_, hdr, payload.data(), len);
  // KEIN fsync hier — flush() macht Group-Commit (Performance).
  ++appends_;
  return lsn;
}

std::vector<uint64_t> Wal::append_many(
    const std::vector<std::string>& payloads) {
  std::lock_guard<std::mutex> g(mu_);
  ensure_open();
  std::vector<uint64_t> out;
  out.reserve(payloads.size());
  for (const auto& p : payloads) {
    if (p.size() > kMaxPayload) throw std::runtime_error("WAL payload too large");
    uint64_t lsn = next_lsn_++;
    uint32_t len = static_cast<uint32_t>(p.size());
    char hdr[4 + 8 + 4 + 4];
    put_u32le(hdr, kMagic);
    put_u64le(hdr + 4, lsn);
    put_u32le(hdr + 12, len);
    put_u32le(hdr + 16, record_crc(lsn, len, p.data()));
    write_record(fd_, hdr, p.data(), len);
    ++appends_;
    out.push_back(lsn);
  }
  return out;
}

void Wal::flush() {
  std::unique_lock<std::mutex> g(mu_);
  ensure_open();
  fsync_file(fd_);
  durable_lsn_ = (next_lsn_ == 0) ? 0 : next_lsn_ - 1;
  ++flushes_;
  g.unlock();
  cv_.notify_all();
}

std::vector<WalRecord> Wal::replay() {
  std::lock_guard<std::mutex> g(mu_);
  ensure_open();
  // Verlaufsgarantie: was der Aufrufer lesen will, muss vorher flush() sein.
  // Wir lesen vom Dateianfang; scan() toleriert torn tail.
  ScanResult s = scan(fd_);
  if (::lseek(fd_, 0, SEEK_END) < 0)
    throw std::runtime_error(std::string("WAL lseek-end: ") + std::strerror(errno));
  return s.records;
}

std::vector<WalRecord> Wal::replay_file(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) return {};  // keine Datei -> leeres Log
    throw std::runtime_error(std::string("WAL replay open: ") + std::strerror(errno));
  }
  ScanResult s = scan(fd);
  ::close(fd);
  return s.records;
}

void Wal::checkpoint(uint64_t checkpoint_lsn) {
  std::unique_lock<std::mutex> g(mu_);
  ensure_open();
  ScanResult s = scan(fd_);
  std::string tmp = path_ + ".chkpt.tmp";
  int tfd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (tfd < 0)
    throw std::runtime_error(std::string("WAL checkpoint open: ") + std::strerror(errno));
  uint64_t max_kept = 0;
  for (const auto& r : s.records) {
    if (r.lsn <= checkpoint_lsn) continue;
    uint32_t len = static_cast<uint32_t>(r.data.size());
    char hdr[4 + 8 + 4 + 4];
    put_u32le(hdr, kMagic);
    put_u64le(hdr + 4, r.lsn);
    put_u32le(hdr + 12, len);
    put_u32le(hdr + 16, record_crc(r.lsn, len, r.data.data()));
    write_record(tfd, hdr, r.data.data(), len);
    max_kept = r.lsn;
  }
  fsync_file(tfd);
  ::close(tfd);
  if (::rename(tmp.c_str(), path_.c_str()) != 0)
    throw std::runtime_error(std::string("WAL checkpoint rename: ") + std::strerror(errno));
  fsync_dir_of(path_);
  // Altes fd zeigt noch auf unlinked inode -> schliessen + neu oeffnen.
  ::close(fd_);
  int fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0)
    throw std::runtime_error(std::string("WAL reopen: ") + std::strerror(errno));
  if (::lseek(fd, 0, SEEK_END) < 0) {
    ::close(fd);
    throw std::runtime_error(std::string("WAL lseek-end: ") + std::strerror(errno));
  }
  fd_ = fd;
  next_lsn_ = max_kept + 1;
  if (next_lsn_ == 0) next_lsn_ = 1;
  if (max_kept + 1 == next_lsn_) durable_lsn_ = max_kept;
  g.unlock();
  cv_.notify_all();
}

void Wal::close() {
  std::lock_guard<std::mutex> g(mu_);
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

uint64_t Wal::next_lsn() const {
  std::lock_guard<std::mutex> g(mu_);
  return next_lsn_;
}

uint64_t Wal::durable_lsn() const {
  std::lock_guard<std::mutex> g(mu_);
  return durable_lsn_;
}

Wal::GroupStats Wal::group_stats() const {
  std::lock_guard<std::mutex> g(mu_);
  return GroupStats{appends_, flushes_};
}

bool Wal::wait_for_lsn(uint64_t target, int timeout_ms) const {
  std::unique_lock<std::mutex> g(mu_);
  auto pred = [&] { return durable_lsn_ >= target; };
  if (pred()) return true;
  if (timeout_ms < 0) {
    cv_.wait(g, pred);
    return true;
  }
  if (timeout_ms == 0) return false;
  return cv_.wait_for(g, std::chrono::milliseconds(timeout_ms), pred);
}

std::vector<WalRecord> Wal::read_from(uint64_t from_lsn, size_t max_records) {
  // replay() lockt intern; hier nicht halten (kein Double-Lock).
  std::vector<WalRecord> all = replay();
  std::vector<WalRecord> out;
  out.reserve(all.size());
  for (auto& r : all) {
    if (r.lsn < from_lsn) continue;
    out.push_back(std::move(r));
    if (max_records && out.size() >= max_records) break;
  }
  return out;
}

std::vector<WalRecord> WalCdcSlot::poll(size_t max_records) {
  if (!wal_) return {};
  auto recs = wal_->read_from(cursor_, max_records);
  if (!recs.empty()) cursor_ = recs.back().lsn + 1;
  return recs;
}

}  // namespace dbengine::storage
