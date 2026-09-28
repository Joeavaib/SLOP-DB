#include <unistd.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dbengine/db.h"
#include "dbengine/kv.h"
#include "dbengine/sql/executor.h"
#include "dbengine/sql/parser.h"
#include "dbengine/storage/wal.h"
#include "dbengine/txn/mvcc.h"
#include "dbengine/version.h"

namespace {

void printVersion() {
  std::cout << "dbengine version " << dbengine::kVersion << '\n';
}

void printHelp(const char* prog) {
  std::cout
      << "Aufruf:\n"
      << "  " << prog << " <db-datei> [--exec \"SQL;...\"] [--sql datei.sql]...\n"
      << "  " << prog << " <db-datei>                       "
         "(interaktiver REPL, liest stdin)\n"
      << "  " << prog << " --help | --version\n"
      << "\nBeschreibung:\n"
      << "  Oeffnet <db-datei> via Db::open (wird erzeugt, falls fehlend) und\n"
      << "  fuehrt SQL ueber KVStore + MvccStore + Executor aus.\n"
      << "  --exec und --sql sind wiederholbar und werden in CLI-Reihenfolge\n"
      << "  ausgefuehrt. Ohne diese Flags startet ein REPL auf stdin.\n"
      << "  Statements werden per ';' getrennt: der Parser versteht genau EIN\n"
      << "  Statement pro Aufruf, der Split respektiert '...'-Strings (inkl.\n"
      << "  ''-Escape) und \"...\"-Bezeichner.\n"
      << "\nPersistenz-Modell (ehrlich):\n"
      << "  - <db-datei> (z.B. foo.db) ist aktuell NUR ein Container\n"
      << "    (Magic+Version+Region-Table+CRC, Regionen reserviert+genullt).\n"
      << "    SQL-Daten stehen NICHT im Body von foo.db.\n"
      << "  - Schema + Rows leben in KV/MVCC im Speicher und werden pro\n"
      << "    Statement ins WAL geschrieben: <db-datei>.wal (neben der DB,\n"
      << "    Group-Commit via flush pro Statement).\n"
      << "  - Beim Start laeuft WAL-Replay (recover): Schema + Rows ueberleben\n"
      << "    einen Restart NUR via die WAL-Datei. Ohne WAL-Datei (geloescht\n"
      << "    oder anderer Pfad) startet die DB leer; WAL-Verlust =\n"
      << "    Datenverlust. Ein abgerissener WAL-Tail wird toleriert\n"
      << "    (gueltiges Prefix gewinnt).\n"
      << "  - Mirror-Checkpoint: <db-datei>.btree (BTreeKV-Sidecar, Latest-\n"
      << "    State je Key + Spiegel-LSN) wird pro WAL-Flush inkrementell\n"
      << "    nachgezogen; beim Start wird daraus geladen und nur der\n"
      << "    WAL-Tail (lsn > Spiegel-LSN) replayt. Fehlt/korrupt -> Voll-\n"
      << "    Replay + Warnung (WAL bleibt Wahrheit, kein Datenverlust).\n"
      << "  - Exit-Checkpoint: bei sauberem Exit (Batch-Ende, .quit/.exit, EOF)\n"
      << "    laeuft mirrorCheckpoint (Spiegel aktuell -> schnellerer Restart).\n"
      << "    Container ohne WAL ist leer; Tipp: DB immer mit gleichem Pfad\n"
      << "    oeffnen, damit <db>.wal und <db>.btree daneben liegen.\n"
      << "\nREPL:\n"
      << "  .quit / .exit beendet, leere Zeilen werden ignoriert, SQL-Fehler\n"
      << "  werden gedruckt und der REPL laeuft weiter, EOF (Ctrl-D) beendet.\n"
      << "  Mehrzeilige Statements sind moeglich (Ausfuehrung bei ';').\n"
      << "\nAusgabe:\n"
      << "  SELECT -> Tabelle (Kopfzeile = Spaltennamen, Zeilen via\n"
      << "  valueToString, '|' als Trenner). Andere Statements -> Message-Tag\n"
      << "  des Executors (z.B. CREATE TABLE, INSERT 0 <n>, UPDATE <n>).\n"
      << "  Fehler -> stderr mit Prefix \"ERROR: \".\n"
      << "\nExit-Codes:\n"
      << "  0 = ok. 1 = Datei-/Oeffnungsfehler oder (nur bei --exec/--sql)\n"
      << "  mindestens ein SQL-Fehler. Der REPL setzt bei SQL-Fehlern den\n"
      << "  Exit-Code NICHT auf 1 (weiterlaufen), Oeffnungsfehler -> 1.\n"
      << "\nBeispiele:\n"
      << "  " << prog << " foo.db --exec \"CREATE TABLE t (id INT, name TEXT);\"\n"
      << "  " << prog << " foo.db --exec \"INSERT INTO t VALUES (1, 'a');\"\n"
      << "                       --exec \"SELECT * FROM t;\"\n"
      << "  " << prog << " foo.db --sql init.sql --exec \"SELECT COUNT(*) FROM t;\"\n"
      << "  " << prog << " foo.db < script.sql\n";
}

void printUsageError(const char* prog, const std::string& msg) {
  std::cerr << prog << ": Fehler: " << msg << "\n"
            << "Aufruf: " << prog
            << " <db-datei> [--exec \"SQL;...\"] [--sql datei.sql] "
               "| --help | --version\n";
}

std::string trim(const std::string& s) {
  const std::string ws = " \t\r\n";
  const std::size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return "";
  const std::size_t e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

// Teilt SQL per ';'. Respektiert '...'-Strings (''-Escape) und
// "..."-Bezeichner (""-Escape), weil der Parser nur ein Single-Statement
// pro Aufruf versteht.
std::vector<std::string> splitStatements(const std::string& sql) {
  std::vector<std::string> out;
  std::string cur;
  bool in_single = false;
  bool in_double = false;
  for (std::size_t i = 0; i < sql.size(); ++i) {
    const char c = sql[i];
    if (in_single) {
      cur += c;
      if (c == '\'') {
        if (i + 1 < sql.size() && sql[i + 1] == '\'') {
          cur += '\'';
          ++i;
        } else {
          in_single = false;
        }
      }
    } else if (in_double) {
      cur += c;
      if (c == '"') {
        if (i + 1 < sql.size() && sql[i + 1] == '"') {
          cur += '"';
          ++i;
        } else {
          in_double = false;
        }
      }
    } else if (c == '\'') {
      in_single = true;
      cur += c;
    } else if (c == '"') {
      in_double = true;
      cur += c;
    } else if (c == ';') {
      const std::string t = trim(cur);
      if (!t.empty()) out.push_back(t);
      cur.clear();
    } else {
      cur += c;
    }
  }
  const std::string rest = trim(cur);
  if (!rest.empty()) out.push_back(rest);
  return out;
}

// Holt aus buf alle per ';' abgeschlossenen Statements (Quote-bewusst wie
// splitStatements) und laesst den unvollstaendigen Rest in buf zurueck.
std::vector<std::string> popComplete(std::string& buf) {
  std::vector<std::string> out;
  std::string cur;
  bool in_single = false;
  bool in_double = false;
  std::size_t done_upto = 0;  // alles vor done_upto ist verarbeitet
  for (std::size_t i = 0; i < buf.size(); ++i) {
    const char c = buf[i];
    if (in_single) {
      cur += c;
      if (c == '\'') {
        if (i + 1 < buf.size() && buf[i + 1] == '\'') {
          cur += '\'';
          ++i;
        } else {
          in_single = false;
        }
      }
    } else if (in_double) {
      cur += c;
      if (c == '"') {
        if (i + 1 < buf.size() && buf[i + 1] == '"') {
          cur += '"';
          ++i;
        } else {
          in_double = false;
        }
      }
    } else if (c == '\'') {
      in_single = true;
      cur += c;
    } else if (c == '"') {
      in_double = true;
      cur += c;
    } else if (c == ';') {
      const std::string t = trim(cur);
      if (!t.empty()) out.push_back(t);
      cur.clear();
      done_upto = i + 1;
    } else {
      cur += c;
    }
  }
  buf = buf.substr(done_upto);
  return out;
}

void printResult(const dbengine::sql::Result& r, std::ostream& out) {
  if (!r.columns.empty()) {
    for (std::size_t i = 0; i < r.columns.size(); ++i) {
      if (i != 0) out << '|';
      out << r.columns[i];
    }
    out << '\n';
    for (const auto& row : r.rows) {
      for (std::size_t i = 0; i < row.size(); ++i) {
        if (i != 0) out << '|';
        out << dbengine::sql::valueToString(row[i]);
      }
      out << '\n';
    }
  } else if (!r.message.empty()) {
    out << r.message << '\n';
  } else {
    out << "OK\n";
  }
}

// Ein Statement ausfuehren + ausgeben. Fehler -> stderr, false zurueck.
bool runOne(dbengine::sql::Executor& ex, const std::string& stmt) {
  try {
    dbengine::sql::Result r = ex.execute(stmt);
    printResult(r, std::cout);
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << '\n';
    return false;
  }
  return true;
}

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in) throw std::runtime_error("SQL-Datei nicht lesbar: " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

int runRepl(dbengine::sql::Executor& ex, const std::string& db_path,
            const std::string& wal_path) {
  const bool tty = ::isatty(STDIN_FILENO) != 0;
  std::cout << "dbengine " << dbengine::kVersion << " -- " << db_path
            << " (WAL: " << wal_path << ")\n"
            << "'.quit' beendet.\n";
  std::string buf;
  std::string line;
  while (true) {
    if (tty) {
      std::cout << "db> " << std::flush;
    }
    if (!std::getline(std::cin, line)) break;  // EOF beendet
    const std::string t = trim(line);
    if (t == ".quit" || t == ".exit") break;
    if (t.empty()) continue;  // leere Zeilen skippen
    if (!t.empty() && t[0] == '.') {
      std::cerr << "ERROR: unbekannter Befehl '" << t
                << "' (nur .quit/.exit)\n";
      continue;  // Fehler drucken + weiter
    }
    buf += line;
    buf += '\n';
    const std::vector<std::string> ready = popComplete(buf);
    for (const auto& stmt : ready) {
      runOne(ex, stmt);  // Fehler drucken + weiter, Exit-Code bleibt 0
    }
  }
  // Rest ohne abschliessendes ';' bei EOF noch ausfuehren (wenn nicht leer).
  const std::string rest = trim(buf);
  if (!rest.empty()) runOne(ex, rest);
  ex.mirrorCheckpoint();  // sauberer Exit: Spiegel aktuell -> schneller Restart
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const char* prog = (argc > 0 && argv[0] != nullptr) ? argv[0] : "dbengine";
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

  for (const auto& a : args) {
    if (a == "--help" || a == "-h") {
      printHelp(prog);
      return 0;
    }
  }
  for (const auto& a : args) {
    if (a == "--version" || a == "-V") {
      printVersion();
      return 0;
    }
  }

  if (args.empty() || (!args[0].empty() && args[0][0] == '-')) {
    printUsageError(prog, "keine DB-Datei angegeben");
    return 1;
  }
  const std::string db_path = args[0];

  // CLI-Reihenfolge von --exec/--sql erhalten.
  std::vector<std::string> chunks;  // SQL-Texte in Ausfuehrungsreihenfolge
  bool want_repl = true;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--exec") {
      if (i + 1 >= args.size()) {
        printUsageError(prog, "--exec braucht ein SQL-Argument");
        return 1;
      }
      chunks.push_back(args[i + 1]);
      ++i;
      want_repl = false;
    } else if (args[i] == "--sql") {
      if (i + 1 >= args.size()) {
        printUsageError(prog, "--sql braucht einen Dateipfad");
        return 1;
      }
      try {
        chunks.push_back(readFile(args[i + 1]));
      } catch (const std::exception& e) {
        std::cerr << prog << ": Fehler: " << e.what() << '\n';
        return 1;
      }
      ++i;
      want_repl = false;
    } else {
      printUsageError(prog, std::string("unbekanntes Argument: ") + args[i]);
      return 1;
    }
  }

