// s12-pgserver: TCPServer (POSIX, 127.0.0.1, ephemeral port).
// Nutzt pgwire-Codec fuer Framing. Siehe include/dbengine/server/pgserver.h.

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
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "dbengine/server/pgwire.h"
#include "dbengine/sql/parser.h"

namespace dbengine::pgserver {
namespace {

using dbengine::pgwire::getInt32BE;
using dbengine::pgwire::putInt32BE;

bool sendAll(int fd, const uint8_t* data, std::size_t len) {
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

bool sendAll(int fd, const std::vector<uint8_t>& v) {
  if (v.empty()) return true;
  return sendAll(fd, v.data(), v.size());
}

bool recvAll(int fd, uint8_t* out, std::size_t len) {
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

// Startup: Int32 len | Rest(len-4). Gibt volles Paket (inkl. len) zurueck.
bool readStartupPacket(int fd, std::vector<uint8_t>& out) {
  uint8_t hdr[4];
  if (!recvAll(fd, hdr, 4)) return false;
  int32_t len = getInt32BE(hdr);
  if (len < 8 || len > 1024 * 1024) return false;
  out.resize(static_cast<std::size_t>(len));
  std::memcpy(out.data(), hdr, 4);
  if (!recvAll(fd, out.data() + 4, static_cast<std::size_t>(len) - 4))
    return false;
  return true;
}

// Normale Nachricht: 'T' | Int32 len | Payload(len-4).
// Gibt Typ + volles Paket (Typ + len + payload) zurueck.
bool readTypedMessage(int fd, char& type, std::vector<uint8_t>& out) {
  uint8_t hdr[5];
  if (!recvAll(fd, hdr, 5)) return false;
  type = static_cast<char>(hdr[0]);
  int32_t len = getInt32BE(hdr + 1);
  if (len < 4 || len > 16 * 1024 * 1024) return false;
  out.resize(1 + static_cast<std::size_t>(len));
  std::memcpy(out.data(), hdr, 5);
  std::size_t rest = static_cast<std::size_t>(len) - 4;
  if (rest > 0 && !recvAll(fd, out.data() + 5, rest)) return false;
  return true;
}

std::vector<uint8_t> encodeAuthOk() {
  std::vector<uint8_t> out;
  out.push_back('R');
  putInt32BE(out, 8);
  putInt32BE(out, 0);
  return out;
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

}  // namespace

PgServer::PgServer() : executor_(kv_, mvcc_, nullptr) {}

PgServer::~PgServer() { stop(); }

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
  // 1) Startup lesen (kein Typ-Byte).
  std::vector<uint8_t> startup;
  if (!readStartupPacket(fd, startup)) {
    ::close(fd);
    return;
  }
  try {
    (void)dbengine::pgwire::parseStartup(startup);
  } catch (const std::exception& e) {
    auto err = dbengine::pgwire::encodeError("FATAL", "08P01", e.what());
    sendAll(fd, err);
    ::close(fd);
    return;
  }

  // 2) AuthOk (psql/libpq erwartet 'R') + ReadyForQuery('I').
  if (!sendAll(fd, encodeAuthOk())) {
    ::close(fd);
    return;
  }
  if (!sendAll(fd, dbengine::pgwire::encodeReadyForQuery('I'))) {
    ::close(fd);
    return;
  }

  // 3) Loop: Q -> T+D+C+Z (SELECT) | C+Z (INSERT/CREATE),
  //         E+Z bei Executor-Fehler (Conn bleibt offen). X -> close.
  //         Extended/COPY (nicht Q/X) -> E(0A000)+Z, unveraendert.
  while (true) {
    char type = 0;
    std::vector<uint8_t> msg;
    if (!readTypedMessage(fd, type, msg)) break;
    if (type == 'X') break;  // Terminate
    if (type == 'Q') {
      std::optional<std::string> q;
      try {
        q = dbengine::pgwire::parseQueryMessage(msg);
      } catch (const std::exception& e) {
        auto err = dbengine::pgwire::encodeError("ERROR", "42601", e.what());
        if (!sendAll(fd, err)) break;
        if (!sendAll(fd, dbengine::pgwire::encodeReadyForQuery('I'))) break;
        continue;
      }
      if (!q.has_value()) {
        auto err = dbengine::pgwire::encodeError(
            "ERROR", "0A000", "nur Simple Protocol (Q) in V1");
        if (!sendAll(fd, err)) break;
        if (!sendAll(fd, dbengine::pgwire::encodeReadyForQuery('I'))) break;
        continue;
      }
      // Sonderpfad: byte-identisch T(?column?)/D("1")/C(SELECT 1).
      if (isSelectOne(*q)) {
        auto t = dbengine::pgwire::encodeRowDescription({"?column?"});
        auto d = dbengine::pgwire::encodeDataRow({"1"});
        auto c = dbengine::pgwire::encodeCommandComplete("SELECT 1");
        auto z = dbengine::pgwire::encodeReadyForQuery('I');
        if (!sendAll(fd, t)) break;
        if (!sendAll(fd, d)) break;
        if (!sendAll(fd, c)) break;
        if (!sendAll(fd, z)) break;
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
        if (!sendAll(fd, err)) break;
        if (!sendAll(fd, dbengine::pgwire::encodeReadyForQuery('I'))) break;
        continue;
      } catch (const std::exception& e) {
        auto err = dbengine::pgwire::encodeError("ERROR", "0A000", e.what());
        if (!sendAll(fd, err)) break;
        if (!sendAll(fd, dbengine::pgwire::encodeReadyForQuery('I'))) break;
        continue;
      }
      if (!res.columns.empty()) {
        auto t = encodeRowDescriptionTyped(res.columns, oids);
        if (!sendAll(fd, t)) break;
        bool ok = true;
        for (const auto& row : res.rows) {
          auto d = encodeDataRowValues(row);
          if (!sendAll(fd, d)) {
            ok = false;
            break;
          }
        }
        if (!ok) break;
        if (tag.empty()) tag = "SELECT " + std::to_string(res.rows.size());
        auto c = dbengine::pgwire::encodeCommandComplete(tag);
        auto z = dbengine::pgwire::encodeReadyForQuery('I');
        if (!sendAll(fd, c)) break;
        if (!sendAll(fd, z)) break;
      } else {
        if (tag.empty()) tag = "SELECT 0";
        auto c = dbengine::pgwire::encodeCommandComplete(tag);
        auto z = dbengine::pgwire::encodeReadyForQuery('I');
        if (!sendAll(fd, c)) break;
        if (!sendAll(fd, z)) break;
      }
      continue;
    }
    // Unbekannter Typ in V1: Error + ReadyForQuery, Connection bleibt offen.
    auto err = dbengine::pgwire::encodeError(
        "ERROR", "0A000", std::string("nur Q/X in V1 (got '") + type + "')");
    if (!sendAll(fd, err)) break;
    if (!sendAll(fd, dbengine::pgwire::encodeReadyForQuery('I'))) break;
  }
  ::close(fd);
}

}  // namespace dbengine::pgserver
