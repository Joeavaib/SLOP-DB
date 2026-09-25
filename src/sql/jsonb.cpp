// s15-jsonb: JsonValue, Parser, Binaer-Encode, contains/arrow, GIN.
// Siehe include/dbengine/sql/jsonb.h fuer Format-Doku.

#include "dbengine/sql/jsonb.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace dbengine::sql {
namespace {

// ---------- Little-Endian Helpers ----------

void putU32LE(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

void putU64LE(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

std::uint32_t getU32LE(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t getU64LE(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= (static_cast<std::uint64_t>(p[i]) << (8 * i));
  return v;
}

// ---------- Parser ----------

class Cursor {
 public:
  explicit Cursor(const std::string& s) : s_(s) {}
  void skipWs() {
    while (pos_ < s_.size() &&
           (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' ||
            s_[pos_] == '\r'))
      ++pos_;
  }
  [[nodiscard]] bool eof() const { return pos_ >= s_.size(); }
  [[nodiscard]] char peek() const {
    if (eof()) throw JsonError("Unerwartetes JSON-Ende");
    return s_[pos_];
  }
  char take() {
    if (eof()) throw JsonError("Unerwartetes JSON-Ende");
    return s_[pos_++];
  }
  void expect(char c) {
    if (eof() || s_[pos_] != c)
      throw JsonError(std::string("Erwartet '") + c + "' in JSON");
    ++pos_;
  }
  void expectLit(const char* lit) {
    for (const char* p = lit; *p; ++p) {
      if (eof() || s_[pos_] != *p)
        throw JsonError(std::string("Ungueltiges Literal, erwartet ") + lit);
      ++pos_;
    }
  }
  [[nodiscard]] std::size_t pos() const { return pos_; }

 private:
  const std::string& s_;
  std::size_t pos_ = 0;
};

JsonValue parseValue(Cursor& c);

std::string parseRawString(Cursor& c) {
  c.expect('"');
  std::string out;
  while (true) {
    if (c.eof()) throw JsonError("Unterminierter JSON-String");
    char ch = c.take();
    if (ch == '"') break;
    if (ch == '\\') {
      if (c.eof()) throw JsonError("Unterminierter Escape in JSON-String");
      char e = c.take();
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          // BMP \uXXXX -> UTF-8. Surrogate-Paare werden zu U+FFFD.
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            if (c.eof()) throw JsonError("Unvollstaendiges \\u-Escape");
            char h = c.take();
            code <<= 4;
            if (h >= '0' && h <= '9') code += static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f')
              code += static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
              code += static_cast<unsigned>(h - 'A' + 10);
            else
              throw JsonError("Ungueltige Hex-Ziffer in \\u-Escape");
          }
          if (code < 0x80) {
            out += static_cast<char>(code);
          } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
          } else {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
          }
          break;
        }
        default: throw JsonError("Ungueltiger Escape in JSON-String");
      }
    } else {
      // Rohe Control-Zeichen < 0x20 sind gemaess RFC ungueltig.
      if (static_cast<unsigned char>(ch) < 0x20)
        throw JsonError("Unescaped Control-Zeichen in JSON-String");
      out += ch;
    }
  }
  return out;
}

