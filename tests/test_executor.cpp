// s11-executor Tests: CREATE + INSERT 100 + Restart-Replay + SELECT WHERE.
// Framework-los (CTest-kompatibel, Rueckgabecode != 0 bei Fehler).

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "dbengine/kv.h"
#include "dbengine/sql/executor.h"
#include "dbengine/sql/parser.h"
#include "dbengine/storage/wal.h"
#include "dbengine/txn/mvcc.h"

using dbengine::sql::Executor;
using dbengine::sql::valueToString;

static int g_fail = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";                 \
      ++g_fail;                                                            \
    }                                                                      \
  } while (0)

static std::string tmpWal() {
  auto p = std::filesystem::temp_directory_path() / "dbengine_test_executor.wal";
  std::error_code ec;
  std::filesystem::remove(p, ec);
  return p.string();
}

int main() {
  const std::string path = tmpWal();

  // ---- Phase 1: frisch, CREATE + INSERT 100 --------------------------------
  {
    dbengine::kv::KVStore kv;
    dbengine::txn::MvccStore mvcc;
    dbengine::storage::Wal wal(path);
    wal.open();
    Executor ex(kv, mvcc, &wal);

    auto r0 = ex.execute("CREATE TABLE users (id INT, name TEXT, score DOUBLE)");
    CHECK(r0.message == "CREATE TABLE");
    CHECK(ex.hasTable("users"));
    CHECK(ex.hasTable("USERS"));  // PG-Folding

    // 100 Zeilen in einem Multi-Tuple-INSERT (id 0..99).
    std::string ins = "INSERT INTO users VALUES ";
    for (int i = 0; i < 100; ++i) {
      if (i) ins += ", ";
      ins += "(" + std::to_string(i) + ", 'user" + std::to_string(i) + "', " +
             std::to_string(i * 1.5) + ")";
    }
    auto r1 = ex.execute(ins);
    CHECK(r1.affected == 100);

    // KV-Keys unter sql/users/ ?
    auto keys = kv.Scan("sql/users/");
    CHECK(keys.size() == 100);
    CHECK(kv.Get("sql/users/42").has_value());

    // SELECT COUNT(*) + WHERE-Filter.
    auto c = ex.execute("SELECT COUNT(*) FROM users");
    CHECK(c.rows.size() == 1);
    CHECK(valueToString(c.rows[0][0]) == "100");

    auto one = ex.execute("SELECT * FROM users WHERE id = 42");
    CHECK(one.rows.size() == 1);
    CHECK(valueToString(one.rows[0][0]) == "42");
    CHECK(valueToString(one.rows[0][1]) == "user42");

    auto range = ex.execute("SELECT id FROM users WHERE score > 140.0");
    CHECK(range.rows.size() == 6);  // 94..99

    auto like = ex.execute("SELECT id FROM users WHERE name LIKE 'user4%'");
    CHECK(like.rows.size() == 11);  // user4 + user40..49

    auto cnt2 = ex.execute("SELECT COUNT(*) FROM users WHERE id >= 50");
    CHECK(valueToString(cnt2.rows[0][0]) == "50");

    wal.flush();
    wal.close();
  }

  // ---- Phase 2: Restart (leere Stores, gleiches WAL) + Replay ---------------
  {
    dbengine::kv::KVStore kv2;
    dbengine::txn::MvccStore mvcc2;
    dbengine::storage::Wal wal2(path);
    wal2.open();
    Executor ex2(kv2, mvcc2, &wal2);
    ex2.recover();

    CHECK(ex2.hasTable("users"));
    auto keys = kv2.Scan("sql/users/");
    CHECK(keys.size() == 100);

    auto c = ex2.execute("SELECT COUNT(*) FROM users");
    CHECK(c.rows.size() == 1);
    CHECK(valueToString(c.rows[0][0]) == "100");

    auto one = ex2.execute("SELECT * FROM users WHERE id = 42");
    CHECK(one.rows.size() == 1);
    CHECK(valueToString(one.rows[0][0]) == "42");
    CHECK(valueToString(one.rows[0][1]) == "user42");

    auto range = ex2.execute("SELECT id FROM users WHERE score > 140.0");
    CHECK(range.rows.size() == 6);

    auto like = ex2.execute("SELECT id FROM users WHERE name LIKE 'user4%'");
    CHECK(like.rows.size() == 11);

    // Snapshot-Read: MVCC sieht replayte Zeilen.
    {
      auto rtxn = mvcc2.BeginRead();
      auto v = mvcc2.Read(rtxn, "sql/users/7");
      CHECK(v.has_value());
      mvcc2.Commit(rtxn);
    }

    // Weiter schreiben nach Restart funktioniert (RowID-Counter intakt).
    auto ri = ex2.execute("INSERT INTO users VALUES (100, 'user100', 150.0)");
    CHECK(ri.affected == 1);
    auto c2 = ex2.execute("SELECT COUNT(*) FROM users");
    CHECK(valueToString(c2.rows[0][0]) == "101");

    wal2.flush();
    wal2.close();
  }

  // ---- s128: EXISTS ueber Executor (Pushdown-Pfad mit Masken) --------------
  // TEXT-Spalten in EXISTS duerfen nicht NULL-dekodiert werden (stille 0).
  {
    dbengine::kv::KVStore kv;
    dbengine::txn::MvccStore mvcc;
    Executor ex(kv, mvcc, nullptr);
    CHECK(ex.execute("CREATE TABLE o (id INT)").message == "CREATE TABLE");
    CHECK(ex.execute("CREATE TABLE l (oid INT, tag TEXT)").message ==
          "CREATE TABLE");
    CHECK(ex.execute("INSERT INTO o VALUES (1), (2), (3)").affected == 3);
    CHECK(ex.execute("INSERT INTO l VALUES (1, 'a'), (3, 'b')").affected == 2);
    // TEXT-Praedikat innen (deckt needMarkAll fuer innere Tabellen ab).
    auto r1 = ex.execute(
        "SELECT id FROM o WHERE EXISTS (SELECT * FROM l WHERE oid = id AND "
        "tag = 'a')");
    CHECK(r1.rows.size() == 1);
    CHECK(valueToString(r1.rows[0][0]) == "1");
    // Spalten-RHS im Rest (inner-inner TEXT-Vergleich).
    auto r2 = ex.execute(
        "SELECT id FROM o WHERE EXISTS (SELECT * FROM l WHERE oid = id AND "
        "tag = tag)");
    CHECK(r2.rows.size() == 2);
  }

  // ---- s128: Spalten-RHS ueber Executor (Pushdown markiert value_col) -----
  // Ohne Markierung wuerde die RHS-TEXT-Spalte NULL-dekodiert (stille 0 Rows).
  {
    dbengine::kv::KVStore kv;
    dbengine::txn::MvccStore mvcc;
    Executor ex(kv, mvcc, nullptr);
    CHECK(ex.execute("CREATE TABLE t (a TEXT, b TEXT)").message == "CREATE TABLE");
    CHECK(ex.execute("INSERT INTO t VALUES ('x', 'x'), ('x', 'y'), ('y', 'x')").affected ==
          3);
    auto r = ex.execute("SELECT a FROM t WHERE a = b");
    CHECK(r.rows.size() == 1);
    CHECK(valueToString(r.rows[0][0]) == "x");
  }

  std::error_code ec;
  std::filesystem::remove(path, ec);

  if (g_fail == 0) {
    std::cout << "executor tests: all passed (100 insert + replay + where)\n";
    return 0;
  }
  std::cerr << "executor tests: " << g_fail << " FAILURES\n";
  return 1;
}
