// Phase-0-MVP Container-Datei (s72): Db::open/close + Header-Format.
// STL/POSIX-only, C++20, -Wall sauber. Keine Abhaengigkeit von
// pager.h/wal.h (reiner Container; Integration erst s73/s74).

#include "dbengine/db.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <sys/stat.h>

namespace dbengine {
namespace {

void put_u32le(char* p, std::uint32_t v) {
  p[0] = static_cast<char>(v & 0xFFu);
  p[1] = static_cast<char>((v >> 8) & 0xFFu);
  p[2] = static_cast<char>((v >> 16) & 0xFFu);
  p[3] = static_cast<char>((v >> 24) & 0xFFu);
}

void put_u64le(char* p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<char>((v >> (8 * i)) & 0xFFu);
}

std::uint32_t get_u32le(const char* p) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

std::uint64_t get_u64le(const char* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

void write_all(int fd, const void* buf, std::size_t n, const char* what) {
  const char* p = static_cast<const char*>(buf);
  std::size_t done = 0;
  while (done < n) {
    ssize_t w = ::write(fd, p + done, n - done);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw std::system_error(errno, std::generic_category(), what);
    }
    done += static_cast<std::size_t>(w);
  }
}

void fsync_file(int fd, const char* what) {
#ifdef __linux__
  if (::fdatasync(fd) == 0) return;
#endif
  if (::fsync(fd) != 0)
    throw std::system_error(errno, std::generic_category(), what);
}

void fsync_dir_of(const std::string& path) {
  auto slash = path.find_last_of('/');
  std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
  if (dir.empty()) dir = ".";
  int dfd = ::open(dir.c_str(), O_RDONLY
#ifdef O_DIRECTORY
                                    | O_DIRECTORY
#endif
  );
  if (dfd < 0) return;  // best effort: Rename ist auch ohne dir-fsync sichtbar
  ::fsync(dfd);
  ::close(dfd);
}

// Serialisiert den vollstaendigen Header (kHeaderSize Bytes, inkl. CRC).
std::vector<char> build_header() {
  std::vector<char> h(Db::kHeaderSize, 0);
  h[0] = 'D';
  h[1] = 'B';
  h[2] = 'E';
  h[3] = 'N';
  h[4] = '0';
  h[5] = '1';
  put_u32le(h.data() + 6, Db::kFileVersion);
  std::uint64_t off = Db::kHeaderSize;
  const std::uint64_t sizes[Db::kRegionCount] = {Db::kCatalogSize, Db::kWalSize,
                                                 Db::kPagerSize, Db::kMetaSize};
  for (std::size_t i = 0; i < Db::kRegionCount; ++i) {
    put_u64le(h.data() + 10 + i * 16, off);
    put_u64le(h.data() + 10 + i * 16 + 8, sizes[i]);
    off += sizes[i];
  }
  const std::uint32_t crc =
      Db::crc32(h.data(), Db::kHeaderSize - 4);
  put_u32le(h.data() + Db::kHeaderSize - 4, crc);
  return h;
}

// Legt eine neue Datei crash-sicher an: tmp schreiben + fsync + rename +
// dir-fsync. Danach ist entweder die neue komplette Datei oder (falls path
// schon existierte und rename fehlschlaegt) der Altstand sichtbar — nie ein
// torn Mix. Existiert path bereits, wird NICHT ueberschrieben (EEXIST-Check
// macht der Aufrufer; hier O_EXCL auf dem tmp-Pfad schuetzt nur tmp).
void create_new(const std::string& path) {
  const std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    throw std::system_error(errno, std::generic_category(),
                            "db create tmp open");
  try {
    const std::vector<char> h = build_header();
    write_all(fd, h.data(), h.size(), "db create header write");
    // Regionen genullt reservieren: in 64-KiB-Chunks nullen schreiben
    // (sparse wuerde auch gehen, explizite Nullen sind portabel lesbar).
    static constexpr std::size_t kChunk = 65536;
    std::vector<char> zero(kChunk, 0);
    std::uint64_t body = Db::kFileSize - Db::kHeaderSize;
    while (body > 0) {
      const std::size_t n = body > kChunk ? kChunk : static_cast<std::size_t>(body);
      write_all(fd, zero.data(), n, "db create body write");
      body -= n;
    }
    fsync_file(fd, "db create fsync");
    if (::close(fd) != 0) {
      fd = -1;
      throw std::system_error(errno, std::generic_category(),
                              "db create close");
    }
    fd = -1;
    if (::rename(tmp.c_str(), path.c_str()) != 0)
      throw std::system_error(errno, std::generic_category(),
                              "db create rename");
    fsync_dir_of(path);
  } catch (...) {
    if (fd >= 0) ::close(fd);
    ::unlink(tmp.c_str());
    throw;
  }
}

bool read_full(int fd, void* buf, std::size_t n) {
  char* p = static_cast<char*>(buf);
  std::size_t got = 0;
  while (got < n) {
    ssize_t r = ::read(fd, p + got, n - got);
    if (r == 0) return false;  // EOF -> torn
    if (r < 0) {
      if (errno == EINTR) continue;
      throw std::system_error(errno, std::generic_category(), "db read");
    }
    got += static_cast<std::size_t>(r);
  }
  return true;
}

}  // namespace

std::uint32_t Db::crc32(const void* data, std::size_t n, std::uint32_t seed) {
  static std::uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  std::uint32_t c = ~seed;
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  return ~c;
}

Db::Db(Db&& other) noexcept
    : path_(std::move(other.path_)), open_(other.open_) {
  for (std::size_t i = 0; i < kRegionCount; ++i) regions_[i] = other.regions_[i];
  other.open_ = false;
  other.path_.clear();
}

Db& Db::operator=(Db&& other) noexcept {
  if (this != &other) {
    close();
    path_ = std::move(other.path_);
    open_ = other.open_;
    for (std::size_t i = 0; i < kRegionCount; ++i)
      regions_[i] = other.regions_[i];
    other.open_ = false;
    other.path_.clear();
  }
  return *this;
}

Db Db::open(const std::string& path) {
  if (path.empty()) throw std::runtime_error("db open: empty path");

  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    if (errno != ENOENT)
      throw std::system_error(errno, std::generic_category(), "db stat");
    create_new(path);  // Datei fehlte -> neu anlegen
    if (::stat(path.c_str(), &st) != 0)
      throw std::system_error(errno, std::generic_category(),
                              "db stat after create");
  }

  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0)
    throw std::system_error(errno, std::generic_category(), "db open file");
  std::vector<char> h(kHeaderSize, 0);
  try {
    if (!read_full(fd, h.data(), kHeaderSize))
      throw std::runtime_error("db open: torn header (file too small)");
    if (static_cast<std::uint64_t>(st.st_size) < kFileSize)
      throw std::runtime_error("db open: torn file (size < header+regions)");
    if (std::memcmp(h.data(), kMagic, 6) != 0)
      throw std::runtime_error("db open: bad magic (not a dbengine file)");
    if (get_u32le(h.data() + 6) != kFileVersion)
      throw std::runtime_error("db open: unsupported version");
    // Region-Table einlesen + validieren (lueckenlos, sortiert, im File).
    const std::uint64_t sizes[kRegionCount] = {kCatalogSize, kWalSize,
                                               kPagerSize, kMetaSize};
    std::uint64_t expect_off = kHeaderSize;
    DbRegion regs[kRegionCount]{};
    for (std::size_t i = 0; i < kRegionCount; ++i) {
      const std::uint64_t off = get_u64le(h.data() + 10 + i * 16);
      const std::uint64_t sz = get_u64le(h.data() + 10 + i * 16 + 8);
      if (off != expect_off || sz != sizes[i])
        throw std::runtime_error("db open: bad region table");
      if (off + sz > static_cast<std::uint64_t>(st.st_size))
        throw std::runtime_error("db open: region outside file");
      regs[i].offset = off;
      regs[i].size = sz;
      expect_off += sz;
    }
    const std::uint32_t want = get_u32le(h.data() + kHeaderSize - 4);
    const std::uint32_t got = crc32(h.data(), kHeaderSize - 4);
    if (want != got) throw std::runtime_error("db open: header CRC mismatch");
    ::close(fd);
    Db db(path);
    for (std::size_t i = 0; i < kRegionCount; ++i) db.regions_[i] = regs[i];
    db.open_ = true;
    return db;
  } catch (...) {
    ::close(fd);
    throw;  // kein Teilzustand: kein Db-Objekt im open-Zustand entkommt
  }
}

void Db::close() {
  open_ = false;
}

DbRegion Db::region(std::size_t idx) const {
  if (!open_) throw std::runtime_error("db region: not open");
  if (idx >= kRegionCount) throw std::out_of_range("db region: bad index");
  return regions_[idx];
}

}  // namespace dbengine