JsonValue parseNumber(Cursor& c) {
  std::size_t start = c.pos();
  std::string num;
  if (!c.eof() && c.peek() == '-') num += c.take();
  if (c.eof()) throw JsonError("Ungueltige JSON-Zahl");
  // Integer-Teil
  if (c.peek() == '0') {
    num += c.take();
  } else if (std::isdigit(static_cast<unsigned char>(c.peek()))) {
    while (!c.eof() && std::isdigit(static_cast<unsigned char>(c.peek())))
      num += c.take();
  } else {
    throw JsonError("Ungueltige JSON-Zahl");
  }
  bool isDouble = false;
  if (!c.eof() && c.peek() == '.') {
    isDouble = true;
    num += c.take();
    if (c.eof() || !std::isdigit(static_cast<unsigned char>(c.peek())))
      throw JsonError("Ungueltige JSON-Zahl (Bruchteil)");
    while (!c.eof() && std::isdigit(static_cast<unsigned char>(c.peek())))
      num += c.take();
  }
  if (!c.eof() && (c.peek() == 'e' || c.peek() == 'E')) {
    isDouble = true;
    num += c.take();
    if (!c.eof() && (c.peek() == '+' || c.peek() == '-')) num += c.take();
    if (c.eof() || !std::isdigit(static_cast<unsigned char>(c.peek())))
      throw JsonError("Ungueltige JSON-Zahl (Exponent)");
    while (!c.eof() && std::isdigit(static_cast<unsigned char>(c.peek())))
      num += c.take();
  }
  (void)start;
  try {
    if (!isDouble) {
      std::size_t idx = 0;
      long long v = std::stoll(num, &idx);
      if (idx == num.size()) return JsonValue(static_cast<std::int64_t>(v));
      isDouble = true;  // Overflow -> double-Fallback
    }
    return JsonValue(std::stod(num));
  } catch (const std::out_of_range&) {
    try {
      return JsonValue(std::stod(num));
    } catch (...) {
      throw JsonError("JSON-Zahl ausserhalb des Bereichs: " + num);
    }
  } catch (...) {
    throw JsonError("Ungueltige JSON-Zahl: " + num);
  }
}

JsonValue parseArray(Cursor& c) {
  c.expect('[');
  JsonValue::Array arr;
  c.skipWs();
  if (!c.eof() && c.peek() == ']') {
    c.take();
    return JsonValue(std::move(arr));
  }
  while (true) {
    c.skipWs();
    arr.push_back(parseValue(c));
    c.skipWs();
    if (c.eof()) throw JsonError("Unterminiertes JSON-Array");
    char ch = c.take();
    if (ch == ']') break;
    if (ch != ',') throw JsonError("Erwartet ',' oder ']' in JSON-Array");
  }
  return JsonValue(std::move(arr));
}

JsonValue parseObject(Cursor& c) {
  c.expect('{');
  JsonValue::Object obj;
  c.skipWs();
  if (!c.eof() && c.peek() == '}') {
    c.take();
    return JsonValue(std::move(obj));
  }
  while (true) {
    c.skipWs();
    if (c.eof() || c.peek() != '"')
      throw JsonError("Objekt-Key muss ein String sein");
    std::string key = parseRawString(c);
    c.skipWs();
    c.expect(':');
    c.skipWs();
    JsonValue val = parseValue(c);
    obj[key] = std::move(val);  // Duplikat-Key: letzter gewinnt (PG-like)
    c.skipWs();
    if (c.eof()) throw JsonError("Unterminiertes JSON-Objekt");
    char ch = c.take();
    if (ch == '}') break;
    if (ch != ',') throw JsonError("Erwartet ',' oder '}' in JSON-Objekt");
  }
  return JsonValue(std::move(obj));
}

JsonValue parseValue(Cursor& c) {
  c.skipWs();
  if (c.eof()) throw JsonError("Leerer JSON-Wert");
  char ch = c.peek();
  if (ch == '{') return parseObject(c);
  if (ch == '[') return parseArray(c);
  if (ch == '"') return JsonValue(parseRawString(c));
  if (ch == 't') {
    c.expectLit("true");
    return JsonValue(true);
  }
  if (ch == 'f') {
    c.expectLit("false");
    return JsonValue(false);
  }
  if (ch == 'n') {
    c.expectLit("null");
    return JsonValue();
  }
  if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)))
    return parseNumber(c);
  throw JsonError(std::string("Unerwartetes Zeichen in JSON: '") + ch + "'");
}

// ---------- String-Escape ----------

void appendEscaped(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char ch : s) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
          out += buf;
        } else {
          out += static_cast<char>(ch);
        }
    }
  }
  out += '"';
}

