// s15-jsonb Tests: Roundtrip, GIN-Lookup, SELECT-Filter-Simulation.
// Framework-frei (assert-light via CHECK, CTest-kompatibel).

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "dbengine/sql/jsonb.h"

using dbengine::sql::GinIndex;
using dbengine::sql::JsonError;
using dbengine::sql::JsonValue;

static int g_fail = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";                     \
      ++g_fail;                                                                \
    }                                                                          \
  } while (0)

static void test_parse_and_types() {
  JsonValue v = dbengine::sql::parseJson(
      R"({"n":null,"b":true,"i":42,"f":1.5,"s":"hi","a":[1,2],"o":{"x":1}})");
  CHECK(v.type() == dbengine::sql::JsonType::Object);
  CHECK(dbengine::sql::jsonArrow(v, "n").isNull());
  CHECK(std::get<bool>(dbengine::sql::jsonArrow(v, "b").data) == true);
  CHECK(std::get<std::int64_t>(dbengine::sql::jsonArrow(v, "i").data) == 42);
  CHECK(dbengine::sql::jsonArrowText(v, "s") == "hi");
  CHECK(dbengine::sql::jsonArrowText(v, "i") == "42");
  // Escapes + \uXXXX
  JsonValue e = dbengine::sql::parseJson(R"({"q":"a\nb\"c\u0041"})");
  CHECK(dbengine::sql::jsonArrowText(e, "q") == std::string("a\nb\"cA"));
  // Fehlerfaelle
  bool threw = false;
  try {
    (void)dbengine::sql::parseJson("{bad}");
  } catch (const JsonError&) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    (void)dbengine::sql::parseJson("[1,]");
  } catch (const JsonError&) {
    threw = true;
  }
  CHECK(threw);
  std::cout << "[ok] parse_and_types\n";
}

static void test_roundtrip() {
  const std::vector<std::string> docs = {
      "null", "true", "false", "0", "-17", "3.25", "\"hello\"",
      "[]", "[1,\"a\",null,true]", "{}",
      R"({"name":"joe","age":30,"tags":["db","json"],"addr":{"city":"ulm"}})",
  };
  for (const auto& s : docs) {
    JsonValue v = dbengine::sql::parseJson(s);
    std::vector<std::uint8_t> bin = dbengine::sql::jsonEncode(v);
    // Laengenpraefix prüfen: u32 LE == body size
    CHECK(bin.size() >= 4);
    std::uint32_t bodyLen = static_cast<std::uint32_t>(bin[0]) |
                            (static_cast<std::uint32_t>(bin[1]) << 8) |
                            (static_cast<std::uint32_t>(bin[2]) << 16) |
                            (static_cast<std::uint32_t>(bin[3]) << 24);
    CHECK(4 + bodyLen == bin.size());
    JsonValue back = dbengine::sql::jsonDecode(bin);
    CHECK(dbengine::sql::jsonEquals(v, back));
    // Text-Stabilitaet: re-parse des serialisierten Strings
    JsonValue v2 =
        dbengine::sql::parseJson(dbengine::sql::jsonToString(back));
    CHECK(dbengine::sql::jsonEquals(v, v2));
  }
  // Trunkierter Buffer muss werfen
  JsonValue v = dbengine::sql::parseJson(R"({"a":1})");
  auto bin = dbengine::sql::jsonEncode(v);
  bin.pop_back();
  bool threw = false;
  try {
    (void)dbengine::sql::jsonDecode(bin);
  } catch (const JsonError&) {
    threw = true;
  }
  CHECK(threw);
  std::cout << "[ok] roundtrip\n";
}

static void test_ops_contains_arrow() {
  JsonValue doc = dbengine::sql::parseJson(
      R"({"name":"joe","age":30,"tags":["db","json","x"],"addr":{"city":"ulm","zip":89073}})");
  // @> positiv
  CHECK(dbengine::sql::jsonContains(
      doc, dbengine::sql::parseJson(R"({"name":"joe"})")));
  CHECK(dbengine::sql::jsonContains(
      doc, dbengine::sql::parseJson(R"({"addr":{"city":"ulm"}})")));
  CHECK(dbengine::sql::jsonContains(
      doc, dbengine::sql::parseJson(R"({"tags":["db","json"]})")));
  // @> negativ
  CHECK(!dbengine::sql::jsonContains(
      doc, dbengine::sql::parseJson(R"({"name":"bob"})")));
  CHECK(!dbengine::sql::jsonContains(
      doc, dbengine::sql::parseJson(R"({"missing":1})")));
  CHECK(!dbengine::sql::jsonContains(
      doc, dbengine::sql::parseJson(R"({"tags":["db","nope"]})")));
  // -> / ->>
  CHECK(dbengine::sql::jsonArrowText(doc, "name") == "joe");
  CHECK(dbengine::sql::jsonArrowText(doc, "age") == "30");
  JsonValue addr = dbengine::sql::jsonArrow(doc, "addr");
  CHECK(dbengine::sql::jsonArrowText(addr, "city") == "ulm");
  JsonValue tags = dbengine::sql::jsonArrow(doc, "tags");
  CHECK(dbengine::sql::jsonArrowText(tags, 0) == "db");
  CHECK(dbengine::sql::jsonArrowText(tags, 99) == "null");  // OOB -> null
  CHECK(dbengine::sql::jsonArrow(doc, "nope").isNull());
  std::cout << "[ok] ops_contains_arrow\n";
}

