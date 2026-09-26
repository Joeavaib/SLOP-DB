// s72 dbfile-Tests (ohne gtest, check-basiert wie test_pager.cpp):
//  1. create-neu (Header lesbar, Groesse>0)
//  2. reopen-ok
//  3. korrupte Magic/Version/CRC -> open wirft
//  4. torn-Header (kuerzen) -> open wirft
// Temp-Pfade + Cleanup. Reine Container-Tests, kein Bezug auf andere Module.

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "dbengine/db.h"

namespace {

int failures = 0;
int tmp_counter = 0;

void check(bool cond, const char* what) {
  if (cond) {
    std::cout << "  ok: " << what << "\n";
  } else {
    std::cout << "  FAIL: " << what << "\n";
    ++failures;
  }
}

std::string tmp_path(const char* name) {
  auto p = std::filesystem::temp_directory_path() /
           (std::string(name) + "_" + std::to_string(static_cast<long long>(::getpid())) +
            "_" + std::to_string(tmp_counter++) + ".db");
  std::error_code ec;
  std::filesystem::remove(p, ec);
  return p.string();
}

void cleanup(const std::string& p) {
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p + ".tmp", ec);
}

// Liest rohen Header direkt von Platte (unabhaengig von Db::open).
std::vector<char> read_raw(const std::string& p, std::size_t n) {
  std::vector<char> b(n, 0);
  int fd = ::open(p.c_str(), O_RDONLY);
  if (fd < 0) return {};
  std::size_t got = 0;
  while (got < n) {
    ssize_t r = ::read(fd, b.data() + got, n - got);
    if (r <= 0) break;
    got += static_cast<std::size_t>(r);
  }
  ::close(fd);
  b.resize(got);
  return b;
}

std::uint32_t get_u32le(const char* p) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

void put_u32le(char* p, std::uint32_t v) {
  p[0] = static_cast<char>(v & 0xFFu);
  p[1] = static_cast<char>((v >> 8) & 0xFFu);
  p[2] = static_cast<char>((v >> 16) & 0xFFu);
  p[3] = static_cast<char>((v >> 24) & 0xFFu);
}

// Schreibt Bytes an Offset (fuer Korruptions-Faelle), mit fsync.
bool poke(const std::string& p, std::size_t off, const void* buf,
          std::size_t n) {
  int fd = ::open(p.c_str(), O_WRONLY);
  if (fd < 0) return false;
  bool ok = ::pwrite(fd, buf, n, static_cast<off_t>(off)) ==
            static_cast<ssize_t>(n);
  ::fsync(fd);
  ::close(fd);
  return ok;
}

bool threw_open(const std::string& p) {
  try {
    auto db = dbengine::Db::open(p);
    (void)db.is_open();
    return false;
  } catch (...) {
    return true;
  }
}

}  // namespace

