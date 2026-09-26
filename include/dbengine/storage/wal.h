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
// s23: Group-Commit (append_batch unter einem Lock + ein fsync), WAIT FOR LSN
// (wait_for_lsn mit Timeout auf durable_lsn), CDC-Slots (read_from ab LSN).

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dbengine::storage {

struct WalRecord {
  uint64_t lsn = 0;
  std::string data;
};

// Audit-Konvention auf WAL-Basis (kein Format-Bruch: normale Records).
//
// Compliance-Vorstufe, KEIN Tamper-Schutz: Audit-Events sind normale
// WAL-Payloads ohne HMAC/Signatur/Kette. Jeder mit Dateizugriff kann sie
// faelschen, umschreiben oder per checkpoint() verwerfen. Keine
// Vollstaendigkeits- oder Unveraenderbarkeitsgarantie — nur Konvention
// zur Filterung per Prefix.
//
// Encoding (Payload, UTF-8/opak, '\0'-tolerant):
//   "AUD1\n" + esc(actor) + "\n" + esc(action) + "\n" + esc(detail)
// Escaping pro Feld (dokumentiert, reversibel):
//   '\\' -> "\\\\"  (Backslash zuerst escapen)
//   '\n' -> "\\n"   (echter Zeilenumbruch -> Backslash + 'n')
// Alle anderen Bytes (inkl. '\r', '\0', UTF-8) passieren unveraendert.
// Trennzeichen sind NUR unescapte '\n'; strikter Prefix-Match "AUD1\n"
// (5 Bytes), sonst gilt der Record als Nicht-Audit und wird geskippt.
struct AuditEvent {
  uint64_t lsn = 0;
  std::string actor;
  std::string action;
  std::string detail;
};

/// Parst einen WAL-Record als Audit-Event. Gibt nullopt zurueck, wenn der
/// Payload nicht mit "AUD1\n" beginnt (strikter Prefix-Match) oder das
/// Restformat ungueltig ist (falsche Feldzahl, ungueltige Escape-Sequenz,
/// einsamer Backslash am Ende). Reine Funktion, kein Lock/Dateizugriff.
std::optional<AuditEvent> parse_audit(const WalRecord& rec);

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
  /// Batch-Append unter einem Lock (Group-Commit-Pfad): ein Lock-Erwerb fuer
  /// alle Records, ein fsync via flush() danach. Gibt LSNs zurueck.
  std::vector<uint64_t> append_many(const std::vector<std::string>& payloads);
  /// Letzte dauerhafte LSN (nach flush/open/checkpoint aktualisiert).
  uint64_t durable_lsn() const;
  /// WAIT FOR LSN: blockiert bis durable_lsn() >= target oder Timeout.
  /// timeout_ms < 0 = unendlich. Rueckgabe true bei erreicht, false bei Timeout.
  bool wait_for_lsn(uint64_t target, int timeout_ms = -1) const;
  /// CDC: alle Records mit lsn >= from_lsn (aufsteigend). max_records==0: alle.
  std::vector<WalRecord> read_from(uint64_t from_lsn,
                                   size_t max_records = 0);
  /// Audit-Append (normale Record-Payload nach AUD1-Konvention, s. oben).
  /// Gibt die LSN zurueck. Wirft bei Ueberschreitung von kMaxPayload
  /// (nach Escaping). Beeinflusst bestehende Tests nicht (neue API only).
  uint64_t append_audit(std::string_view actor, std::string_view action,
                        std::string_view detail);
  /// Audit-Read: read_from(from_lsn) + parse_audit + nur AUD1-Eintraege
  /// (strikter Prefix-Match, sonst skip, LSN-Ordnung bleibt). max_records==0:
  /// alle Audit-Events; sonst max. so viele Audit-Events (Limit zaehlt
  /// gefilterte Events, nicht gescannte Records).
  std::vector<AuditEvent> read_audit(uint64_t from_lsn,
                                     size_t max_records = 0);
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

  struct GroupStats {
    uint64_t appends = 0;
    uint64_t flushes = 0;
  };
  GroupStats group_stats() const;

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
  uint64_t durable_lsn_ = 0;
  uint64_t appends_ = 0;
  uint64_t flushes_ = 0;
  mutable std::mutex mu_;
  mutable std::condition_variable cv_;
};

// CDC-Slot (leichtgewichtig, kein Hintergrund-Thread): cursor-basiertes Pollen
// ueber Wal::read_from. Nach poll() liegt cursor auf (letzte gesehene LSN + 1).
class WalCdcSlot {
 public:
  explicit WalCdcSlot(Wal* wal, uint64_t from_lsn = 1)
      : wal_(wal), cursor_(from_lsn) {}
  std::vector<WalRecord> poll(size_t max_records = 0);
  uint64_t cursor() const { return cursor_; }
  void seek(uint64_t lsn) { cursor_ = lsn; }

 private:
  Wal* wal_ = nullptr;
  uint64_t cursor_ = 1;
};

}  // namespace dbengine::storage
