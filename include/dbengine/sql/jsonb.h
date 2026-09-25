#pragma once

// s15-jsonb: Binaer-JSON Typ, GIN-Inverted-Index, Operatoren ->/->>/@>.
// Scope: simples JSON (kein Streaming), Binär-Encode (Laenge+Typ-Tag),
// GIN als token->docIDs Multimap, SELECT-Filter-Simulation via contains().
// Fehler via JsonError (std::runtime_error). Header-only Typen, Logik in
// src/sql/jsonb.cpp. C++20, ohne externe Deps.

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace dbengine::sql {

class JsonError : public std::runtime_error {
 public:
  explicit JsonError(const std::string& msg) : std::runtime_error(msg) {}
};

enum class JsonType : std::uint8_t {
  Null = 0,
  Bool = 1,
  Int = 2,
  Double = 3,
  String = 4,
  Array = 5,
  Object = 6
};

struct JsonValue {
  using Array = std::vector<JsonValue>;
  using Object = std::map<std::string, JsonValue>;
  using Storage =
      std::variant<std::monostate, bool, std::int64_t, double, std::string,
                   Array, Object>;

  Storage data{std::monostate{}};

  JsonValue() = default;
  JsonValue(std::nullptr_t) : data(std::monostate{}) {}
  JsonValue(bool b) : data(b) {}
  JsonValue(int v) : data(static_cast<std::int64_t>(v)) {}
  JsonValue(std::int64_t v) : data(v) {}
  JsonValue(double v) : data(v) {}
  JsonValue(const char* s) : data(std::string(s)) {}
  JsonValue(std::string s) : data(std::move(s)) {}
  JsonValue(Array a) : data(std::move(a)) {}
  JsonValue(Object o) : data(std::move(o)) {}

  [[nodiscard]] JsonType type() const noexcept;
  [[nodiscard]] bool isNull() const noexcept {
    return std::holds_alternative<std::monostate>(data);
  }
};

[[nodiscard]] bool jsonEquals(const JsonValue& a, const JsonValue& b);
[[nodiscard]] std::string jsonToString(const JsonValue& v);

// Parser fuer simples JSON (RFC 8259 Subset): Objekte, Arrays, Strings mit
// Standard-Escapes (\" \\ \/ \b \f \n \r \t \uXXXX BMP), Zahlen
// (int64 wenn ganzzahlig, sonst double), true/false/null. Whitespace wird
// uebersprungen; trailing garbage wirft JsonError.
[[nodiscard]] JsonValue parseJson(const std::string& text);

// Binaer-Encode: [u32 LE Gesamtlaenge des Bodys][Body].
// Body pro Wert: 1 Byte JsonType-Tag, dann Payload:
//   Null:  -
//   Bool:  1 Byte (0/1)
//   Int:   8 Byte LE (two's complement)
//   Double: 8 Byte IEEE754 LE (memcpy)
//   String: u32 LE len + bytes
//   Array:  u32 LE count + Elemente rekursiv
//   Object: u32 LE count + je (u32 LE klen + key bytes + Wert rekursiv)
// Keys sind sortiert (std::map), daher kanonisch.
[[nodiscard]] std::vector<std::uint8_t> jsonEncode(const JsonValue& v);
[[nodiscard]] JsonValue jsonDecode(const std::vector<std::uint8_t>& buf);
// Ueberladung mit Offset/Laenge fuer eingebettete Nutzung.
[[nodiscard]] JsonValue jsonDecode(const std::uint8_t* data, std::size_t size);

// Operatoren (Postgres-Semantik, vereinfacht):
//   contains(doc, query)  ~  doc @> query: query ist Teilmenge von doc.
//     - Skalare: jsonEquals
//     - Array query: jedes q-Element muss in doc-Array enthalten sein
//       (Reihenfolge egal, contains-rekursiv)
//     - Object query: jeder q-Key muss in doc existieren und
//       contains(doc[key], q[key]) erfuellen
//     - Typmismatch -> false (kein Throw)
//   arrow(doc, key)       ~  doc -> 'key': Objekt-Member oder Null.
//   arrow(doc, index)     ~  doc -> n: Array-Element oder Null.
//   arrowText(doc, ...)   ~  doc ->> 'key': Skalar als Text, sonst JSON-Text.
[[nodiscard]] bool jsonContains(const JsonValue& doc, const JsonValue& query);
[[nodiscard]] JsonValue jsonArrow(const JsonValue& doc, const std::string& key);
[[nodiscard]] JsonValue jsonArrow(const JsonValue& doc, std::size_t index);
[[nodiscard]] std::string jsonArrowText(const JsonValue& doc,
                                        const std::string& key);
[[nodiscard]] std::string jsonArrowText(const JsonValue& doc,
                                        std::size_t index);

// GIN (Generalized Inverted Index): token -> docIDs Multimap.
// Token-Schema (flach, rekursiv extrahiert):
//   Objekt-Key Existenz:  "k:<key>"
//   String-Wert:          "s:<value>"
//   Int-Wert:             "i:<n>"
//   Double-Wert:          "d:<repr>"
//   Bool-Wert:            "b:true|false"
//   Null-Vorkommen:       "n:null"
//   Key=Skalar-Pfad:      "k:<key>=<scalarRepr>" (nur skalare Blattwerte)
// lookup(token) liefert sortierte, deduplizierte docIDs.
// lookupAnd(tokens) = Schnittmenge (AND), lookupOr = Vereinigung (OR).
[[nodiscard]] std::vector<std::string> jsonTokens(const JsonValue& v);

class GinIndex {
 public:
  GinIndex() = default;

  void addDoc(int docId, const JsonValue& doc);
  // true wenn mindestens ein Token-Eintrag entfernt wurde.
  bool removeDoc(int docId, const JsonValue& doc);
  void clear();

  [[nodiscard]] std::vector<int> lookup(const std::string& token) const;
  [[nodiscard]] std::vector<int> lookupAnd(
      const std::vector<std::string>& tokens) const;
  [[nodiscard]] std::vector<int> lookupOr(
      const std::vector<std::string>& tokens) const;
  [[nodiscard]] std::size_t tokenCount() const;
  [[nodiscard]] std::size_t entryCount() const;

 private:
  // token -> docIDs (multimap-Semantik via vector pro Token).
  std::map<std::string, std::vector<int>> postings_;
};

}  // namespace dbengine::sql