static void test_gin_lookup() {
  GinIndex gin;
  JsonValue d0 =
      dbengine::sql::parseJson(R"({"city":"ulm","tag":"db"})");
  JsonValue d1 =
      dbengine::sql::parseJson(R"({"city":"berlin","tag":"db"})");
  JsonValue d2 =
      dbengine::sql::parseJson(R"({"city":"ulm","tag":"vec"})");
  gin.addDoc(0, d0);
  gin.addDoc(1, d1);
  gin.addDoc(2, d2);

  auto ulm = gin.lookup("k:city=ulm");
  CHECK(ulm.size() == 2 && ulm[0] == 0 && ulm[1] == 2);
  auto db = gin.lookup("s:db");
  CHECK(db.size() == 2 && db[0] == 0 && db[1] == 1);
  // AND-Schnitt: ulm + db -> nur doc 0
  auto both = gin.lookupAnd({"k:city=ulm", "s:db"});
  CHECK(both.size() == 1 && both[0] == 0);
  // OR-Vereinigung
  auto either = gin.lookupOr({"k:city=berlin", "s:vec"});
  CHECK(either.size() == 2 && either[0] == 1 && either[1] == 2);
  // Miss
  CHECK(gin.lookup("k:nosuchkey").empty());
  // removeDoc
  CHECK(gin.removeDoc(0, d0) == true);
  CHECK(gin.lookup("k:city=ulm").size() == 1);
  CHECK(gin.removeDoc(0, d0) == false);  // doppelt -> false
  std::cout << "[ok] gin_lookup\n";
}

static void test_select_filter_sim() {
  // Simuliertes SELECT ... WHERE payload @> '{"city":"ulm"}':
  // docs-Tabelle (id, payload), Full-Scan vs GIN-Kandidaten muessen
  // identische Ergebnismenge liefern.
  struct Row {
    int id;
    JsonValue payload;
  };
  std::vector<Row> table = {
      {0, dbengine::sql::parseJson(R"({"city":"ulm","n":1})")},
      {1, dbengine::sql::parseJson(R"({"city":"berlin","n":2})")},
      {2, dbengine::sql::parseJson(R"({"city":"ulm","n":3})")},
      {3, dbengine::sql::parseJson(R"({"n":4})")},
  };
  JsonValue q = dbengine::sql::parseJson(R"({"city":"ulm"})");

  // 1) Full-Scan via contains (@>)
  std::vector<int> expected;
  for (const auto& r : table)
    if (dbengine::sql::jsonContains(r.payload, q)) expected.push_back(r.id);
  CHECK(expected.size() == 2 && expected[0] == 0 && expected[1] == 2);

  // 2) GIN-beschleunigt: Kandidaten via Token, dann contains verifizieren
  GinIndex gin;
  for (const auto& r : table) gin.addDoc(r.id, r.payload);
  std::vector<int> cand = gin.lookup("k:city=ulm");
  std::vector<int> via_gin;
  for (int id : cand) {
    const Row* rp = nullptr;
    for (const auto& r : table)
      if (r.id == id) rp = &r;
    assert(rp != nullptr);
    if (dbengine::sql::jsonContains(rp->payload, q)) via_gin.push_back(id);
  }
  CHECK(via_gin == expected);

  // 3) Arrow-Projektion: SELECT payload->>'city'
  std::vector<std::string> cities;
  for (int id : expected) {
    for (const auto& r : table)
      if (r.id == id)
        cities.push_back(dbengine::sql::jsonArrowText(r.payload, "city"));
  }
  CHECK(cities.size() == 2 && cities[0] == "ulm" && cities[1] == "ulm");
  std::cout << "[ok] select_filter_sim\n";
}

int main() {
  test_parse_and_types();
  test_roundtrip();
  test_ops_contains_arrow();
  test_gin_lookup();
  test_select_filter_sim();
  if (g_fail == 0) {
    std::cout << "jsonb tests: all passed\n";
    return 0;
  }
  std::cerr << "jsonb tests: " << g_fail << " FAILURES\n";
  return 1;
}