// ---------- Binaer-Codec ----------

void encodeBody(std::vector<std::uint8_t>& out, const JsonValue& v) {
  out.push_back(static_cast<std::uint8_t>(v.type()));
  const auto& d = v.data;
  if (std::holds_alternative<std::monostate>(d)) return;
  if (auto* b = std::get_if<bool>(&d)) {
    out.push_back(*b ? 1 : 0);
  } else if (auto* i = std::get_if<std::int64_t>(&d)) {
    putU64LE(out, static_cast<std::uint64_t>(*i));
  } else if (auto* f = std::get_if<double>(&d)) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(*f));
    std::memcpy(&bits, f, sizeof(bits));
    putU64LE(out, bits);
  } else if (auto* s = std::get_if<std::string>(&d)) {
    putU32LE(out, static_cast<std::uint32_t>(s->size()));
    out.insert(out.end(), s->begin(), s->end());
  } else if (auto* a = std::get_if<JsonValue::Array>(&d)) {
    putU32LE(out, static_cast<std::uint32_t>(a->size()));
    for (const auto& e : *a) encodeBody(out, e);
  } else if (auto* o = std::get_if<JsonValue::Object>(&d)) {
    putU32LE(out, static_cast<std::uint32_t>(o->size()));
    for (const auto& [k, val] : *o) {
      putU32LE(out, static_cast<std::uint32_t>(k.size()));
      out.insert(out.end(), k.begin(), k.end());
      encodeBody(out, val);
    }
  }
}

JsonValue decodeBody(const std::uint8_t* data, std::size_t size,
                     std::size_t& pos) {
  if (pos >= size) throw JsonError("Trunkierter JSONB-Body (Tag fehlt)");
  auto tag = static_cast<JsonType>(data[pos++]);
  auto need = [&](std::size_t n) {
    if (pos + n > size) throw JsonError("Trunkierter JSONB-Body (Payload)");
  };
  switch (tag) {
    case JsonType::Null: return JsonValue();
    case JsonType::Bool: {
      need(1);
      std::uint8_t b = data[pos++];
      if (b > 1) throw JsonError("Ungueltiger JSONB-Bool-Wert");
      return JsonValue(b == 1);
    }
    case JsonType::Int: {
      need(8);
      std::int64_t v = static_cast<std::int64_t>(getU64LE(data + pos));
      pos += 8;
      return JsonValue(v);
    }
    case JsonType::Double: {
      need(8);
      std::uint64_t bits = getU64LE(data + pos);
      pos += 8;
      double f = 0;
      std::memcpy(&f, &bits, sizeof(f));
      return JsonValue(f);
    }
    case JsonType::String: {
      need(4);
      std::uint32_t len = getU32LE(data + pos);
      pos += 4;
      need(len);
      std::string s(reinterpret_cast<const char*>(data + pos), len);
      pos += len;
      return JsonValue(std::move(s));
    }
    case JsonType::Array: {
      need(4);
      std::uint32_t n = getU32LE(data + pos);
      pos += 4;
      JsonValue::Array arr;
      arr.reserve(n);
      for (std::uint32_t i = 0; i < n; ++i)
        arr.push_back(decodeBody(data, size, pos));
      return JsonValue(std::move(arr));
    }
    case JsonType::Object: {
      need(4);
      std::uint32_t n = getU32LE(data + pos);
      pos += 4;
      JsonValue::Object obj;
      for (std::uint32_t i = 0; i < n; ++i) {
        need(4);
        std::uint32_t kl = getU32LE(data + pos);
        pos += 4;
        need(kl);
        std::string k(reinterpret_cast<const char*>(data + pos), kl);
        pos += kl;
        obj[std::move(k)] = decodeBody(data, size, pos);
      }
      return JsonValue(std::move(obj));
    }
  }
  throw JsonError("Unbekannter JSONB-Typ-Tag");
}

