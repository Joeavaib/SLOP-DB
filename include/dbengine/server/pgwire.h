#pragma once

// PGWire-Stub V1: Startup-Handshake + Simple Query ('Q') + ErrorResponse.
// Kompatibel zum PostgreSQL Frontend/Backend-Protokoll (libpq):
//   Startup: Int32 len | Int32 proto(196608) | k\0v\0 ... \0
//   Simple Query: 'Q' | Int32 len | query\0
//   ErrorResponse: 'E' | Int32 len | Feld* | '\0'  (Felder: S,V,C,M)
// Full extended protocol (Parse/Bind/Describe/Execute/Sync), Auth (SASL),
// SSLRequest (80877103) und Cancel werden in V1 explizit abgelehnt.

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dbengine::pgwire {

struct ProtoError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct StartupParams {
  std::string user;
  std::string database;
  std::map<std::string, std::string> params;
};

constexpr int32_t kProtocolV3 = 196608;      // 3.0
constexpr int32_t kSslRequestCode = 80877103;
constexpr int32_t kCancelRequestCode = 80877102;

// Big-Endian Helpers (Netzwerk-Reihenfolge)
void putInt32BE(std::vector<uint8_t>& out, int32_t v);
int32_t getInt32BE(const uint8_t* p);

// Startup-Paket (ohne fuehrendes Typ-Byte) parsen.
StartupParams parseStartup(const uint8_t* data, std::size_t len);
inline StartupParams parseStartup(const std::vector<uint8_t>& buf) {
  return parseStartup(buf.data(), buf.size());
}

// Einfaches 'Q'-Paket (inkl. Typ-Byte) parsen. Gibt nullopt wenn kein 'Q'.
std::optional<std::string> parseQueryMessage(const uint8_t* data,
                                             std::size_t len);
inline std::optional<std::string> parseQueryMessage(
    const std::vector<uint8_t>& buf) {
  return parseQueryMessage(buf.data(), buf.size());
}

// Server-Nachrichten bauen (jeweils inkl. Typ-Byte + Laenge).
std::vector<uint8_t> encodeError(const std::string& severity,
                                 const std::string& code,
                                 const std::string& message);
std::vector<uint8_t> encodeReadyForQuery(char status = 'I');
std::vector<uint8_t> encodeCommandComplete(const std::string& tag);
std::vector<uint8_t> encodeRowDescription(
    const std::vector<std::string>& columns);
std::vector<uint8_t> encodeDataRow(const std::vector<std::string>& values);
std::vector<uint8_t> encodeStartupRequest(
    const std::map<std::string, std::string>& params);  // fuer Tests/Client

}  // namespace dbengine::pgwire