  // 1) Echte DB oeffnen (Fehler -> stderr + exit 1).
  dbengine::Db db;
  try {
    db = dbengine::Db::open(db_path);
  } catch (const std::exception& e) {
    std::cerr << prog << ": Fehler: Db::open('" << db_path
              << "') fehlgeschlagen: " << e.what() << '\n';
    return 1;
  }
  if (!db.is_open()) {
    std::cerr << prog << ": Fehler: Db::open('" << db_path
              << "') meldet geschlossen\n";
    return 1;
  }

  // 2) KV + MVCC + WAL dahinter. WAL-Datei liegt neben foo.db als
  //    "<foo.db>.wal" und traegt Schema + Rows (s. --help Persistenz-Modell).
  const std::string wal_path = db_path + ".wal";
  dbengine::kv::KVStore kv;
  dbengine::txn::MvccStore mvcc;
  dbengine::storage::Wal wal(wal_path);
  // Ehrliche Start-Warnung: Container ohne WAL ist leer (s100).
  bool wal_existed = false;
  {
    std::ifstream probe(wal_path, std::ios::binary);
    wal_existed = static_cast<bool>(probe);
  }
  try {
    wal.open();
  } catch (const std::exception& e) {
    std::cerr << prog << ": Fehler: WAL '" << wal_path
              << "' konnte nicht geoeffnet werden: " << e.what() << '\n';
    return 1;
  }
  dbengine::sql::Executor ex(kv, mvcc, &wal);
  // Mirror-Checkpoint-Sidecar (<db>.btree) tolerant aktivieren: Fehler
  // (fehlend/korrupt) -> Voll-Replay + Warnung, nie Datenverlust.
  const std::string mirror_path = db_path + ".btree";
  try {
    std::string mirror_warn;
    const bool mirror_ok = ex.enableMirror(mirror_path, &mirror_warn);
    if (!mirror_ok) {
      std::cerr << prog << ": Warnung: Spiegel '" << mirror_path
                << "' nicht aktiv"
                << (mirror_warn.empty() ? "" : (": " + mirror_warn))
                << "; Voll-Replay aus WAL\n";
    } else if (!mirror_warn.empty()) {
      std::cerr << prog << ": Warnung: Spiegel '" << mirror_path
                << "': " << mirror_warn
                << " (Voll-Replay, Spiegel wird geheilt)\n";
    }
  } catch (const std::exception& e) {
    std::cerr << prog << ": Warnung: Spiegel '" << mirror_path << "' Fehler ("
              << e.what() << "); Voll-Replay aus WAL\n";
  }
  try {
    const std::size_t skipped = ex.recover();
    if (!wal_existed) {
      std::cerr << prog << ": Warnung: WAL '" << wal_path
                << "' fehlt/neu - Container ohne WAL ist leer; starte mit "
                   "leerem SQL-Stand\n";
    }
    if (skipped != 0) {
      std::cerr << prog << ": Warnung: " << skipped
                << " WAL-Record(s) beim Replay uebersprungen (korrupt/unbekannt)\n";
    }
  } catch (const std::exception& e) {
    std::cerr << prog << ": Warnung: WAL-Replay fehlgeschlagen (" << e.what()
              << "); starte mit leerem SQL-Stand\n";
  }

  // 3) Batch-Modus (--exec/--sql) oder REPL.
  if (!want_repl) {
    bool all_ok = true;
    for (const auto& chunk : chunks) {
      for (const auto& stmt : splitStatements(chunk)) {
        if (!runOne(ex, stmt)) all_ok = false;
      }
    }
    ex.mirrorCheckpoint();  // sauberer Exit: Spiegel aktuell
    return all_ok ? 0 : 1;
  }
  return runRepl(ex, db_path, wal_path);
}
