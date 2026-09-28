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

  // ---- s127: CASE-WHEN in Aggregat-Argumenten (q12/q14-Kerne) ----
  auto rc = db.execute("CREATE TABLE c (p TEXT, x DOUBLE)");
  CHECK(rc.message == "CREATE TABLE");
  auto rci = db.execute(
      "INSERT INTO c VALUES ('1-URGENT', 10), ('2-HIGH', 20), ('3-LOW', 30)");
  CHECK(rci.affected == 3);
  // OR im WHEN, THEN 1 ELSE 0 (q12-Kern).
  auto rq12 = db.execute(
      "SELECT SUM(CASE WHEN p = '1-URGENT' OR p = '2-HIGH' THEN 1 ELSE 0 END) "
      "AS hi FROM c");
  CHECK(rq12.rows.size() == 1);
  CHECK(valueToString(rq12.rows[0][0]) == "2");
  auto rq12b = db.execute(
      "SELECT SUM(CASE WHEN p <> '1-URGENT' AND p <> '2-HIGH' THEN 1 ELSE 0 END) "
      "AS lo FROM c");
  CHECK(valueToString(rq12b.rows[0][0]) == "1");
  // LIKE im WHEN + Spalten-THEN (q14-Kern).
  auto rq14 = db.execute(
      "SELECT SUM(CASE WHEN p LIKE '1-%' THEN x ELSE 0 END) AS s FROM c");
  CHECK(valueToString(rq14.rows[0][0]) == "10");
  // Ohne ELSE -> NULL (SUM skippt NULL).
  auto rqN = db.execute("SELECT SUM(CASE WHEN p = 'zzz' THEN x END) AS s FROM c");
  CHECK(valueToString(rqN.rows[0][0]) == "NULL");
  // Fehler: CASE ohne WHEN / ohne END werfen laut.
  for (const char* bad :
       {"SELECT SUM(CASE x END) FROM c", "SELECT SUM(CASE WHEN p = 'a' THEN 1 FROM c"}) {
    bool t = false;
    try {
      db.execute(bad);
    } catch (...) {
      t = true;
    }
    CHECK(t);
  }
  // Komma-Join muss LAUT scheitern (frueher: nur erste Tabelle gelesen,
  // Rest inkl. WHERE still verworfen).
  bool threw = false;
  try {
    db.execute("SELECT a FROM t, docs WHERE a = 1");
  } catch (...) {
    threw = true;
  }
  CHECK(threw);
  // ---- s128: EXISTS / NOT EXISTS (korreliert per Semi-Join) ----
  auto re = db.execute("CREATE TABLE o (id INT)");
  CHECK(re.message == "CREATE TABLE");
  auto rl = db.execute("CREATE TABLE l (oid INT, x INT)");
  CHECK(rl.message == "CREATE TABLE");
  auto rei = db.execute("INSERT INTO o VALUES (1), (2), (3)");
  CHECK(rei.affected == 3);
  auto rli = db.execute("INSERT INTO l VALUES (1, 10), (1, 20), (3, 30)");
  CHECK(rli.affected == 3);
  auto rex = db.execute("SELECT id FROM o WHERE EXISTS (SELECT * FROM l WHERE oid = id)");
  CHECK(rex.rows.size() == 2);
  CHECK(valueToString(rex.rows[0][0]) == "1");
  CHECK(valueToString(rex.rows[1][0]) == "3");
  auto rnx = db.execute("SELECT id FROM o WHERE NOT EXISTS (SELECT * FROM l WHERE oid = id)");
  CHECK(rnx.rows.size() == 1);
  CHECK(valueToString(rnx.rows[0][0]) == "2");
  // Unkorreliert: leer -> keine Zeilen; voll -> alle.
  auto rue = db.execute("SELECT id FROM o WHERE EXISTS (SELECT * FROM l WHERE x > 100)");
  CHECK(rue.rows.empty());
  auto ruf = db.execute("SELECT id FROM o WHERE EXISTS (SELECT * FROM l WHERE x > 5)");
  CHECK(ruf.rows.size() == 3);
  // Inner-inner Spaltenvergleich als Rest-Bedingung.
  auto rum = db.execute(
      "SELECT id FROM o WHERE EXISTS (SELECT * FROM l WHERE x > 5 AND oid = oid)");
  CHECK(rum.rows.size() == 3);
  // Laut statt still: nicht-equi Korrelation, OR-Korrelation, JOIN innen.
  for (const char* bad :
       {"SELECT id FROM o WHERE EXISTS (SELECT * FROM l WHERE oid > id)",
        "SELECT id FROM o WHERE EXISTS (SELECT * FROM l WHERE oid = id OR x > 1)",
        "SELECT id FROM o WHERE EXISTS (SELECT * FROM l JOIN o ON oid = id)"}) {
    bool t = false;
    try {
      db.execute(bad);
    } catch (...) {
      t = true;
    }
    CHECK(t);
  }
  // Trailing-Garbage nach gueltigem SELECT muss werfen.
  threw = false;
  try {
    db.execute("SELECT * FROM t WHERE a = 1 GARBAGE");
  } catch (...) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    db.execute("SELECT * FROM t UNION SELECT * FROM t");
  } catch (...) {
    threw = true;
  }
  CHECK(threw);
  // Gueltige Statements weiter ok (EOF-Check greift nicht bei Subqueries).
  auto rok = db.execute("SELECT a FROM t WHERE a IN (SELECT a FROM t WHERE a > 1)");
  CHECK(rok.rows.size() == 1);
  auto rd = db.execute("CREATE TABLE d (d INT, x DOUBLE)");
  CHECK(rd.message == "CREATE TABLE");
  auto rdi = db.execute(
      "INSERT INTO d VALUES (19970101, 0.06), (19980101, 0.05), (19961231, 0.07)");
  CHECK(rdi.affected == 3);
  // date-Literal -> INT, Intervall +1 Jahr, BETWEEN-Arithmetik.
  auto rq = db.execute(
      "SELECT d FROM d WHERE d >= date '1997-01-01' AND d < date '1997-01-01' "
      "+ interval '1' year AND x BETWEEN 0.06 - 0.01 AND 0.06 + 0.01 ORDER BY d");
  CHECK(rq.rows.size() == 1);
  CHECK(valueToString(rq.rows[0][0]) == "19970101");
  // -91 Tage Intervall + Monats-Clamping (Jan31 + 1 Monat = Feb28/29).
  auto rq2 = db.execute(
      "SELECT d FROM d WHERE d >= date '1998-01-01' - interval '91' day");
  CHECK(rq2.rows.size() == 1);  // nur 19980101 (19971002 waere Schwelle)
  auto rq3 = db.execute(
      "SELECT d FROM d WHERE d < date '1997-02-28' + interval '3' month");
  CHECK(rq3.rows.size() == 2);  // 19961231 + 19970101 (< 19970528)
  // Fehlerfaelle: falsches Datum, unbekannte Einheit, Intervall ohne Datum.
  threw = false;
  try {
    db.execute("SELECT d FROM d WHERE d = date '1997-13-01'");
  } catch (...) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    db.execute("SELECT d FROM d WHERE d = date '1997-01-01' + interval '1' eon");
  } catch (...) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    db.execute("SELECT d FROM d WHERE d = 'x' + interval '1' year");
  } catch (...) {
    threw = true;
  }
  CHECK(threw);
  // IN-Liste mit Arithmetik.
  auto rq4 = db.execute("SELECT d FROM d WHERE d IN (19970101, 19961231 + 0)");
  CHECK(rq4.rows.size() == 2);
  // Monats-Clamping: Jan31 + 13 Monate = Feb28 Folgejahr (PG-Semantik).
  auto rq5 = db.execute(
      "SELECT d FROM d WHERE d = date '1997-01-31' + interval '13' month");
  CHECK(rq5.rows.empty());  // 19980228 nicht in Testdaten
  auto rqi = db.execute("INSERT INTO d VALUES (19980228, 0.01)");
  CHECK(rqi.affected == 1);
  rq5 = db.execute(
      "SELECT d FROM d WHERE d = date '1997-01-31' + interval '13' month");
  CHECK(rq5.rows.size() == 1);

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
