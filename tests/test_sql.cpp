// s06-sql Tests: 3 Statement-Arten end-to-end (CREATE/INSERT/SELECT)
// plus WHERE a=1, WHERE b LIKE, COUNT(*) und PGWire-Stub (Startup/Q/Error).
// Kein gtest (reine asserts, CTest-kompatibel: Rueckgabecode != 0 bei Fehler).

#include <cassert>
#include <iostream>
#include <string>

#include "dbengine/server/pgwire.h"
#include "dbengine/sql/parser.h"

using dbengine::sql::Database;
using dbengine::sql::valueToString;

static int g_fail = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";                   \
      ++g_fail;                                                              \
    }                                                                        \
  } while (0)

int main() {
  Database db;

  // 1) CREATE TABLE (PG-Typen, Mixed-Case wie libpq sie schickt)
  auto r0 = db.execute("CREATE TABLE t (a INT, b TEXT)");
  CHECK(r0.message == "CREATE TABLE");

  // IF NOT EXISTS idempotent
  auto r0b = db.execute("CREATE TABLE IF NOT EXISTS t (a INT, b TEXT)");
  CHECK(r0b.message == "TABLE EXISTS t");

  // JSONB-Stub-Tabelle (F1.1)
  auto rj = db.execute("CREATE TABLE docs (id INT, payload JSONB)");
  CHECK(rj.message == "CREATE TABLE");
  auto rji = db.execute("INSERT INTO docs VALUES (1, '{\"a\": 1}')");
  CHECK(rji.affected == 1);
  auto rjs = db.execute("SELECT payload FROM docs WHERE id = 1");
  CHECK(rjs.rows.size() == 1);
  CHECK(valueToString(rjs.rows[0][0]) == "{\"a\": 1}");

  // 2) INSERT (2 Tupel, ein Statement)
  auto r1 = db.execute("INSERT INTO t VALUES (1, 'hello'), (2, 'world')");
  CHECK(r1.affected == 2);
  CHECK(r1.message == "INSERT 0 2");

  // 3a) SELECT mit Gleichheits-Filter (PK-Lookup)
  auto r2 = db.execute("SELECT * FROM t WHERE a = 1");
  CHECK(r2.columns.size() == 2);
  CHECK(r2.columns[0] == "a" && r2.columns[1] == "b");
  CHECK(r2.rows.size() == 1);
  CHECK(valueToString(r2.rows[0][0]) == "1");
  CHECK(valueToString(r2.rows[0][1]) == "hello");

  // 3b) SELECT mit LIKE-Filter
  auto r3 = db.execute("SELECT * FROM t WHERE b LIKE 'wor%'");
  CHECK(r3.rows.size() == 1);
  CHECK(valueToString(r3.rows[0][0]) == "2");
  CHECK(valueToString(r3.rows[0][1]) == "world");

  // LIKE ohne Treffer
  auto r3b = db.execute("SELECT * FROM t WHERE b LIKE 'zzz%'");
  CHECK(r3b.rows.empty());

  // Vergleichsoperator + Projektionsliste + COUNT(*)
  auto r4 = db.execute("SELECT b FROM t WHERE a > 1");
  CHECK(r4.columns.size() == 1 && r4.columns[0] == "b");
  CHECK(r4.rows.size() == 1 && valueToString(r4.rows[0][0]) == "world");
  auto r5 = db.execute("SELECT COUNT(*) FROM t");
  CHECK(r5.rows.size() == 1 && valueToString(r5.rows[0][0]) == "2");

  // Case-Insensitivitaet (PG-Folding)
  auto r6 = db.execute("SeLeCT * FrOm T WhErE A = 2");
  CHECK(r6.rows.size() == 1);

  // ---- PGWire-Stub ----
  using namespace dbengine::pgwire;
  // Startup roundtrip
  auto startup =
      encodeStartupRequest({{"user", "joe"}, {"database", "dbengine"}});
  StartupParams sp = parseStartup(startup);
  CHECK(sp.user == "joe");
  CHECK(sp.database == "dbengine");

  // Q-Message roundtrip: 'Q' + len + "SELECT 1\0"
  std::string q = "SELECT * FROM t WHERE a = 1";
  std::vector<uint8_t> qp;
  qp.push_back('Q');
  int32_t qlen = (int32_t)(q.size() + 1 + 4);
  putInt32BE(qp, qlen);
  qp.insert(qp.end(), q.begin(), q.end());
  qp.push_back(0);
  auto parsed = parseQueryMessage(qp);
  CHECK(parsed.has_value() && *parsed == q);

  // Nicht-Q -> nullopt
  std::vector<uint8_t> other = {'P', 0, 0, 0, 5, 'x'};
  CHECK(!parseQueryMessage(other).has_value());

  // ErrorResponse beginnt mit 'E' und enthaelt Code+Message
  auto err = encodeError("ERROR", "42601", "syntax error");
  CHECK(!err.empty() && err[0] == 'E');
  std::string blob((char*)err.data(), err.size());
  CHECK(blob.find("42601") != std::string::npos);
  CHECK(blob.find("syntax error") != std::string::npos);

  // ReadyForQuery / CommandComplete Tags
  auto rfq = encodeReadyForQuery();
  CHECK(rfq.size() == 6 && rfq[0] == 'Z');
  auto cc = encodeCommandComplete("SELECT 1");
  CHECK(cc[0] == 'C');

  if (g_fail == 0) {
    std::cout << "sql tests: all passed\n";
    return 0;
  }
  std::cerr << "sql tests: " << g_fail << " FAILURES\n";
  return 1;
}
