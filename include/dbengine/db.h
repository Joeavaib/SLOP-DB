#pragma once

// Phase-0-MVP Container-Datei (s72).
//
// Format (little-endian, alles fix):
//   offset 0:  magic[6]  = "DBEN01"
//   offset 6:  version u32 = kFileVersion (1)
//   offset 10: region-table, 4 Eintraege je {offset u64, size u64}:
//                [0] catalog, [1] wal-seg, [2] pager, [3] meta
//   offset 74: crc32 u32 ueber Bytes [0, 74) (IEEE 0xEDB88320, Seed 0)
//   Header total: kHeaderSize = 78 Bytes.
// Body: Regionen direkt hinter Header, lueckenlos, genullt reserviert:
//   catalog @78 size 4096, wal-seg @4174 size 16384,
//   pager @20558 size 16384, meta @36942 size 4096.
//   Datei total: kFileSize = 41038 Bytes.
// Region-Inhalte sind in s72 NUR reserviert+genullt; s73 (B-Tree) und
// s74 (SQL) fuellen sie spaeter. Pager/WAL-Header-APIs werden hier NICHT
// eingebunden (reiner Container, STL/POSIX-only).

#include <cstdint>
#include <string>

namespace dbengine {

struct DbRegion {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

class Db {
 public:
  static constexpr char kMagic[6] = {'D', 'B', 'E', 'N', '0', '1'};
  static constexpr std::uint32_t kFileVersion = 1;
  static constexpr std::size_t kRegionCount = 4;
  // Reihenfolge in der Region-Table.
  static constexpr std::size_t kCatalogIdx = 0;
  static constexpr std::size_t kWalIdx = 1;
  static constexpr std::size_t kPagerIdx = 2;
  static constexpr std::size_t kMetaIdx = 3;

  static constexpr std::uint64_t kCatalogSize = 4096;
  static constexpr std::uint64_t kWalSize = 16384;
  static constexpr std::uint64_t kPagerSize = 16384;
  static constexpr std::uint64_t kMetaSize = 4096;

  static constexpr std::size_t kHeaderSize = 6 + 4 + 4 * 16 + 4;  // 78
  static constexpr std::uint64_t kFileSize =
      kHeaderSize + kCatalogSize + kWalSize + kPagerSize + kMetaSize;  // 41038

  // Oeffnet path (erzeugt via tmp+rename+fsync falls fehlend, sonst
  // oeffnet+validiert Magic/Version/CRC/Groesse). Fehler -> wirft
  // std::runtime_error / std::system_error, kein Teilzustand.
  static Db open(const std::string& path);

  Db() = default;
  ~Db() { close(); }

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;
  Db(Db&& other) noexcept;
  Db& operator=(Db&& other) noexcept;

  void close();
  [[nodiscard]] bool is_open() const { return open_; }
  [[nodiscard]] const std::string& path() const { return path_; }

  // Region-Table des geoeffneten Images (fuer spaetere s73/s74-Integration).
  [[nodiscard]] DbRegion region(std::size_t idx) const;
  [[nodiscard]] DbRegion catalog_region() const { return region(kCatalogIdx); }
  [[nodiscard]] DbRegion wal_region() const { return region(kWalIdx); }
  [[nodiscard]] DbRegion pager_region() const { return region(kPagerIdx); }
  [[nodiscard]] DbRegion meta_region() const { return region(kMetaIdx); }

  // CRC32-IEEE (tabellengetrieben). "123456789" -> 0xCBF43926.
  static std::uint32_t crc32(const void* data, std::size_t n,
                             std::uint32_t seed = 0);

 private:
  explicit Db(std::string path) : path_(std::move(path)) {}

  std::string path_;
  bool open_ = false;
  DbRegion regions_[kRegionCount]{};
};

}  // namespace dbengine
