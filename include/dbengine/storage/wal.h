#pragma once

// WAL (Write-Ahead Log) — REDO-Log mit Checkpoint + Crash-Recovery.
//
// Format pro Record (little-endian, append-only):
//   magic u32  = 0x57414C31 ("WAL1")
//   lsn   u64  = log sequence number, streng monoton steigend ab 1
//   len   u32  = payload bytes
//   crc   u32  = CRC32-lite (IEEE-Polynom) ueber lsn-Bytes + len-Bytes + payload
//   payload[len]
//
// Crash-Safety-Modell (ohne Performance-Bremse):
//   - append() schreibt nur in den OS-Page-Cache (write), KEIN fsync pro Record.
//   - flush() macht genau einen fdatasync/fsync (group commit).
//   - Regel: nach flush() ist alles bis dahin Kill--9-sicher.
//   - replay() toleriert torn tail (abgerissener letzter Record -> Prefix gewinnen).
//   - checkpoint(lsn) verwirft alle Records <= lsn, crash-sicher via tmp+rename.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace dbengine::storage {

struct WalRecord {
  uint64_t lsn = 0;
  std::string data;
};

class Wal {
 public:
  static constexpr uint32_t kMagic = 0x57414C31u;  // "WAL1"
  static constexpr uint32_t kMaxPayload = 16u * 1024u * 1024u;  // 16 MiB

  explicit Wal(std::string path);
  ~Wal();

  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  /// Oeffnet (erstellt falls noetig), scannt max-LSN und kappt torn tail.
  void open();
  /// Schreibt einen Record, gibt LSN zurueck. Kein fsync (siehe flush()).
  uint64_t append(std::string_view payload);
  /// Macht alle appends seit open/letztem flush dauerhaft (fdatasync/fsync).
  void flush();
  /// Liest alle gueltigen Records ab Dateianfang (torn tail -> Prefix).
  std::vector<WalRecord> replay();
  /// Statische Variante ohne offene Instanz (fuer Recovery beim Start).
  static std::vector<WalRecord> replay_file(const std::string& path);
  /// Verwirft alle Records mit lsn <= checkpoint_lsn (crash-sicher).
  void checkpoint(uint64_t checkpoint_lsn);
  void close();

  uint64_t next_lsn() const;
  const std::string& path() const { return path_; }
  bool is_open() const { return fd_ >= 0; }

  /// CRC32-lite (IEEE 0xEDB88320), tabellengetrieben.
  static uint32_t crc32(const void* data, size_t n, uint32_t seed = 0);

 private:
  static void crc32_table_init(uint32_t t[256]);
  static uint32_t record_crc(uint64_t lsn, uint32_t len, const char* payload);

  void ensure_open();
  static void write_all(int fd, const void* buf, size_t n);
  // Scannt Datei, gibt (records, gueltige_bytes, max_lsn) zurueck.
  struct ScanResult {
    std::vector<WalRecord> records;
    int64_t valid_bytes = 0;
    uint64_t max_lsn = 0;
  };
  static ScanResult scan(int fd);
  static int64_t file_size(int fd);

  std::string path_;
  int fd_ = -1;
  uint64_t next_lsn_ = 1;
  mutable std::mutex mu_;
};

}  // namespace dbengine::storage
