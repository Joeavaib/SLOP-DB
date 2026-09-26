// PGWire-Stub V1: nur Startup + Simple-Query + Error/Ready/Complete.
// Siehe include/dbengine/server/pgwire.h.

#include "dbengine/server/pgwire.h"

namespace dbengine::pgwire {

void putInt32BE(std::vector<uint8_t>& out, int32_t v) {
  out.push_back((uint8_t)((v >> 24) & 0xFF));
  out.push_back((uint8_t)((v >> 16) & 0xFF));
  out.push_back((uint8_t)((v >> 8) & 0xFF));
  out.push_back((uint8_t)(v & 0xFF));
}

int32_t getInt32BE(const uint8_t* p) {
  return (int32_t)(((int32_t)p[0] << 24) | ((int32_t)p[1] << 16) |
                   ((int32_t)p[2] << 8) | (int32_t)p[3]);
}

namespace {
std::string readCString(const uint8_t* data, std::size_t len,
                        std::size_t& pos) {
  std::size_t start = pos;
  while (pos < len && data[pos] != 0) ++pos;
  if (pos >= len) throw ProtoError("Startup: unterminierter String");
  std::string s((const char*)data + start, pos - start);
  ++pos;  // NUL
  return s;
}
void putCString(std::vector<uint8_t>& out, const std::string& s) {
  out.insert(out.end(), s.begin(), s.end());
  out.push_back(0);
}
}  // namespace

StartupParams parseStartup(const uint8_t* data, std::size_t len) {
  if (len < 8) throw ProtoError("Startup: Paket zu kurz");
  int32_t msgLen = getInt32BE(data);
  int32_t proto = getInt32BE(data + 4);
  if (msgLen != (int32_t)len)
    throw ProtoError("Startup: Laengenfeld passt nicht");
  if (proto == kSslRequestCode)
    // Hinweis: PgServer beantwortet SSLRequest PG-konform ('N' ohne TLS,
    // 'S' + Handshake mit TLS) und erreicht diesen Throw im Normalbetrieb
    // nicht; direkter Codec-Gebrauch faellt hier weiterhin auf FATAL zurueck.
    throw ProtoError("SSLRequest: V1 ohne TLS (Server lehnt ab)");
  if (proto == kCancelRequestCode)
    throw ProtoError("CancelRequest: V1 nicht unterstuetzt");
  if (proto != kProtocolV3)
    throw ProtoError("Nur Protokoll 3.0 wird unterstuetzt");
  StartupParams p;
  std::size_t pos = 8;
  while (true) {
    if (pos >= len) throw ProtoError("Startup: fehlender Terminator");
    if (data[pos] == 0) {
      ++pos;
      break;
    }
    std::string k = readCString(data, len, pos);
    if (pos >= len) throw ProtoError("Startup: Wert fehlt");
    if (data[pos] == 0) {
      // leerer Key = Terminator wurde schon oben behandelt; hier: Wert leer?
      // (k war Key, jetzt Terminator ohne Wert -> Fehler)
      throw ProtoError("Startup: Wert fehlt fuer " + k);
    }
    std::string v = readCString(data, len, pos);
    p.params[k] = v;
  }
  auto it = p.params.find("user");
  if (it != p.params.end()) p.user = it->second;
  auto db = p.params.find("database");
  if (db != p.params.end())
    p.database = db->second;
  else
    p.database = p.user;
  return p;
}

std::optional<std::string> parseQueryMessage(const uint8_t* data,
                                             std::size_t len) {
  if (len < 5) throw ProtoError("Query: Paket zu kurz");
  if (data[0] != 'Q') return std::nullopt;  // V1: nur Q
  int32_t msgLen = getInt32BE(data + 1);
  if (msgLen != (int32_t)(len - 1))
    throw ProtoError("Query: Laengenfeld passt nicht");
  // Inhalt: query\0
  if (data[len - 1] != 0) throw ProtoError("Query: fehlender NUL-Terminator");
  return std::string((const char*)data + 5, len - 5 - 1);
}

std::vector<uint8_t> encodeError(const std::string& severity,
                                 const std::string& code,
                                 const std::string& message) {
  std::vector<uint8_t> body;
  body.push_back('S');
  putCString(body, severity);
  body.push_back('V');
  putCString(body, severity);
  body.push_back('C');
  putCString(body, code);
  body.push_back('M');
  putCString(body, message);
  body.push_back(0);
  std::vector<uint8_t> out;
  out.push_back('E');
  putInt32BE(out, (int32_t)(body.size() + 4));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

std::vector<uint8_t> encodeReadyForQuery(char status) {
  return {'Z', 0, 0, 0, 5, (uint8_t)status};
}

std::vector<uint8_t> encodeCommandComplete(const std::string& tag) {
  std::vector<uint8_t> out;
  out.push_back('C');
  putInt32BE(out, (int32_t)(tag.size() + 1 + 4));
  putCString(out, tag);
  return out;
}

std::vector<uint8_t> encodeRowDescription(
    const std::vector<std::string>& columns) {
  std::vector<uint8_t> body;
  putInt32BE(body, (int32_t)columns.size());
  for (auto& c : columns) {
    putCString(body, c);
    putInt32BE(body, 0);    // table OID
    // int16 attr, int32 type OID (25 = text), int16 typlen, int32 typmod,
    // int16 format
    body.push_back(0);
    body.push_back(0);  // attrno
    putInt32BE(body, 25);
    body.push_back(0xFF);
    body.push_back(0xFF);  // typlen -1
    putInt32BE(body, -1);  // typmod
    body.push_back(0);
    body.push_back(0);  // text format
  }
  std::vector<uint8_t> out;
  out.push_back('T');
  putInt32BE(out, (int32_t)(body.size() + 4));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

std::vector<uint8_t> encodeDataRow(const std::vector<std::string>& values) {
  std::vector<uint8_t> body;
  // int16 count
  body.push_back((uint8_t)((values.size() >> 8) & 0xFF));
  body.push_back((uint8_t)(values.size() & 0xFF));
  for (auto& v : values) {
    putInt32BE(body, (int32_t)v.size());
    body.insert(body.end(), v.begin(), v.end());
  }
  std::vector<uint8_t> out;
  out.push_back('D');
  putInt32BE(out, (int32_t)(body.size() + 4));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

std::vector<uint8_t> encodeStartupRequest(
    const std::map<std::string, std::string>& params) {
  std::vector<uint8_t> body;
  putInt32BE(body, kProtocolV3);
  for (auto& [k, v] : params) {
    putCString(body, k);
    putCString(body, v);
  }
  body.push_back(0);
  std::vector<uint8_t> out;
  putInt32BE(out, (int32_t)(body.size() + 4));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

}  // namespace dbengine::pgwire
