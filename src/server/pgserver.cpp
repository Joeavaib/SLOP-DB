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
#include <map>
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
  dbengine::pgwire::StartupParams startupParams;
  try {
    startupParams = dbengine::pgwire::parseStartup(startup);
  } catch (const std::exception& e) {
    auto err = dbengine::pgwire::encodeError("FATAL", "08P01", e.what());
    sendAll(fd, err);
    ::close(fd);
    return;
  }

  // 1b) Auth-Hook (opt-in). Default Trust-All: direkt zu 2).
  // Wenn required: R(3 Cleartext) statt R(0), PasswordMessage ('p') lesen,
  // gegen authUsers_ vergleichen; Fail -> E FATAL 28P01 + close, OK -> 2).
  {
    bool required = false;
    std::map<std::string, std::string> users;
    {
      std::lock_guard<std::mutex> lk(authMu_);
      required = authRequired_;
      users = authUsers_;
    }
    if (required) {
      if (!sendAll(fd, encodeAuthCleartext())) {
        ::close(fd);
        return;
      }
      char ptype = 0;
      std::vector<uint8_t> pmsg;
      if (!readTypedMessage(fd, ptype, pmsg)) {
        ::close(fd);
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
        sendAll(fd, err);
        ::close(fd);
        return;
      }
      // OK: weiter zu 2) (R(0) + Z wie bisher).
    }
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
    if (!sendAll(fd, err)) {
      brk = true;
      return false;
    }
    if (!sendAll(fd, dbengine::pgwire::encodeReadyForQuery('I'))) {
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
        if (!sendAll(fd, encodeParseComplete())) break;
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
        if (!sendAll(fd, encodeBindComplete())) break;
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
          if (!sendAll(fd, encodeNoData())) break;
        } else {
          auto t = encodeRowDescriptionTyped(cols, oids);
          if (!sendAll(fd, t)) break;
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
          if (!sendAll(fd, t)) break;
          if (!sendAll(fd, d)) break;
          if (!sendAll(fd, c)) break;
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
          if (!sendAll(fd, c)) break;
        } else {
          if (tag.empty()) tag = "SELECT 0";
          auto c = dbengine::pgwire::encodeCommandComplete(tag);
          if (!sendAll(fd, c)) break;
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
      if (!sendAll(fd, dbengine::pgwire::encodeReadyForQuery('I'))) break;
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
        if (!sendAll(fd, encodeCloseComplete())) break;
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
    if (!sendAll(fd, err)) break;
    if (!sendAll(fd, dbengine::pgwire::encodeReadyForQuery('I'))) break;
  }
  ::close(fd);
}

}  // namespace dbengine::pgserver