int main() {
  using dbengine::Db;

  std::cout << "[dbfile] crc32 sanity\n";
  {
    const char* v = "123456789";
    check(Db::crc32(v, 9) == 0xCBF43926u, "crc32 IEEE check value");
  }

  std::cout << "[dbfile] create-neu (Header lesbar, Groesse>0)\n";
  {
    const std::string path = tmp_path("dbengine_db_create");
    Db db = Db::open(path);
    check(db.is_open(), "open new file");
    check(db.path() == path, "path() matches");
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    check(!ec && sz == Db::kFileSize, "file size == kFileSize");
    check(sz > 0, "size > 0");
    auto raw = read_raw(path, Db::kHeaderSize);
    check(raw.size() == Db::kHeaderSize, "header fully readable");
    check(std::memcmp(raw.data(), "DBEN01", 6) == 0, "magic DBEN01");
    check(get_u32le(raw.data() + 6) == Db::kFileVersion, "version == 1");
    check(Db::crc32(raw.data(), Db::kHeaderSize - 4) ==
              get_u32le(raw.data() + Db::kHeaderSize - 4),
          "header CRC matches");
    // Regionen genullt reserviert: Spot-Checks in jeder Region = 0.
    auto body = read_raw(path, static_cast<std::size_t>(Db::kFileSize));
    bool zeros = body.size() == Db::kFileSize;
    for (std::size_t i : {Db::kCatalogIdx, Db::kWalIdx, Db::kPagerIdx,
                          Db::kMetaIdx}) {
      dbengine::DbRegion r = db.region(i);
      if (r.size == 0 || r.offset + r.size > body.size()) {
        zeros = false;
        break;
      }
      if (body[r.offset] != 0 || body[r.offset + r.size - 1] != 0) {
        zeros = false;
        break;
      }
    }
    check(zeros, "regions reserved + zeroed (spot check)");
    // Region-Table lueckenlos ab Header.
    check(db.catalog_region().offset == Db::kHeaderSize, "catalog offset");
    check(db.catalog_region().size == Db::kCatalogSize, "catalog size");
    check(db.wal_region().size == Db::kWalSize, "wal size");
    check(db.pager_region().size == Db::kPagerSize, "pager size");
    check(db.meta_region().size == Db::kMetaSize, "meta size");
    db.close();
    check(!db.is_open(), "close() clears is_open");
    cleanup(path);
  }

  std::cout << "[dbfile] reopen-ok\n";
  {
    const std::string path = tmp_path("dbengine_db_reopen");
    {
      Db db = Db::open(path);
      check(db.is_open(), "first open");
      db.close();
    }
    {
      Db db = Db::open(path);
      check(db.is_open(), "reopen ok");
      check(db.path() == path, "reopen path matches");
      auto raw = read_raw(path, Db::kHeaderSize);
      check(std::memcmp(raw.data(), "DBEN01", 6) == 0, "magic after reopen");
      db.close();
    }
    // Leere path-Argumente muessen werfen, nicht anlegen.
    check(threw_open(""), "empty path throws");
    cleanup(path);
  }

  std::cout << "[dbfile] korrupte Magic wirft\n";
  {
    const std::string path = tmp_path("dbengine_db_badmagic");
    {
      Db db = Db::open(path);
      db.close();
    }
    const char bad[6] = {'X', 'B', 'E', 'N', '0', '1'};
    check(poke(path, 0, bad, 6), "poke bad magic");
    check(threw_open(path), "bad magic -> open throws");
    cleanup(path);
  }

  std::cout << "[dbfile] korrupte Version wirft\n";
  {
    const std::string path = tmp_path("dbengine_db_badver");
    {
      Db db = Db::open(path);
      db.close();
    }
    char v[4];
    put_u32le(v, 9999u);
    check(poke(path, 6, v, 4), "poke bad version");
    check(threw_open(path), "bad version -> open throws");
    cleanup(path);
  }

  std::cout << "[dbfile] korrupte CRC wirft\n";
  {
    const std::string path = tmp_path("dbengine_db_badcrc");
    {
      Db db = Db::open(path);
      db.close();
    }
    // Ein Byte in der Region-Table flippen (CRC danach falsch).
    auto raw = read_raw(path, Db::kHeaderSize);
    char flip = static_cast<char>(raw[20] ^ 0xFF);
    check(poke(path, 20, &flip, 1), "poke flipped byte");
    check(threw_open(path), "bad CRC -> open throws");
    cleanup(path);
  }

  std::cout << "[dbfile] korrupte Region-Table wirft\n";
  {
    const std::string path = tmp_path("dbengine_db_badregion");
    {
      Db db = Db::open(path);
      db.close();
    }
    // Region-Offset aendern + CRC neu berechnen, damit NUR die Table-Pruefung
    // greift (nicht die CRC-Pruefung).
    auto raw = read_raw(path, Db::kHeaderSize);
    check(raw.size() == Db::kHeaderSize, "raw header for region test");
    raw[10] ^= static_cast<char>(0xFF);
    put_u32le(raw.data() + Db::kHeaderSize - 4, 0);  // Platzhalter
    std::uint32_t c = Db::crc32(raw.data(), Db::kHeaderSize - 4);
    put_u32le(raw.data() + Db::kHeaderSize - 4, c);
    int fd = ::open(path.c_str(), O_WRONLY);
    check(fd >= 0, "open for region rewrite");
    if (fd >= 0) {
      ssize_t w = ::pwrite(fd, raw.data(), raw.size(), 0);
      check(w == static_cast<ssize_t>(raw.size()), "rewrite header");
      ::fsync(fd);
      ::close(fd);
    }
    check(threw_open(path), "bad region table -> open throws");
    cleanup(path);
  }

  std::cout << "[dbfile] torn-Header (kuerzen) wirft\n";
  {
    const std::string path = tmp_path("dbengine_db_torn");
    {
      Db db = Db::open(path);
      db.close();
    }
    int fd = ::open(path.c_str(), O_RDWR);
    check(fd >= 0, "open for truncate");
    if (fd >= 0) {
      check(::ftruncate(fd, 10) == 0, "truncate to 10 bytes");
      ::fsync(fd);
      ::close(fd);
    }
    check(threw_open(path), "torn header (10B) -> open throws");
    // Auch Header-minus-1 muss werfen.
    {
      Db db = Db::open(path + ".fresh");
      db.close();
      int fd2 = ::open((path + ".fresh").c_str(), O_RDWR);
      if (fd2 >= 0) {
        ::ftruncate(fd2, static_cast<off_t>(Db::kHeaderSize - 1));
        ::fsync(fd2);
        ::close(fd2);
      }
      check(threw_open(path + ".fresh"), "torn header (H-1) -> open throws");
      cleanup(path + ".fresh");
    }
    cleanup(path);
  }

  if (failures == 0) {
    std::cout << "DBFILE TESTS PASSED\n";
    return 0;
  }
  std::cout << "DBFILE TESTS FAILED (" << failures << ")\n";
  return 1;
}