std::string scalarRepr(const JsonValue& v) {
  const auto& d = v.data;
  if (auto* s = std::get_if<std::string>(&d)) return *s;
  if (auto* i = std::get_if<std::int64_t>(&d)) return std::to_string(*i);
  if (auto* f = std::get_if<double>(&d)) {
    std::ostringstream o;
    o << *f;
    return o.str();
  }
  if (auto* b = std::get_if<bool>(&d)) return *b ? "true" : "false";
  if (std::holds_alternative<std::monostate>(d)) return "null";
  return jsonToString(v);
}

void collectTokens(const JsonValue& v, std::vector<std::string>& out) {
  const auto& d = v.data;
  if (auto* s = std::get_if<std::string>(&d)) {
    out.push_back("s:" + *s);
  } else if (auto* i = std::get_if<std::int64_t>(&d)) {
    out.push_back("i:" + std::to_string(*i));
  } else if (auto* f = std::get_if<double>(&d)) {
    std::ostringstream o;
    o << *f;
    out.push_back("d:" + o.str());
  } else if (auto* b = std::get_if<bool>(&d)) {
    out.push_back(std::string("b:") + (*b ? "true" : "false"));
  } else if (std::holds_alternative<std::monostate>(d)) {
    out.push_back("n:null");
  } else if (auto* a = std::get_if<JsonValue::Array>(&d)) {
    for (const auto& e : *a) collectTokens(e, out);
  } else if (auto* o = std::get_if<JsonValue::Object>(&d)) {
    for (const auto& [k, val] : *o) {
      out.push_back("k:" + k);
      // Blatt-Skalare zusaetzlich als k=v Pfad-Token (selektiver).
      const auto& vd = val.data;
      bool scalar = std::holds_alternative<std::string>(vd) ||
                    std::holds_alternative<std::int64_t>(vd) ||
                    std::holds_alternative<double>(vd) ||
                    std::holds_alternative<bool>(vd) ||
                    std::holds_alternative<std::monostate>(vd);
      if (scalar) out.push_back("k:" + k + "=" + scalarRepr(val));
      collectTokens(val, out);
    }
  }
}

}  // namespace

JsonType JsonValue::type() const noexcept {
  const auto& d = data;
  if (std::holds_alternative<std::monostate>(d)) return JsonType::Null;
  if (std::holds_alternative<bool>(d)) return JsonType::Bool;
  if (std::holds_alternative<std::int64_t>(d)) return JsonType::Int;
  if (std::holds_alternative<double>(d)) return JsonType::Double;
  if (std::holds_alternative<std::string>(d)) return JsonType::String;
  if (std::holds_alternative<JsonValue::Array>(d)) return JsonType::Array;
  return JsonType::Object;
}

bool jsonEquals(const JsonValue& a, const JsonValue& b) {
  const auto& da = a.data;
  const auto& db = b.data;
  if (da.index() != db.index()) {
    // Int vs Double tolerant vergleichen (1 == 1.0).
    const auto* ai = std::get_if<std::int64_t>(&da);
    const auto* bd = std::get_if<double>(&db);
    if (ai && bd) return static_cast<double>(*ai) == *bd;
    const auto* ad = std::get_if<double>(&da);
    const auto* bi = std::get_if<std::int64_t>(&db);
    if (ad && bi) return *ad == static_cast<double>(*bi);
    return false;
  }
  if (std::holds_alternative<std::monostate>(da)) return true;
  if (auto* x = std::get_if<bool>(&da))
    return *x == std::get<bool>(db);
  if (auto* x = std::get_if<std::int64_t>(&da))
    return *x == std::get<std::int64_t>(db);
  if (auto* x = std::get_if<double>(&da))
    return *x == std::get<double>(db);
  if (auto* x = std::get_if<std::string>(&da))
    return *x == std::get<std::string>(db);
  if (auto* x = std::get_if<JsonValue::Array>(&da)) {
    const auto& y = std::get<JsonValue::Array>(db);
    if (x->size() != y.size()) return false;
    for (std::size_t i = 0; i < x->size(); ++i)
      if (!jsonEquals((*x)[i], y[i])) return false;
    return true;
  }
  if (auto* x = std::get_if<JsonValue::Object>(&da)) {
    const auto& y = std::get<JsonValue::Object>(db);
    if (x->size() != y.size()) return false;
    for (const auto& [k, v] : *x) {
      auto it = y.find(k);
      if (it == y.end() || !jsonEquals(v, it->second)) return false;
    }
    return true;
  }
  return false;
}

