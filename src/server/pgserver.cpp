// s12-pgserver: TCPServer (POSIX, 127.0.0.1, ephemeral port).
// Nutzt pgwire-Codec fuer Framing. Siehe include/dbengine/server/pgserver.h.
// TLS opt-in: SSLRequest -> 'N' (Default) bzw. 'S' + SSL_accept (mit Zert).

#include "dbengine/server/pgserver.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "dbengine/server/pgwire.h"
#include "dbengine/sql/parser.h"

#ifdef DBENGINE_WITH_TLS
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#endif

namespace dbengine::pgserver {
namespace {

using dbengine::pgwire::getInt32BE;
using dbengine::pgwire::putInt32BE;

// Connection-Abstraktion: Klartext (fd) oder TLS (SSL* auf gleichem fd).
// Alles OpenSSL hinter DBENGINE_WITH_TLS; ohne Define enthaelt Conn nur fd.
struct Conn {
  int fd = -1;
  bool useTls = false;
#ifdef DBENGINE_WITH_TLS
  SSL* ssl = nullptr;
#endif
};

bool sendRaw(int fd, const uint8_t* data, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

#ifdef DBENGINE_WITH_TLS
bool sendSsl(SSL* ssl, const uint8_t* data, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    int n = SSL_write(ssl, data + off,
                      static_cast<int>(len - off));
    if (n <= 0) {
      int e = SSL_get_error(ssl, n);
      if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

bool recvSsl(SSL* ssl, uint8_t* out, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    int n = SSL_read(ssl, out + off, static_cast<int>(len - off));
    if (n <= 0) {
      int e = SSL_get_error(ssl, n);
      if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}
#endif

bool connSend(Conn& c, const uint8_t* data, std::size_t len) {
#ifdef DBENGINE_WITH_TLS
  if (c.useTls && c.ssl != nullptr) return sendSsl(c.ssl, data, len);
#endif
  return sendRaw(c.fd, data, len);
}

bool connSend(Conn& c, const std::vector<uint8_t>& v) {
  if (v.empty()) return true;
  return connSend(c, v.data(), v.size());
}

bool recvRaw(int fd, uint8_t* out, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    ssize_t n = ::recv(fd, out + off, len - off, 0);
    if (n == 0) return false;  // orderly shutdown
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

bool connRecv(Conn& c, uint8_t* out, std::size_t len) {
#ifdef DBENGINE_WITH_TLS
  if (c.useTls && c.ssl != nullptr) return recvSsl(c.ssl, out, len);
#endif
  return recvRaw(c.fd, out, len);
}

// Startup: Int32 len | Rest(len-4). Gibt volles Paket (inkl. len) zurueck.
bool readStartupPacket(Conn& c, std::vector<uint8_t>& out) {
  uint8_t hdr[4];
  if (!connRecv(c, hdr, 4)) return false;
  int32_t len = getInt32BE(hdr);
  if (len < 8 || len > 1024 * 1024) return false;
  out.resize(static_cast<std::size_t>(len));
  std::memcpy(out.data(), hdr, 4);
  if (!connRecv(c, out.data() + 4, static_cast<std::size_t>(len) - 4))
    return false;
  return true;
}

// Normale Nachricht: 'T' | Int32 len | Payload(len-4).
// Gibt Typ + volles Paket (Typ + len + payload) zurueck.
bool readTypedMessage(Conn& c, char& type, std::vector<uint8_t>& out) {
  uint8_t hdr[5];
  if (!connRecv(c, hdr, 5)) return false;
  type = static_cast<char>(hdr[0]);
  int32_t len = getInt32BE(hdr + 1);
  if (len < 4 || len > 16 * 1024 * 1024) return false;
  out.resize(1 + static_cast<std::size_t>(len));
  std::memcpy(out.data(), hdr, 5);
  std::size_t rest = static_cast<std::size_t>(len) - 4;
  if (rest > 0 && !connRecv(c, out.data() + 5, rest)) return false;
  return true;
}

// SSLRequest: exakt len 8 + Code 80877103 (kein Typ-Byte).
bool isSslRequestPacket(const std::vector<uint8_t>& pkt) {
  if (pkt.size() != 8) return false;
  return getInt32BE(pkt.data() + 4) == dbengine::pgwire::kSslRequestCode;
}

#ifdef DBENGINE_WITH_TLS
// Server-Handshake auf gleichem fd (ctx pro Connection, danach freigegeben).
bool tlsHandshake(Conn& c, const std::string& certPath,
                  const std::string& keyPath) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  if (ctx == nullptr) return false;
  bool ok = false;
  if (SSL_CTX_use_certificate_file(ctx, certPath.c_str(), SSL_FILETYPE_PEM) ==
          1 &&
      SSL_CTX_use_PrivateKey_file(ctx, keyPath.c_str(), SSL_FILETYPE_PEM) ==
          1) {
    SSL* ssl = SSL_new(ctx);
    if (ssl != nullptr) {
      if (SSL_set_fd(ssl, c.fd) == 1 && SSL_accept(ssl) == 1) {
        c.ssl = ssl;
        c.useTls = true;
        ok = true;
      } else {
        SSL_free(ssl);
      }
    }
  }
  SSL_CTX_free(ctx);
  return ok;
}
#endif

std::vector<uint8_t> encodeAuthOk() {
  std::vector<uint8_t> out;
  out.push_back('R');
  putInt32BE(out, 8);
  putInt32BE(out, 0);
  return out;
}

// AuthenticationCleartextPassword: 'R' | len=8 | int32(3). Kein Payload.
// SICHERHEIT: Klartext ohne TLS mitlesbar — nur mit Sidecar-TLS nutzen.
std::vector<uint8_t> encodeAuthCleartext() {
  std::vector<uint8_t> out;
  out.push_back('R');
  putInt32BE(out, 8);
  putInt32BE(out, 3);
  return out;
}

// PasswordMessage ('p' | len | password\0) -> Passwort. nullopt bei Fehlformat
// (falscher Typ wird vom Caller geprüft; hier nur Payload-Form: genau ein
// NUL am Ende, keine eingebetteten NULs/Trailer).
std::optional<std::string> parsePasswordMessage(
    const std::vector<uint8_t>& msg) {
  if (msg.size() < 6) return std::nullopt;  // 'p' + len(4) + mind. NUL
  if (msg[0] != 'p') return std::nullopt;
  if (msg.back() != 0) return std::nullopt;
  const char* begin = reinterpret_cast<const char*>(msg.data() + 5);
  std::size_t payLen = msg.size() - 5;
  std::size_t n = 0;
  while (n < payLen && begin[n] != '\0') ++n;
  if (n + 1 != payLen) return std::nullopt;
  return std::string(begin, n);
}

// ---- SCRAM-SHA-256 (RFC 5802 Server-Seite) --------------------------------
// Base64 (STL-only, kein Krypto) ist immer verfuegbar; SHA256/HMAC nur mit
// DBENGINE_WITH_TLS via OpenSSL (EVP/HMAC). Ohne das Define wird SCRAM zur
// Laufzeit verweigert (keine eigene Crypto-Implementierung).

[[maybe_unused]] constexpr const char* kScramMech = "SCRAM-SHA-256";

bool isScramEntry(const std::string& v) {
  return v.size() >= 6 && v.compare(0, 6, "scram:") == 0;
}

[[maybe_unused]] std::string scramB64Encode(const uint8_t* data,
                                            std::size_t len) {
  static const char* kTab =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (std::size_t i = 0; i < len; i += 3) {
    uint32_t b0 = data[i];
    uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
    uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
    uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kTab[(triple >> 18) & 63]);
    out.push_back(kTab[(triple >> 12) & 63]);
    out.push_back(i + 1 < len ? kTab[(triple >> 6) & 63] : '=');
    out.push_back(i + 2 < len ? kTab[triple & 63] : '=');
  }
  return out;
}

[[maybe_unused]] std::optional<std::vector<uint8_t>> scramB64Decode(
    const std::string& s) {
  if (s.empty() || s.size() % 4 != 0) return std::nullopt;
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;  // padding
    return -1;
  };
  std::size_t pad = 0;
  if (s.back() == '=') ++pad;
  if (s.size() >= 2 && s[s.size() - 2] == '=') ++pad;
  if (pad > 2) return std::nullopt;
  for (std::size_t i = 0; i < s.size() - pad; ++i) {
    if (val(s[i]) < 0) return std::nullopt;
  }
  for (std::size_t i = s.size() - pad; i < s.size(); ++i) {
    if (s[i] != '=') return std::nullopt;
  }
  std::vector<uint8_t> out;
  out.reserve((s.size() / 4) * 3);
  for (std::size_t i = 0; i < s.size(); i += 4) {
    int v0 = val(s[i]);
    int v1 = val(s[i + 1]);
    int v2 = (s[i + 2] == '=') ? 0 : val(s[i + 2]);
    int v3 = (s[i + 3] == '=') ? 0 : val(s[i + 3]);
    if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return std::nullopt;
    uint32_t triple = (static_cast<uint32_t>(v0) << 18) |
                      (static_cast<uint32_t>(v1) << 12) |
                      (static_cast<uint32_t>(v2) << 6) |
                      static_cast<uint32_t>(v3);
    out.push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
    if (s[i + 2] != '=') out.push_back(static_cast<uint8_t>((triple >> 8) & 0xFF));
    if (s[i + 3] != '=') out.push_back(static_cast<uint8_t>(triple & 0xFF));
  }
  return out;
}

struct ScramVerifier {
  std::string saltB64;  // Original-String fuer Server-First (s=...)
  std::vector<uint8_t> salt;
  int iter = 4096;
  std::vector<uint8_t> storedKey;  // 32 Byte (SHA256(ClientKey))
  std::vector<uint8_t> serverKey;  // 32 Byte, optional
  bool hasServerKey = false;
};

// Format: "scram:<salt-b64>:<iter>:<storedkey-b64>[:<serverkey-b64>]".
// Rueckgabe nullopt bei Fehlformat (Caller -> 28P01, kein Leak).
[[maybe_unused]] std::optional<ScramVerifier> parseScramEntry(
    const std::string& v) {
  if (!isScramEntry(v)) return std::nullopt;
  std::vector<std::string> parts;
  std::string cur;
  for (char c : v) {
    if (c == ':') {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  parts.push_back(cur);
  if (parts.size() != 4 && parts.size() != 5) return std::nullopt;
  if (parts[0] != "scram") return std::nullopt;
  if (parts[1].empty() || parts[2].empty() || parts[3].empty()) return std::nullopt;
  long iter = 0;
  try {
    std::size_t pos = 0;
    iter = std::stol(parts[2], &pos, 10);
    if (pos != parts[2].size()) return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
  if (iter <= 0 || iter > 10000000) return std::nullopt;
  auto salt = scramB64Decode(parts[1]);
  auto stored = scramB64Decode(parts[3]);
  if (!salt.has_value() || salt->empty()) return std::nullopt;
  if (!stored.has_value() || stored->size() != 32) return std::nullopt;
  ScramVerifier sv;
  sv.saltB64 = parts[1];
  sv.salt = std::move(*salt);
  sv.iter = static_cast<int>(iter);
  sv.storedKey = std::move(*stored);
  if (parts.size() == 5) {
    if (parts[4].empty()) return std::nullopt;
    auto sk = scramB64Decode(parts[4]);
    if (!sk.has_value() || sk->size() != 32) return std::nullopt;
    sv.serverKey = std::move(*sk);
    sv.hasServerKey = true;
  }
  return sv;
}

// R(10 AuthenticationSASL): 'R' | len | int32(10) | "SCRAM-SHA-256\0" | "\0".
// (Mechanismus-String + Pflicht-Terminator nach letzter Mechanism-Angabe.)
[[maybe_unused]] std::vector<uint8_t> encodeAuthSasl() {
  std::vector<uint8_t> out;
  out.push_back('R');
  const std::string mech = kScramMech;
  int32_t len = 4 + 4 + static_cast<int32_t>(mech.size()) + 1 + 1;
  putInt32BE(out, len);
  putInt32BE(out, 10);
  out.insert(out.end(), mech.begin(), mech.end());
  out.push_back(0);
  out.push_back(0);
  return out;
}

// R(11 SASLContinue) / R(12 SASLFinal): 'R' | len | int32(kind) | raw bytes.
[[maybe_unused]] std::vector<uint8_t> encodeAuthSaslCont(
    int32_t kind, const std::string& data) {
  std::vector<uint8_t> out;
  out.push_back('R');
  putInt32BE(out, 4 + 4 + static_cast<int32_t>(data.size()));
  putInt32BE(out, kind);
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

struct SaslInitial {
  std::string mech;
  std::string initial;  // Client-First-Message (kann leer sein -> Fail)
  bool hasInitial = false;
};

// SASLInitialResponse: 'p' | len | mech\0 | int32(n) | n bytes (n=-1: keine).
[[maybe_unused]] std::optional<SaslInitial> parseSaslInitial(
    const std::vector<uint8_t>& msg) {
  if (msg.empty() || msg[0] != 'p' || msg.size() < 5) return std::nullopt;
  std::size_t pos = 5;
  std::size_t start = pos;
  while (pos < msg.size() && msg[pos] != 0) ++pos;
  if (pos >= msg.size()) return std::nullopt;
  SaslInitial out;
  out.mech.assign(reinterpret_cast<const char*>(msg.data() + start), pos - start);
  ++pos;  // NUL
  if (pos + 4 > msg.size()) return std::nullopt;
  int32_t n = getInt32BE(msg.data() + pos);
  pos += 4;
  if (n == -1) {
    if (pos != msg.size()) return std::nullopt;
    out.hasInitial = false;
    return out;
  }
  if (n < 0 || pos + static_cast<std::size_t>(n) != msg.size()) return std::nullopt;
  out.initial.assign(reinterpret_cast<const char*>(msg.data() + pos),
                     static_cast<std::size_t>(n));
  out.hasInitial = true;
  return out;
}

// SASLResponse: 'p' | len | raw bytes (0..n).
[[maybe_unused]] std::optional<std::string> parseSaslResponse(
    const std::vector<uint8_t>& msg) {
  if (msg.empty() || msg[0] != 'p' || msg.size() < 5) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(msg.data() + 5), msg.size() - 5);
}

// Client-First "n,,n=user,r=nonce" (gs2 "n,,"/"y,,"; PLUS "p=..,, " abgelehnt).
// Gibt (bare, user, clientNonce, gs2Header) zurueck.
struct ClientFirst {
  std::string bare;
  std::string user;
  std::string nonce;
  std::string gs2;
};

[[maybe_unused]] std::optional<ClientFirst> parseClientFirst(
    const std::string& cf) {
  if (cf.size() < 4) return std::nullopt;
  std::string gs2;
  if (cf.compare(0, 3, "n,,") == 0 || cf.compare(0, 3, "y,,") == 0) {
    gs2 = cf.substr(0, 3);
  } else {
    return std::nullopt;  // inkl. "p=...,," (PLUS nicht angeboten)
  }
  std::string bare = cf.substr(3);
  if (bare.empty()) return std::nullopt;
  // Attribute splitten, "m=" (reserved ext) tolerieren, n=/r= Pflicht.
  std::string user;
  std::string nonce;
  bool hasUser = false;
  bool hasNonce = false;
  std::size_t i = 0;
  while (i <= bare.size()) {
    std::size_t j = bare.find(',', i);
    std::string attr = (j == std::string::npos) ? bare.substr(i)
                                                : bare.substr(i, j - i);
    if (attr.size() >= 2 && attr[1] == '=') {
      char k = attr[0];
      std::string val = attr.substr(2);
      if (k == 'n' && !hasUser) {
        if (val.empty()) return std::nullopt;
        user = val;
        hasUser = true;
      } else if (k == 'r' && !hasNonce) {
        if (val.empty()) return std::nullopt;
        nonce = val;
        hasNonce = true;
      }
    }
    if (j == std::string::npos) break;
    i = j + 1;
  }
  if (!hasUser || !hasNonce) return std::nullopt;
  // Nonce darf kein ',' enthalten (waere sonst Attr-Bruch); hier implizit ok.
  if (nonce.find(',') != std::string::npos) return std::nullopt;
  ClientFirst out;
  out.bare = bare;
  out.user = user;
  out.nonce = nonce;
  out.gs2 = gs2;
  return out;
}

struct ClientFinal {
  std::string cbind;     // c=... (b64)
  std::string nonce;     // r=... (muss combined sein)
  std::string proofB64;  // p=... (b64)
  std::string withoutProof;  // "c=...,r=..." fuer AuthMessage
};

[[maybe_unused]] std::optional<ClientFinal> parseClientFinal(
    const std::string& cf) {
  std::size_t ppos = cf.rfind(",p=");
  if (ppos == std::string::npos) return std::nullopt;
  std::string without = cf.substr(0, ppos);
  std::string proof = cf.substr(ppos + 3);
  if (proof.empty()) return std::nullopt;
  // without muss "c=...,r=..." enthalten (Reihenfolge c vor r per RFC).
  std::string cbind;
  std::string nonce;
  std::size_t i = 0;
  while (i <= without.size()) {
    std::size_t j = without.find(',', i);
    std::string attr = (j == std::string::npos) ? without.substr(i)
                                                : without.substr(i, j - i);
    if (attr.size() >= 2 && attr[1] == '=') {
      if (attr[0] == 'c' && cbind.empty()) cbind = attr.substr(2);
      if (attr[0] == 'r' && nonce.empty()) nonce = attr.substr(2);
    }
    if (j == std::string::npos) break;
    i = j + 1;
  }
  if (cbind.empty() || nonce.empty()) return std::nullopt;
  ClientFinal out;
  out.cbind = cbind;
  out.nonce = nonce;
  out.proofB64 = proof;
  out.withoutProof = without;
  return out;
}

#ifdef DBENGINE_WITH_TLS
bool scramSha256(const uint8_t* data, std::size_t len, uint8_t out32[32]) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) return false;
  bool ok = false;
  unsigned int olen = 0;
  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
      EVP_DigestUpdate(ctx, data, len) == 1 &&
      EVP_DigestFinal_ex(ctx, out32, &olen) == 1 && olen == 32) {
    ok = true;
  }
  EVP_MD_CTX_free(ctx);
  return ok;
}

bool scramHmac(const uint8_t* key, std::size_t keyLen, const uint8_t* data,
               std::size_t dataLen, uint8_t out32[32]) {
  unsigned int olen = 0;
  uint8_t tmp[32];
  if (::HMAC(EVP_sha256(), key, static_cast<int>(keyLen), data, dataLen, tmp,
             &olen) == nullptr ||
      olen != 32) {
    return false;
  }
  std::memcpy(out32, tmp, 32);
  OPENSSL_cleanse(tmp, sizeof(tmp));
  return true;
}

bool scramHmacStr(const std::vector<uint8_t>& key, const std::string& data,
                  uint8_t out32[32]) {
  if (key.empty()) return false;
  return scramHmac(key.data(), key.size(),
                   reinterpret_cast<const uint8_t*>(data.data()), data.size(),
                   out32);
}
#endif

[[maybe_unused]] bool scramConstEq(const std::vector<uint8_t>& a,
                                    const std::vector<uint8_t>& b) {
  if (a.size() != b.size()) return false;
  uint8_t d = 0;
  for (std::size_t i = 0; i < a.size(); ++i) d |= (a[i] ^ b[i]);
  return d == 0;
}

[[maybe_unused]] std::string scramServerNonce() {
  uint8_t buf[18] = {0};
#ifdef DBENGINE_WITH_TLS
  if (RAND_bytes(buf, sizeof(buf)) != 1) {
    std::random_device rd;
    for (auto& b : buf) b = static_cast<uint8_t>(rd());
  }
#else
  std::random_device rd;
  for (auto& b : buf) b = static_cast<uint8_t>(rd());
#endif
  return scramB64Encode(buf, sizeof(buf));
}

std::string trimUpper(const std::string& s) {
  std::size_t a = 0;
  while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
  std::size_t b = s.size();
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  std::string t = s.substr(a, b - a);
  while (!t.empty() && t.back() == ';') t.pop_back();
  while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
  std::string u;
  u.reserve(t.size());
  for (char c : t) u += static_cast<char>(std::toupper((unsigned char)c));
  return u;
}

bool isSelectOne(const std::string& q) {
  std::string u = trimUpper(q);
  return u == "SELECT 1";
}

void putCString(std::vector<uint8_t>& out, const std::string& s) {
  out.insert(out.end(), s.begin(), s.end());
  out.push_back(0);
}

std::string toLowerStr(std::string s) {
  for (auto& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

int32_t colTypeOid(dbengine::sql::ColType t) {
  using dbengine::sql::ColType;
  switch (t) {
    case ColType::Int: return 23;     // INT4
    case ColType::Double: return 701;  // FLOAT8
    case ColType::Bool: return 16;     // BOOL
    case ColType::Text:  // TEXT
    default: return 25;
  }
}

// Typed RowDescription (Layout byte-identisch zu pgwire::encodeRowDescription
// fuer OID 25; erlaubt aber 23/701/16 pro Spalte, Format 0 fuer alle).
std::vector<uint8_t> encodeRowDescriptionTyped(
    const std::vector<std::string>& columns, const std::vector<int32_t>& oids) {
  std::vector<uint8_t> body;
  putInt32BE(body, static_cast<int32_t>(columns.size()));
  for (std::size_t i = 0; i < columns.size(); ++i) {
    putCString(body, columns[i]);
    putInt32BE(body, 0);  // table OID
    body.push_back(0);
    body.push_back(0);  // attrno
    putInt32BE(body, i < oids.size() ? oids[i] : 25);
    body.push_back(0xFF);
    body.push_back(0xFF);  // typlen -1
    putInt32BE(body, -1);  // typmod
    body.push_back(0);
    body.push_back(0);  // format 0 (text)
  }
  std::vector<uint8_t> out;
  out.push_back('T');
  putInt32BE(out, static_cast<int32_t>(body.size() + 4));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// DataRow aus Values (Text-Encoding via valueToString, NULL als -1).
std::vector<uint8_t> encodeDataRowValues(
    const std::vector<dbengine::sql::Value>& row) {
  std::vector<uint8_t> body;
  body.push_back(static_cast<uint8_t>((row.size() >> 8) & 0xFF));
  body.push_back(static_cast<uint8_t>(row.size() & 0xFF));
  for (const auto& v : row) {
    if (dbengine::sql::valueIsNull(v)) {
      putInt32BE(body, -1);
    } else {
      std::string s = dbengine::sql::valueToString(v);
      putInt32BE(body, static_cast<int32_t>(s.size()));
      body.insert(body.end(), s.begin(), s.end());
    }
  }
  std::vector<uint8_t> out;
  out.push_back('D');
  putInt32BE(out, static_cast<int32_t>(body.size() + 4));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// OID-Aufloesung: SELECT -> Schema-Typen via Executor::schemaOf (COUNT(*) = 23),
// Fallback: Werttyp der ersten Zeile, sonst 25. Kein Throw.
std::vector<int32_t> resolveOids(const dbengine::sql::Result& r,
                                 const std::string& q,
                                 dbengine::sql::Executor& ex) {
  std::vector<int32_t> oids(r.columns.size(), 25);
  bool viaSchema = false;
  try {
    dbengine::sql::Statement st = dbengine::sql::parseStatement(q);
    if (const auto* sel = std::get_if<dbengine::sql::SelectStmt>(&st)) {
      if (sel->count_star) {
        if (!oids.empty()) oids[0] = 23;
        return oids;
      }
      const dbengine::sql::TableSchema* sch = ex.schemaOf(sel->table);
      if (sch != nullptr) {
        for (std::size_t i = 0; i < r.columns.size(); ++i) {
          std::string want = toLowerStr(r.columns[i]);
          for (const auto& cd : sch->columns) {
            if (toLowerStr(cd.name) == want) {
              oids[i] = colTypeOid(cd.type);
              break;
            }
          }
        }
        viaSchema = true;
      }
    }
  } catch (...) {
    // Fallback unten
  }
  if (viaSchema) return oids;
  if (!r.rows.empty()) {
    const auto& first = r.rows[0];
    std::size_t n = std::min(oids.size(), first.size());
    for (std::size_t i = 0; i < n; ++i) {
      const auto& v = first[i];
      if (std::holds_alternative<int64_t>(v))
        oids[i] = 23;
      else if (std::holds_alternative<double>(v))
        oids[i] = 701;
      else if (std::holds_alternative<bool>(v))
        oids[i] = 16;
      else
        oids[i] = 25;
    }
  }
  return oids;
}

// ---- Extended-Protokoll minimal (parameterlos) ---------------------------
// Pro-Connection State: preparedStmts[name]=query, portals[name]=query.
// Parse -> '1', Bind -> '2', Describe -> T/n (ohne Execute), Execute ->
// T/D/C wie Q-Pfad, Sync -> Z, Close -> '3'. Mit Parametern -> E 0A000.

std::vector<uint8_t> encodeParseComplete() { return {'1', 0, 0, 0, 4}; }

std::vector<uint8_t> encodeBindComplete() { return {'2', 0, 0, 0, 4}; }

std::vector<uint8_t> encodeCloseComplete() { return {'3', 0, 0, 0, 4}; }

std::vector<uint8_t> encodeNoData() { return {'n', 0, 0, 0, 4}; }

uint16_t getUint16BE(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                               static_cast<uint16_t>(p[1]));
}

std::string readCStringExt(const std::vector<uint8_t>& msg, std::size_t& pos) {
  if (pos >= msg.size())
    throw dbengine::pgwire::ProtoError("Extended: Position ausserhalb");
  std::size_t start = pos;
  while (pos < msg.size() && msg[pos] != 0) ++pos;
  if (pos >= msg.size())
    throw dbengine::pgwire::ProtoError("Extended: unterminierter String");
  std::string s(reinterpret_cast<const char*>(msg.data() + start),
                pos - start);
  ++pos;  // NUL
  return s;
}

int32_t getI32At(const std::vector<uint8_t>& msg, std::size_t pos) {
  if (pos + 4 > msg.size())
    throw dbengine::pgwire::ProtoError("Extended: Paket zu kurz (i32)");
  return getInt32BE(msg.data() + pos);
}

uint16_t getU16At(const std::vector<uint8_t>& msg, std::size_t pos) {
  if (pos + 2 > msg.size())
    throw dbengine::pgwire::ProtoError("Extended: Paket zu kurz (i16)");
  return getUint16BE(msg.data() + pos);
}

// '$' + Ziffer ausserhalb von '...'-Literalen ('' = escape) -> Parameter.
bool containsDollarParam(const std::string& q) {
  bool inStr = false;
  for (std::size_t i = 0; i < q.size(); ++i) {
    char c = q[i];
    if (inStr) {
      if (c == '\'') {
        if (i + 1 < q.size() && q[i + 1] == '\'')
          ++i;
        else
          inStr = false;
      }
    } else {
      if (c == '\'')
        inStr = true;
      else if (c == '$' && i + 1 < q.size() &&
               std::isdigit(static_cast<unsigned char>(q[i + 1])))
        return true;
    }
  }
  return false;
}

struct ParseMsg {
  std::string stmt;
  std::string query;
  uint16_t numParams = 0;
};

ParseMsg parseParseMsg(const std::vector<uint8_t>& msg) {
  if (msg.empty() || msg[0] != 'P')
    throw dbengine::pgwire::ProtoError("Parse: falscher Typ");
  std::size_t pos = 5;
  ParseMsg out;
  out.stmt = readCStringExt(msg, pos);
  out.query = readCStringExt(msg, pos);
  out.numParams = getU16At(msg, pos);
  pos += 2;
  if (pos + static_cast<std::size_t>(out.numParams) * 4 != msg.size())
    throw dbengine::pgwire::ProtoError("Parse: Laenge passt nicht");
  return out;
}

struct BindMsg {
  std::string portal;
  std::string stmt;
  uint16_t numParams = 0;
};

BindMsg parseBindMsg(const std::vector<uint8_t>& msg) {
  if (msg.empty() || msg[0] != 'B')
    throw dbengine::pgwire::ProtoError("Bind: falscher Typ");
  std::size_t pos = 5;
  BindMsg out;
  out.portal = readCStringExt(msg, pos);
  out.stmt = readCStringExt(msg, pos);
  uint16_t nFmt = getU16At(msg, pos);
  pos += 2;
  if (pos + static_cast<std::size_t>(nFmt) * 2 > msg.size())
    throw dbengine::pgwire::ProtoError("Bind: Format-Codes zu kurz");
  pos += static_cast<std::size_t>(nFmt) * 2;
  out.numParams = getU16At(msg, pos);
  pos += 2;
  // Parameter-Werte ueberspringen (fuer Validierung; Inhalt egal, >0 -> 0A000).
  for (uint16_t i = 0; i < out.numParams; ++i) {
    int32_t plen = getI32At(msg, pos);
    pos += 4;
    if (plen == -1) continue;
    if (plen < -1)
      throw dbengine::pgwire::ProtoError("Bind: negative Param-Laenge");
    if (pos + static_cast<std::size_t>(plen) > msg.size())
      throw dbengine::pgwire::ProtoError("Bind: Param-Wert zu kurz");
    pos += static_cast<std::size_t>(plen);
  }
  uint16_t nRes = getU16At(msg, pos);
  pos += 2;
  if (pos + static_cast<std::size_t>(nRes) * 2 != msg.size())
    throw dbengine::pgwire::ProtoError("Bind: Laenge passt nicht");
  return out;
}

struct DescribeMsg {
  char kind = 0;  // 'S' oder 'P'
  std::string name;
};

DescribeMsg parseDescribeMsg(const std::vector<uint8_t>& msg) {
  if (msg.empty() || msg[0] != 'D')
    throw dbengine::pgwire::ProtoError("Describe: falscher Typ");
  if (msg.size() < 7)
    throw dbengine::pgwire::ProtoError("Describe: Paket zu kurz");
  DescribeMsg out;
  out.kind = static_cast<char>(msg[5]);
  std::size_t pos = 6;
  out.name = readCStringExt(msg, pos);
  if (pos != msg.size())
    throw dbengine::pgwire::ProtoError("Describe: Laenge passt nicht");
  return out;
}

struct ExecuteMsg {
  std::string portal;
};

ExecuteMsg parseExecuteMsg(const std::vector<uint8_t>& msg) {
  if (msg.empty() || msg[0] != 'E')
    throw dbengine::pgwire::ProtoError("Execute: falscher Typ");
  std::size_t pos = 5;
  ExecuteMsg out;
  out.portal = readCStringExt(msg, pos);
  if (pos + 4 != msg.size())
    throw dbengine::pgwire::ProtoError("Execute: Laenge passt nicht");
  return out;
}

struct CloseMsg {
  char kind = 0;
  std::string name;
};

CloseMsg parseCloseMsg(const std::vector<uint8_t>& msg) {
  if (msg.empty() || msg[0] != 'C')
    throw dbengine::pgwire::ProtoError("Close: falscher Typ");
  if (msg.size() < 7)
    throw dbengine::pgwire::ProtoError("Close: Paket zu kurz");
  CloseMsg out;
  out.kind = static_cast<char>(msg[5]);
  std::size_t pos = 6;
  out.name = readCStringExt(msg, pos);
  if (pos != msg.size())
    throw dbengine::pgwire::ProtoError("Close: Laenge passt nicht");
  return out;
}

// Spaltentyp-Lookup single-table: ggf. "t.c" -> "c" (Prefix tolerant).
int singleColIndex(const dbengine::sql::TableSchema& sch,
                   const std::string& ref) {
  std::string col = ref;
  std::size_t dot = ref.find('.');
  if (dot != std::string::npos) col = ref.substr(dot + 1);
  std::string want = toLowerStr(col);
  for (std::size_t i = 0; i < sch.columns.size(); ++i) {
    if (toLowerStr(sch.columns[i].name) == want) return static_cast<int>(i);
  }
  return -1;
}

// Join-Lookup: gibt (Schema, Index). Wirft SqlError bei unknown/ambiguous.
void joinColLookup(const dbengine::sql::TableSchema& lsch,
                   const dbengine::sql::TableSchema& rsch,
                   const std::string& lEff, const std::string& rEff,
                   const std::string& lTable, const std::string& rTable,
                   const std::string& ref, const dbengine::sql::TableSchema*& oSch,
                   int& oIdx) {
  std::string pre, col;
  std::size_t dot = ref.find('.');
  if (dot == std::string::npos) {
    pre.clear();
    col = ref;
  } else {
    pre = ref.substr(0, dot);
    col = ref.substr(dot + 1);
  }
  auto colIdx = [&](const dbengine::sql::TableSchema& sch,
                    const std::string& c) -> int {
    std::string want = toLowerStr(c);
    for (std::size_t i = 0; i < sch.columns.size(); ++i) {
      if (toLowerStr(sch.columns[i].name) == want) return static_cast<int>(i);
    }
    return -1;
  };
  if (!pre.empty()) {
    bool lm = (toLowerStr(pre) == toLowerStr(lEff)) ||
              (toLowerStr(pre) == toLowerStr(lTable));
    bool rm = (toLowerStr(pre) == toLowerStr(rEff)) ||
              (toLowerStr(pre) == toLowerStr(rTable));
    if (lm && rm)
      throw dbengine::sql::SqlError("Mehrdeutiger Tabellen-Prefix: " + pre);
    if (lm) {
      int idx = colIdx(lsch, col);
      if (idx < 0)
        throw dbengine::sql::SqlError("Unbekannte Spalte: " + ref);
      oSch = &lsch;
      oIdx = idx;
      return;
    }
    if (rm) {
      int idx = colIdx(rsch, col);
      if (idx < 0)
        throw dbengine::sql::SqlError("Unbekannte Spalte: " + ref);
      oSch = &rsch;
      oIdx = idx;
      return;
    }
    throw dbengine::sql::SqlError("Unbekannter Tabellen-Prefix: " + pre);
  }
  int li = colIdx(lsch, col);
  int ri = colIdx(rsch, col);
  if (li >= 0 && ri >= 0)
    throw dbengine::sql::SqlError("Mehrdeutige Spalte: " + col);
  if (li >= 0) {
    oSch = &lsch;
    oIdx = li;
    return;
  }
  if (ri >= 0) {
    oSch = &rsch;
    oIdx = ri;
    return;
  }
  throw dbengine::sql::SqlError("Unbekannte Spalte: " + ref);
}

// Aggregat-OID: COUNT->23, AVG->701, sonst Spaltentyp oder Literaltyp, sonst 25.
int32_t aggOid(const dbengine::sql::Aggregate& a,
               const dbengine::sql::TableSchema* lsch,
               const dbengine::sql::TableSchema* rsch, const std::string& lEff,
               const std::string& rEff, const std::string& lTable,
               const std::string& rTable, bool isJoin) {
  std::string func;
  func.reserve(a.func.size());
  for (char c : a.func)
    func += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (a.star || func == "COUNT") return 23;
  if (func == "AVG") return 701;
  if (a.arg != nullptr &&
      a.arg->kind == dbengine::sql::AggExpr::Kind::Column) {
    const std::string& ref = a.arg->column;
    try {
      if (isJoin && lsch != nullptr && rsch != nullptr) {
        const dbengine::sql::TableSchema* oSch = nullptr;
        int oIdx = -1;
        joinColLookup(*lsch, *rsch, lEff, rEff, lTable, rTable, ref, oSch,
                      oIdx);
        return colTypeOid(oSch->columns[static_cast<std::size_t>(oIdx)].type);
      }
      if (lsch != nullptr) {
        int idx = singleColIndex(*lsch, ref);
        if (idx >= 0)
          return colTypeOid(
              lsch->columns[static_cast<std::size_t>(idx)].type);
      }
    } catch (...) {
    }
    return 25;
  }
  if (a.arg != nullptr &&
      a.arg->kind == dbengine::sql::AggExpr::Kind::Literal) {
    const auto& v = a.arg->literal;
    if (std::holds_alternative<int64_t>(v)) return 23;
    if (std::holds_alternative<double>(v)) return 701;
    if (std::holds_alternative<bool>(v)) return 16;
    return 25;
  }
  return 25;
}

// Projektions-Analyse OHNE Execute. Rueckgabe true = Spalten vorhanden
// (cols/oids gefuellt), false = NoData (DDL / INSERT ohne Projektion).
// Wirft SqlError bei unbekannter Tabelle/Spalte. Caller haelt execMu_.
bool describeProjection(const std::string& q, dbengine::sql::Executor& ex,
                        std::vector<std::string>& cols,
                        std::vector<int32_t>& oids) {
  cols.clear();
  oids.clear();
  if (isSelectOne(q)) {
    cols = {"?column?"};
    oids = {25};
    return true;
  }
  dbengine::sql::Statement st = dbengine::sql::parseStatement(q);
  if (!std::holds_alternative<dbengine::sql::SelectStmt>(st)) return false;
  const auto& sel = std::get<dbengine::sql::SelectStmt>(st);
  if (sel.count_star) {
    cols = {"count"};
    oids = {23};
    return true;
  }
  const dbengine::sql::TableSchema* lsch = nullptr;
  const dbengine::sql::TableSchema* rsch = nullptr;
  std::string lEff, rEff;
  if (sel.has_join) {
    lsch = ex.schemaOf(sel.table);
    const std::string lnorm = dbengine::sql::Executor::normalizeTable(sel.table);
    const std::string rnorm =
        dbengine::sql::Executor::normalizeTable(sel.join_table);
    if (lnorm == rnorm) {
      rsch = lsch;
    } else {
      rsch = ex.schemaOf(sel.join_table);
    }
    if (lsch == nullptr)
      throw dbengine::sql::SqlError("Tabelle unbekannt: " + sel.table);
    if (rsch == nullptr)
      throw dbengine::sql::SqlError("Tabelle unbekannt: " + sel.join_table);
    lEff = sel.table_alias.empty() ? sel.table : sel.table_alias;
    rEff = sel.join_alias.empty() ? sel.join_table : sel.join_alias;
  } else {
    lsch = ex.schemaOf(sel.table);
    if (lsch == nullptr)
      throw dbengine::sql::SqlError("Tabelle unbekannt: " + sel.table);
  }
  if (sel.select_all) {
    if (sel.has_join) {
      for (const auto& c : lsch->columns) {
        cols.push_back(c.name);
        oids.push_back(colTypeOid(c.type));
      }
      for (const auto& c : rsch->columns) {
        cols.push_back(c.name);
        oids.push_back(colTypeOid(c.type));
      }
    } else {
      for (const auto& c : lsch->columns) {
        cols.push_back(c.name);
        oids.push_back(colTypeOid(c.type));
      }
    }
    return true;
  }
  // Explizite Projektion in SELECT-Reihenfolge (items bevorzugt).
  std::vector<dbengine::sql::SelectItem> items = sel.items;
  if (items.empty()) {
    for (std::size_t i = 0; i < sel.columns.size(); ++i)
      items.push_back(dbengine::sql::SelectItem{false, i});
    for (std::size_t i = 0; i < sel.aggregates.size(); ++i)
      items.push_back(dbengine::sql::SelectItem{true, i});
  }
  cols.reserve(items.size());
  oids.reserve(items.size());
  for (const auto& it : items) {
    if (it.is_agg) {
      if (it.index >= sel.aggregates.size())
        throw dbengine::sql::SqlError("Ungueltige Projektion (Aggregat)");
      const auto& a = sel.aggregates[it.index];
      cols.push_back(a.alias.empty() ? a.display : a.alias);
      oids.push_back(
          aggOid(a, lsch, rsch, lEff, rEff, sel.table, sel.join_table,
                 sel.has_join));
    } else {
      if (it.index >= sel.columns.size())
        throw dbengine::sql::SqlError("Ungueltige Projektion (Spalte)");
      const std::string& ref = sel.columns[it.index];
      std::string alias;
      if (it.index < sel.column_aliases.size()) alias = sel.column_aliases[it.index];
      if (sel.has_join) {
        const dbengine::sql::TableSchema* oSch = nullptr;
        int oIdx = -1;
        joinColLookup(*lsch, *rsch, lEff, rEff, sel.table, sel.join_table, ref,
                      oSch, oIdx);
        const std::string& base =
            oSch->columns[static_cast<std::size_t>(oIdx)].name;
        cols.push_back(alias.empty() ? base : alias);
        oids.push_back(
            colTypeOid(oSch->columns[static_cast<std::size_t>(oIdx)].type));
      } else {
        int idx = singleColIndex(*lsch, ref);
        if (idx < 0)
          throw dbengine::sql::SqlError("Unbekannte Spalte: " + ref);
        const std::string& base =
            lsch->columns[static_cast<std::size_t>(idx)].name;
        cols.push_back(alias.empty() ? base : alias);
        oids.push_back(
            colTypeOid(lsch->columns[static_cast<std::size_t>(idx)].type));
      }
    }
  }
  return true;
}

}  // namespace

PgServer::PgServer() : executor_(kv_, mvcc_, nullptr) {}

PgServer::~PgServer() { stop(); }

void PgServer::setAuth(const std::map<std::string, std::string>& users) {
  std::lock_guard<std::mutex> lk(authMu_);
  authUsers_ = users;
}

void PgServer::setAuthRequired(bool required) {
  std::lock_guard<std::mutex> lk(authMu_);
  authRequired_ = required;
}

void PgServer::setScram(bool enabled) {
  std::lock_guard<std::mutex> lk(authMu_);
  scramEnabled_ = enabled;
}

void PgServer::setTlsCert(const std::string& keyPath,
                          const std::string& certPath) {
  std::lock_guard<std::mutex> lk(tlsMu_);
  tlsKeyPath_ = keyPath;
  tlsCertPath_ = certPath;
  tlsEnabled_ = !keyPath.empty() && !certPath.empty();
#ifndef DBENGINE_WITH_TLS
  // Ohne OpenSSL: No-Op (Pfade nur gespeichert, 'N'-Pfad bleibt aktiv).
#endif
}

bool PgServer::running() const { return running_.load(); }

void PgServer::start() {
  if (running_.load()) return;
  listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd_ < 0) throw std::runtime_error("pgserver: socket() failed");

  int one = 1;
  ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1
  addr.sin_port = htons(0);                       // ephemeral
  if (::bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    throw std::runtime_error("pgserver: bind() failed");
  }
  if (::listen(listenFd_, 16) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    throw std::runtime_error("pgserver: listen() failed");
  }
  sockaddr_in bound{};
  socklen_t blen = sizeof(bound);
  if (::getsockname(listenFd_, (sockaddr*)&bound, &blen) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    throw std::runtime_error("pgserver: getsockname() failed");
  }
  port_ = ntohs(bound.sin_port);

  running_.store(true);
  thread_ = std::thread(&PgServer::acceptLoop, this);
}

void PgServer::stop() {
  bool was = running_.exchange(false);
  (void)was;
  if (listenFd_ >= 0) {
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    listenFd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
  port_ = 0;
}

void PgServer::acceptLoop() {
  while (running_.load()) {
    int cfd = ::accept(listenFd_, nullptr, nullptr);
    if (cfd < 0) {
      if (!running_.load()) break;
      if (errno == EINTR) continue;
      if (errno == EINVAL || errno == EBADF) break;
      continue;
    }
    // Je Connection ein detached Thread (V1: ausreichend, Test + psql-Smoke).
    std::thread(&PgServer::handleConn, this, cfd).detach();
  }
}

void PgServer::handleConn(int fd) {
  Conn conn;
  conn.fd = fd;
  auto closeConn = [&]() {
#ifdef DBENGINE_WITH_TLS
    if (conn.ssl != nullptr) {
      SSL_shutdown(conn.ssl);
      SSL_free(conn.ssl);
      conn.ssl = nullptr;
    }
#endif
    ::close(conn.fd);
  };

  // 0) SSLRequest-Schleife (PG-konform, max. 2 Runden):
  //    'N' -> Klartext-Retry auf gleichem fd; 'S' + Handshake -> weiter via TLS.
  //    Danach genau ein normaler Startup (parseStartup wirft sonst FATAL).
  std::vector<uint8_t> startup;
  bool gotStartup = false;
  for (int round = 0; round < 2; ++round) {
    if (!readStartupPacket(conn, startup)) {
      closeConn();
      return;
    }
    if (!isSslRequestPacket(startup)) {
      gotStartup = true;
      break;
    }
#ifdef DBENGINE_WITH_TLS
    bool wantTls = false;
    std::string keyPath;
    std::string certPath;
    {
      std::lock_guard<std::mutex> lk(tlsMu_);
      wantTls = tlsEnabled_;
      keyPath = tlsKeyPath_;
      certPath = tlsCertPath_;
    }
    if (wantTls) {
      uint8_t s = 'S';
      if (!sendRaw(conn.fd, &s, 1)) {
        closeConn();
        return;
      }
      if (!tlsHandshake(conn, certPath, keyPath)) {
        closeConn();
        return;
      }
      continue;  // naechstes Paket (Startup) kommt ueber TLS
    }
#endif
    {
      uint8_t n = 'N';
      if (!sendRaw(conn.fd, &n, 1)) {
        closeConn();
        return;
      }
      continue;  // Client faellt auf Klartext zurueck, sendet Startup neu
    }
  }
  if (!gotStartup) {
    auto err =
        dbengine::pgwire::encodeError("FATAL", "08P01", "Startup erwartet");
    connSend(conn, err);
    closeConn();
    return;
  }
  dbengine::pgwire::StartupParams startupParams;
  try {
    startupParams = dbengine::pgwire::parseStartup(startup);
  } catch (const std::exception& e) {
    auto err = dbengine::pgwire::encodeError("FATAL", "08P01", e.what());
    connSend(conn, err);
    closeConn();
    return;
  }

  // 1b) Auth-Hook (opt-in). Default Trust-All: direkt zu 2).
  // Wenn required und SCRAM aus (Default): R(3 Cleartext) statt R(0),
  // PasswordMessage ('p') lesen, gegen authUsers_ vergleichen;
  // Fail -> E FATAL 28P01 + close, OK -> 2). Byte-identisch zu bisher.
  // Wenn required + setScram(true) + User-Eintrag "scram:...": SCRAM-SHA-256
  // (RFC 5802 Server-Seite): R(10,'SCRAM-SHA-256') -> Client-First ->
  // R(11,Server-First r=combined,i=iter,s=salt) -> Client-Final-Proof
  // (HMAC-SHA256/XOR gegen StoredKey) -> bei OK R(12,"v=...") und weiter
  // zu 2) (R(0)+Z), bei Fail E FATAL 28P01 + close. Klartext-Eintrag oder
  // unbekannter User bei aktivem SCRAM = Legacy-R3-Pfad. Ohne
  // DBENGINE_WITH_TLS wird SCRAM mit 28P01 verweigert (kein eigenes Krypto).
  {
    bool required = false;
    bool scramOn = false;
    std::map<std::string, std::string> users;
    {
      std::lock_guard<std::mutex> lk(authMu_);
      required = authRequired_;
      scramOn = scramEnabled_;
      users = authUsers_;
    }
    if (required) {
      bool wantScram = false;
      {
        auto it = users.find(startupParams.user);
        if (scramOn && it != users.end() && isScramEntry(it->second))
          wantScram = true;
      }
      if (!wantScram) {
        if (!connSend(conn, encodeAuthCleartext())) {
          closeConn();
          return;
        }
        char ptype = 0;
        std::vector<uint8_t> pmsg;
        if (!readTypedMessage(conn, ptype, pmsg)) {
          closeConn();
          return;
        }
        std::optional<std::string> pw;
        if (ptype == 'p') pw = parsePasswordMessage(pmsg);
        bool ok = false;
        if (pw.has_value()) {
          auto it = users.find(startupParams.user);
          if (it != users.end() && pw.value() == it->second) ok = true;
        }
        if (!ok) {
          auto err = dbengine::pgwire::encodeError(
              "FATAL", "28P01",
              std::string("password authentication failed for user \"") +
                  startupParams.user + "\"");
          connSend(conn, err);
          closeConn();
          return;
        }
        // OK: weiter zu 2) (R(0) + Z wie bisher).
      } else {
        auto failScram = [&](const std::string& detail) {
          auto err = dbengine::pgwire::encodeError(
              "FATAL", "28P01",
              std::string("password authentication failed for user \"") +
                  startupParams.user + "\"" +
                  (detail.empty() ? "" : ": " + detail));
          connSend(conn, err);
          closeConn();
        };
#ifdef DBENGINE_WITH_TLS
        std::optional<ScramVerifier> verifier;
        {
          auto it = users.find(startupParams.user);
          if (it != users.end()) verifier = parseScramEntry(it->second);
        }
        if (!verifier.has_value()) {
          failScram("invalid SCRAM verifier");
          return;
        }
        if (!connSend(conn, encodeAuthSasl())) {
          closeConn();
          return;
        }
        char itype = 0;
        std::vector<uint8_t> imsg;
        if (!readTypedMessage(conn, itype, imsg)) {
          closeConn();
          return;
        }
        std::optional<SaslInitial> init;
        if (itype == 'p') init = parseSaslInitial(imsg);
        if (!init.has_value() || !init->hasInitial ||
            init->mech != kScramMech) {
          failScram("invalid SASLInitialResponse");
          return;
        }
        std::optional<ClientFirst> cfirst = parseClientFirst(init->initial);
        if (!cfirst.has_value() || cfirst->user != startupParams.user) {
          failScram("invalid client-first-message");
          return;
        }
        const std::string combined = cfirst->nonce + scramServerNonce();
        if (combined.empty() || combined.find(',') != std::string::npos) {
          failScram("invalid nonce");
          return;
        }
        const std::string serverFirst = "r=" + combined + ",s=" +
                                        verifier->saltB64 + ",i=" +
                                        std::to_string(verifier->iter);
        if (!connSend(conn, encodeAuthSaslCont(11, serverFirst))) {
          closeConn();
          return;
        }
        char ftype = 0;
        std::vector<uint8_t> fmsg;
        if (!readTypedMessage(conn, ftype, fmsg)) {
          closeConn();
          return;
        }
        std::optional<std::string> cfinalRaw;
        if (ftype == 'p') cfinalRaw = parseSaslResponse(fmsg);
        if (!cfinalRaw.has_value()) {
          failScram("invalid SASLResponse");
          return;
        }
        std::optional<ClientFinal> cfinal = parseClientFinal(*cfinalRaw);
        if (!cfinal.has_value() || cfinal->nonce != combined) {
          failScram("invalid client-final-message");
          return;
        }
        // Channel-Binding: c= muss base64(gs2-Header) sein ("n,, "->"biws").
        {
          auto cdec = scramB64Decode(cfinal->cbind);
          std::string expectGs2 = cfirst->gs2;
          std::string got;
          if (cdec.has_value())
            got.assign(reinterpret_cast<const char*>(cdec->data()),
                       cdec->size());
          if (!cdec.has_value() || got != expectGs2) {
            failScram("channel-binding mismatch");
            return;
          }
        }
        auto proofBytes = scramB64Decode(cfinal->proofB64);
        if (!proofBytes.has_value() || proofBytes->size() != 32) {
          failScram("invalid proof");
          return;
        }
        const std::string authMsg =
            cfirst->bare + "," + serverFirst + "," + cfinal->withoutProof;
        uint8_t clientSig[32] = {0};
        uint8_t serverSig[32] = {0};
        bool cryptoOk = false;
        if (scramHmacStr(verifier->storedKey, authMsg, clientSig)) {
          std::vector<uint8_t> clientKey(32);
          for (std::size_t k = 0; k < 32; ++k)
            clientKey[k] = static_cast<uint8_t>((*proofBytes)[k] ^ clientSig[k]);
          uint8_t check[32] = {0};
          if (scramSha256(clientKey.data(), clientKey.size(), check)) {
            std::vector<uint8_t> checkV(check, check + 32);
            if (scramConstEq(checkV, verifier->storedKey)) {
              // Server-Signatur: mit ServerKey (RFC) bzw. StoredKey-Fallback.
              const std::vector<uint8_t>& skey = verifier->hasServerKey
                                                     ? verifier->serverKey
                                                     : verifier->storedKey;
              if (scramHmacStr(skey, authMsg, serverSig)) cryptoOk = true;
            }
          }
          OPENSSL_cleanse(clientSig, sizeof(clientSig));
          OPENSSL_cleanse(clientKey.data(), clientKey.size());
        }
        if (!cryptoOk) {
          OPENSSL_cleanse(serverSig, sizeof(serverSig));
          failScram("SCRAM proof mismatch");
          return;
        }
        const std::string serverFinal =
            "v=" + scramB64Encode(serverSig, sizeof(serverSig));
        OPENSSL_cleanse(serverSig, sizeof(serverSig));
        if (!connSend(conn, encodeAuthSaslCont(12, serverFinal))) {
          closeConn();
          return;
        }
        // OK: weiter zu 2) (R(0) + Z wie bisher; Reihenfolge R12,R0,Z).
#else
        // Ohne OpenSSL: SCRAM explizit verweigert (kein R10, kein R3).
        (void)users;
        failScram("SCRAM-SHA-256 requires DBENGINE_WITH_TLS build");
        return;
#endif
      }
    }
  }

  // 2) AuthOk (psql/libpq erwartet 'R') + ReadyForQuery('I').
  if (!connSend(conn, encodeAuthOk())) {
    closeConn();
    return;
  }
  if (!connSend(conn, dbengine::pgwire::encodeReadyForQuery('I'))) {
    closeConn();
    return;
  }

  // 3) Loop: Q -> T+D+C+Z (SELECT) | C+Z (INSERT/CREATE),
  //         E+Z bei Executor-Fehler (Conn bleibt offen). X -> close.
  //         Extended minimal parameterlos: P->1, D->T/n, B->2, E->T/D/C,
  //         S->Z, C->3; mit Parametern -> E(0A000)+Z. Sonst E(0A000)+Z.
  // Per-Connection State: prepared Statements + Portale (Name -> Query).
  std::map<std::string, std::string> prepStmts;
  std::map<std::string, std::string> portals;
  bool extNeedSync = false;
  bool extErrZ = false;
  auto sendExtError = [&](const std::string& code, const std::string& what,
                          bool& brk) -> bool {
    auto err = dbengine::pgwire::encodeError("ERROR", code, what);
    if (!connSend(conn, err)) {
      brk = true;
      return false;
    }
    if (!connSend(conn, dbengine::pgwire::encodeReadyForQuery('I'))) {
      brk = true;
      return false;
    }
    extErrZ = true;
    extNeedSync = false;
    return true;
  };
  while (true) {
    char type = 0;
    std::vector<uint8_t> msg;
    if (!readTypedMessage(conn, type, msg)) break;
    if (type == 'X') break;  // Terminate
    if (type == 'Q') {
      std::optional<std::string> q;
      try {
        q = dbengine::pgwire::parseQueryMessage(msg);
      } catch (const std::exception& e) {
        auto err = dbengine::pgwire::encodeError("ERROR", "42601", e.what());
        if (!connSend(conn, err)) break;
        if (!connSend(conn, dbengine::pgwire::encodeReadyForQuery('I'))) break;
        continue;
      }
      if (!q.has_value()) {
        auto err = dbengine::pgwire::encodeError(
            "ERROR", "0A000", "nur Simple Protocol (Q) in V1");
        if (!connSend(conn, err)) break;
        if (!connSend(conn, dbengine::pgwire::encodeReadyForQuery('I'))) break;
        continue;
      }
      // Sonderpfad: byte-identisch T(?column?)/D("1")/C(SELECT 1).
      if (isSelectOne(*q)) {
        auto t = dbengine::pgwire::encodeRowDescription({"?column?"});
        auto d = dbengine::pgwire::encodeDataRow({"1"});
        auto c = dbengine::pgwire::encodeCommandComplete("SELECT 1");
        auto z = dbengine::pgwire::encodeReadyForQuery('I');
        if (!connSend(conn, t)) break;
        if (!connSend(conn, d)) break;
        if (!connSend(conn, c)) break;
        if (!connSend(conn, z)) break;
        continue;
      }
      // Session: Q-String an die server-eigene Executor-Instanz.
      dbengine::sql::Result res;
      std::vector<int32_t> oids;
      std::string tag;
      try {
        std::lock_guard<std::mutex> lk(execMu_);
        res = executor_.execute(*q);
        oids = resolveOids(res, *q, executor_);
        tag = res.message;
      } catch (const dbengine::sql::SqlError& e) {
        auto err = dbengine::pgwire::encodeError("ERROR", "42601", e.what());
        if (!connSend(conn, err)) break;
        if (!connSend(conn, dbengine::pgwire::encodeReadyForQuery('I'))) break;
        continue;
      } catch (const std::exception& e) {
        auto err = dbengine::pgwire::encodeError("ERROR", "0A000", e.what());
        if (!connSend(conn, err)) break;
        if (!connSend(conn, dbengine::pgwire::encodeReadyForQuery('I'))) break;
        continue;
      }
      if (!res.columns.empty()) {
        auto t = encodeRowDescriptionTyped(res.columns, oids);
        if (!connSend(conn, t)) break;
        bool ok = true;
        for (const auto& row : res.rows) {
          auto d = encodeDataRowValues(row);
          if (!connSend(conn, d)) {
            ok = false;
            break;
          }
        }
        if (!ok) break;
        if (tag.empty()) tag = "SELECT " + std::to_string(res.rows.size());
        auto c = dbengine::pgwire::encodeCommandComplete(tag);
        auto z = dbengine::pgwire::encodeReadyForQuery('I');
        if (!connSend(conn, c)) break;
        if (!connSend(conn, z)) break;
      } else {
        if (tag.empty()) tag = "SELECT 0";
        auto c = dbengine::pgwire::encodeCommandComplete(tag);
        auto z = dbengine::pgwire::encodeReadyForQuery('I');
        if (!connSend(conn, c)) break;
        if (!connSend(conn, z)) break;
      }
      continue;
    }
    if (type == 'P') {  // Parse: stmt\0 query\0 i16 nParams (i32 OIDs)
      bool brk = false;
      try {
        ParseMsg pm = parseParseMsg(msg);
        if (pm.numParams != 0 || containsDollarParam(pm.query)) {
          if (!sendExtError("0A000",
                            "extended mit Parametern nicht unterstuetzt",
                            brk)) {
            break;
          }
          if (brk) break;
          continue;
        }
        try {
          // Sonderfall wie im Q-Pfad: SELECT 1 braucht kein FROM.
          if (!isSelectOne(pm.query)) {
            (void)dbengine::sql::parseStatement(pm.query);
          }
        } catch (const dbengine::sql::SqlError& e) {
          if (!sendExtError("42601", e.what(), brk)) break;
          if (brk) break;
          continue;
        } catch (const std::exception& e) {
          if (!sendExtError("0A000", e.what(), brk)) break;
          if (brk) break;
          continue;
        }
        prepStmts[pm.stmt] = pm.query;
        if (!connSend(conn, encodeParseComplete())) break;
        extNeedSync = true;
        extErrZ = false;
      } catch (const std::exception& e) {
        if (!sendExtError("42601", e.what(), brk)) break;
        if (brk) break;
      }
      continue;
    }
    if (type == 'B') {  // Bind: portal\0 stmt\0 ... i16 nParams ...
      bool brk = false;
      try {
        BindMsg bm = parseBindMsg(msg);
        if (bm.numParams != 0) {
          if (!sendExtError("0A000",
                            "extended mit Parametern nicht unterstuetzt",
                            brk)) {
            break;
          }
          if (brk) break;
          continue;
        }
        auto it = prepStmts.find(bm.stmt);
        if (it == prepStmts.end()) {
          if (!sendExtError("42601", "unknown prepared statement", brk))
            break;
          if (brk) break;
          continue;
        }
        if (containsDollarParam(it->second)) {
          if (!sendExtError("0A000",
                            "extended mit Parametern nicht unterstuetzt",
                            brk)) {
            break;
          }
          if (brk) break;
          continue;
        }
        portals[bm.portal] = it->second;
        if (!connSend(conn, encodeBindComplete())) break;
        extNeedSync = true;
        extErrZ = false;
      } catch (const std::exception& e) {
        if (!sendExtError("42601", e.what(), brk)) break;
        if (brk) break;
      }
      continue;
    }
    if (type == 'D') {  // Describe: 'S'/'P' + name\0 -> T/n ohne Execute
      bool brk = false;
      try {
        DescribeMsg dm = parseDescribeMsg(msg);
        std::string q;
        if (dm.kind == 'S') {
          auto it = prepStmts.find(dm.name);
          if (it == prepStmts.end()) {
            if (!sendExtError("42601", "unknown prepared statement", brk))
              break;
            if (brk) break;
            continue;
          }
          q = it->second;
        } else if (dm.kind == 'P') {
          auto it = portals.find(dm.name);
          if (it == portals.end()) {
            if (!sendExtError("42601", "unknown portal", brk)) break;
            if (brk) break;
            continue;
          }
          q = it->second;
        } else {
          if (!sendExtError("0A000", "Describe: Typ muss S/P sein", brk))
            break;
          if (brk) break;
          continue;
        }
        if (containsDollarParam(q)) {
          if (!sendExtError("0A000",
                            "extended mit Parametern nicht unterstuetzt",
                            brk)) {
            break;
          }
          if (brk) break;
          continue;
        }
        std::vector<std::string> cols;
        std::vector<int32_t> oids;
        bool hasCols = false;
        try {
          std::lock_guard<std::mutex> lk(execMu_);
          hasCols = describeProjection(q, executor_, cols, oids);
        } catch (const dbengine::sql::SqlError& e) {
          if (!sendExtError("42601", e.what(), brk)) break;
          if (brk) break;
          continue;
        } catch (const std::exception& e) {
          if (!sendExtError("0A000", e.what(), brk)) break;
          if (brk) break;
          continue;
        }
        if (!hasCols) {
          if (!connSend(conn, encodeNoData())) break;
        } else {
          auto t = encodeRowDescriptionTyped(cols, oids);
          if (!connSend(conn, t)) break;
        }
        extNeedSync = true;
        extErrZ = false;
      } catch (const std::exception& e) {
        if (!sendExtError("42601", e.what(), brk)) break;
        if (brk) break;
      }
      continue;
    }
    if (type == 'E') {  // Execute: portal\0 i32 maxRows -> T/D/C wie Q-Pfad
      bool brk = false;
      try {
        ExecuteMsg em = parseExecuteMsg(msg);
        auto it = portals.find(em.portal);
        if (it == portals.end()) {
          if (!sendExtError("42601", "unknown portal", brk)) break;
          if (brk) break;
          continue;
        }
        std::string q = it->second;
        if (containsDollarParam(q)) {
          if (!sendExtError("0A000",
                            "extended mit Parametern nicht unterstuetzt",
                            brk)) {
            break;
          }
          if (brk) break;
          continue;
        }
        if (isSelectOne(q)) {
          auto t = dbengine::pgwire::encodeRowDescription({"?column?"});
          auto d = dbengine::pgwire::encodeDataRow({"1"});
          auto c = dbengine::pgwire::encodeCommandComplete("SELECT 1");
          if (!connSend(conn, t)) break;
          if (!connSend(conn, d)) break;
          if (!connSend(conn, c)) break;
          extNeedSync = true;
          extErrZ = false;
          continue;
        }
        dbengine::sql::Result res;
        std::vector<int32_t> oids;
        std::string tag;
        try {
          std::lock_guard<std::mutex> lk(execMu_);
          res = executor_.execute(q);
          oids = resolveOids(res, q, executor_);
          tag = res.message;
        } catch (const dbengine::sql::SqlError& e) {
          if (!sendExtError("42601", e.what(), brk)) break;
          if (brk) break;
          continue;
        } catch (const std::exception& e) {
          if (!sendExtError("0A000", e.what(), brk)) break;
          if (brk) break;
          continue;
        }
        if (!res.columns.empty()) {
          auto t = encodeRowDescriptionTyped(res.columns, oids);
          if (!connSend(conn, t)) break;
          bool ok = true;
          for (const auto& row : res.rows) {
            auto d = encodeDataRowValues(row);
            if (!connSend(conn, d)) {
              ok = false;
              break;
            }
          }
          if (!ok) break;
          if (tag.empty()) tag = "SELECT " + std::to_string(res.rows.size());
          auto c = dbengine::pgwire::encodeCommandComplete(tag);
          if (!connSend(conn, c)) break;
        } else {
          if (tag.empty()) tag = "SELECT 0";
          auto c = dbengine::pgwire::encodeCommandComplete(tag);
          if (!connSend(conn, c)) break;
        }
        extNeedSync = true;
        extErrZ = false;
      } catch (const std::exception& e) {
        if (!sendExtError("42601", e.what(), brk)) break;
        if (brk) break;
      }
      continue;
    }
    if (type == 'S') {  // Sync -> Z (nach Fehler evtl. schon gesendet)
      if (msg.size() != 5) {
        bool brk = false;
        if (!sendExtError("42601", "Sync: Laenge passt nicht", brk)) break;
        if (brk) break;
        continue;
      }
      if (extErrZ && !extNeedSync) {
        extErrZ = false;
        continue;  // Z des Fehlers bereits gesendet, kein Doppel-Z
      }
      if (!connSend(conn, dbengine::pgwire::encodeReadyForQuery('I'))) break;
      extNeedSync = false;
      extErrZ = false;
      continue;
    }
    if (type == 'C') {  // Close: 'S'/'P' + name\0 -> '3'
      bool brk = false;
      try {
        CloseMsg cm = parseCloseMsg(msg);
        if (cm.kind == 'S') {
          prepStmts.erase(cm.name);
        } else if (cm.kind == 'P') {
          portals.erase(cm.name);
        } else {
          if (!sendExtError("0A000", "Close: Typ muss S/P sein", brk))
            break;
          if (brk) break;
          continue;
        }
        if (!connSend(conn, encodeCloseComplete())) break;
        extNeedSync = true;
        extErrZ = false;
      } catch (const std::exception& e) {
        if (!sendExtError("42601", e.what(), brk)) break;
        if (brk) break;
      }
      continue;
    }
    // Unbekannter Typ: Error + ReadyForQuery, Connection bleibt offen.
    auto err = dbengine::pgwire::encodeError(
        "ERROR", "0A000", std::string("nur Q/X in V1 (got '") + type + "')");
    if (!connSend(conn, err)) break;
    if (!connSend(conn, dbengine::pgwire::encodeReadyForQuery('I'))) break;
  }
  closeConn();
}

}  // namespace dbengine::pgserver