std::string jsonToString(const JsonValue& v) {
  const auto& d = v.data;
  if (std::holds_alternative<std::monostate>(d)) return "null";
  if (auto* b = std::get_if<bool>(&d)) return *b ? "true" : "false";
  if (auto* i = std::get_if<std::int64_t>(&d)) return std::to_string(*i);
  if (auto* f = std::get_if<double>(&d)) {
    std::ostringstream o;
    o << *f;
    return o.str();
  }
  if (auto* s = std::get_if<std::string>(&d)) {
    std::string out;
    appendEscaped(out, *s);
    return out;
  }
  if (auto* a = std::get_if<JsonValue::Array>(&d)) {
    std::string out = "[";
    for (std::size_t i = 0; i < a->size(); ++i) {
      if (i) out += ",";
      out += jsonToString((*a)[i]);
    }
    out += "]";
    return out;
  }
  if (auto* o = std::get_if<JsonValue::Object>(&d)) {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, val] : *o) {
      if (!first) out += ",";
      first = false;
      appendEscaped(out, k);
      out += ":";
      out += jsonToString(val);
    }
    out += "}";
    return out;
  }
  return "null";
}

JsonValue parseJson(const std::string& text) {
  Cursor c(text);
  JsonValue v = parseValue(c);
  c.skipWs();
  if (!c.eof()) throw JsonError("Trailing garbage nach JSON-Wert");
  return v;
}

std::vector<std::uint8_t> jsonEncode(const JsonValue& v) {
  std::vector<std::uint8_t> body;
  encodeBody(body, v);
  std::vector<std::uint8_t> out;
  out.reserve(4 + body.size());
  putU32LE(out, static_cast<std::uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

JsonValue jsonDecode(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr && size != 0) throw JsonError("Null-Buffer decode");
  if (size < 4) throw JsonError("JSONB-Buffer zu kurz (Laengenpraefix fehlt)");
  std::uint32_t bodyLen = getU32LE(data);
  if (4 + bodyLen != size)
    throw JsonError("JSONB-Laengenpraefix passt nicht zur Buffergroesse");
  std::size_t pos = 4;
  JsonValue v = decodeBody(data, size, pos);
  if (pos != size) throw JsonError("Trailing bytes in JSONB-Buffer");
  return v;
}

JsonValue jsonDecode(const std::vector<std::uint8_t>& buf) {
  return jsonDecode(buf.data(), buf.size());
}

bool jsonContains(const JsonValue& doc, const JsonValue& query) {
  // Skalar/Null: Gleichheit.
  if (query.type() != JsonType::Object && query.type() != JsonType::Array)
    return jsonEquals(doc, query);
  if (query.type() == JsonType::Array) {
    if (doc.type() != JsonType::Array) return false;
    const auto& qa = std::get<JsonValue::Array>(query.data);
    const auto& da = std::get<JsonValue::Array>(doc.data);
    for (const auto& q : qa) {
      bool found = false;
      for (const auto& d : da) {
        if (q.type() == JsonType::Object || q.type() == JsonType::Array) {
          if (jsonContains(d, q)) {
            found = true;
            break;
          }
        } else if (jsonEquals(d, q)) {
          found = true;
          break;
        }
      }
      if (!found) return false;
    }
    return true;
  }
  // Object query:
  if (doc.type() != JsonType::Object) return false;
  const auto& qo = std::get<JsonValue::Object>(query.data);
  const auto& dob = std::get<JsonValue::Object>(doc.data);
  for (const auto& [k, qv] : qo) {
    auto it = dob.find(k);
    if (it == dob.end()) return false;
    const JsonValue& dv = it->second;
    if (qv.type() == JsonType::Object || qv.type() == JsonType::Array) {
      if (!jsonContains(dv, qv)) return false;
    } else if (!jsonEquals(dv, qv)) {
      return false;
    }
  }
  return true;
}

JsonValue jsonArrow(const JsonValue& doc, const std::string& key) {
  if (doc.type() != JsonType::Object) return JsonValue();
  const auto& o = std::get<JsonValue::Object>(doc.data);
  auto it = o.find(key);
  if (it == o.end()) return JsonValue();
  return it->second;
}

JsonValue jsonArrow(const JsonValue& doc, std::size_t index) {
  if (doc.type() != JsonType::Array) return JsonValue();
  const auto& a = std::get<JsonValue::Array>(doc.data);
  if (index >= a.size()) return JsonValue();
  return a[index];
}

std::string jsonArrowText(const JsonValue& doc, const std::string& key) {
  JsonValue v = jsonArrow(doc, key);
  if (v.type() == JsonType::String) return std::get<std::string>(v.data);
  return jsonToString(v);
}

std::string jsonArrowText(const JsonValue& doc, std::size_t index) {
  JsonValue v = jsonArrow(doc, index);
  if (v.type() == JsonType::String)
    return std::get<std::string>(v.data);
  return jsonToString(v);
}

std::vector<std::string> jsonTokens(const JsonValue& v) {
  std::vector<std::string> out;
  collectTokens(v, out);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

void GinIndex::addDoc(int docId, const JsonValue& doc) {
  for (const auto& t : jsonTokens(doc)) {
    auto& vec = postings_[t];
    if (std::find(vec.begin(), vec.end(), docId) == vec.end()) {
      vec.push_back(docId);
      std::sort(vec.begin(), vec.end());
    }
  }
}

bool GinIndex::removeDoc(int docId, const JsonValue& doc) {
  bool removed = false;
  for (const auto& t : jsonTokens(doc)) {
    auto it = postings_.find(t);
    if (it == postings_.end()) continue;
    auto& vec = it->second;
    auto vit = std::find(vec.begin(), vec.end(), docId);
    if (vit != vec.end()) {
      vec.erase(vit);
      removed = true;
      if (vec.empty()) postings_.erase(it);
    }
  }
  return removed;
}

void GinIndex::clear() { postings_.clear(); }

std::vector<int> GinIndex::lookup(const std::string& token) const {
  auto it = postings_.find(token);
  if (it == postings_.end()) return {};
  return it->second;  // bereits sortiert + dedup
}

std::vector<int> GinIndex::lookupAnd(
    const std::vector<std::string>& tokens) const {
  if (tokens.empty()) return {};
  std::vector<int> acc = lookup(tokens[0]);
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    std::vector<int> cur = lookup(tokens[i]);
    std::vector<int> next;
    std::set_intersection(acc.begin(), acc.end(), cur.begin(), cur.end(),
                          std::back_inserter(next));
    acc.swap(next);
    if (acc.empty()) break;
  }
  return acc;
}

std::vector<int> GinIndex::lookupOr(
    const std::vector<std::string>& tokens) const {
  std::vector<int> acc;
  for (const auto& t : tokens) {
    std::vector<int> cur = lookup(t);
    std::vector<int> next;
    std::set_union(acc.begin(), acc.end(), cur.begin(), cur.end(),
                   std::back_inserter(next));
    acc.swap(next);
  }
  return acc;
}

std::size_t GinIndex::tokenCount() const { return postings_.size(); }

std::size_t GinIndex::entryCount() const {
  std::size_t n = 0;
  for (const auto& [k, v] : postings_) n += v.size();
  return n;
}

}  // namespace dbengine::sql
