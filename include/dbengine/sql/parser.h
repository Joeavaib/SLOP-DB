#pragma once

// Mini-SQL V1 (PostgreSQL-kompatibler Subset, kein eigenes Dialekt-Silo).
// Unterstuetzte Syntax (case-insensitiv, unquoted Identifier werden wie in PG
// nach lowercase gefaltet):
//   CREATE TABLE [IF NOT EXISTS] t (a INT|INTEGER, b TEXT|VARCHAR|CHAR,
//                                   c DOUBLE|FLOAT|REAL, d BOOL|BOOLEAN,
//                                   j JSONB)
//   INSERT INTO t [(cols)] VALUES (v, ...), (...), ...
//   SELECT [* | col [AS alias], ... | COUNT(*)] FROM t
//     [WHERE disj [OR disj ...]] [GROUP BY <cols>] [ORDER BY o [ASC|DESC]
//     [NULLS FIRST|LAST], ...] [LIMIT n|ALL] [OFFSET n]
//     disj  := cond [AND cond ...]              (AND bindet staerker als OR)
//     cond  := col (=|<>|!=|<|<=|>|>=) literal | col LIKE 'pat' | col ILIKE 'pat'
//            | col NOT LIKE 'pat' | col NOT ILIKE 'pat'
//            | col [NOT] BETWEEN a AND b | col [NOT] IN (v, ...)
//            | col IS [NOT] NULL
//   Skalar-Aggregate (s39, ohne GROUP BY): SUM/AVG/MIN/MAX/COUNT(col|*),
//     Arg = Spalte oder binaerer */+-Ausdruck, z.B. SUM(price*(1-disc)).
//   GROUP BY (s43): SELECT <group-cols>, AGG(..) [AS alias], ... FROM t
//     [WHERE ...] GROUP BY <cols> (1..n Spalten, Hash-Aggregation, Q1-Kern).
//     Leere Eingabe -> 0 Gruppen (keine Zeile); ohne GROUP BY bleibt die
//     1-Zeilen-Semantik der Skalar-Aggregate bestehen.
// Literale: INT, FLOAT, 'string' ('' = escape), NULL, TRUE/FALSE,
//           JSONB als Text-Literal ('{"a":1}'::jsonb wird als Text genommen).
// ORDER BY-Referenzen: Ausgabe-Spalte/Alias, (gruppierte) Tabellenspalte,
// Aggregat-Ausdruck oder 1-basiertes Positions-Ordinal (PG). Sortierung +
// LIMIT/OFFSET werden nach Filter/Gruppierung/Aggregation angewendet.
// NULL-Platzierung PG-konform (Default ASC->NULLS LAST, DESC->NULLS FIRST).
// V2-Luecken (bewusst): Joins, UPDATE/DELETE, Indexe, Typcheck streng,
// Prepared Statements / Extended Protocol.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace dbengine::sql {

class SqlError : public std::runtime_error {
 public:
  explicit SqlError(const std::string& msg) : std::runtime_error(msg) {}
};

enum class ColType { Int, Text, Double, Bool, Jsonb };

std::string colTypeToString(ColType t);
ColType colTypeFromString(const std::string& s);  // wirft SqlError bei unknown

struct ColumnDef {
  std::string name;
  ColType type = ColType::Text;
};

// NULL = std::monostate
using Value = std::variant<std::monostate, int64_t, double, std::string, bool>;

bool valueIsNull(const Value& v);
std::string valueToString(const Value& v);  // Anzeige / Wire-Format
bool valueEquals(const Value& a, const Value& b);

struct CreateTableStmt {
  std::string table;
  std::vector<ColumnDef> columns;
  bool if_not_exists = false;
};

struct InsertStmt {
  std::string table;
  std::vector<std::string> columns;  // leer = alle Spalten in Tabellenreihenfolge
  std::vector<std::vector<Value>> rows;
};

struct Condition {
  std::string column;
  std::string op;  // "=", "<>", "<", "<=", ">", ">=", "LIKE", "ILIKE",
                   // "NOT LIKE", "NOT ILIKE", "IS NULL", "IS NOT NULL",
                   // "BETWEEN", "NOT BETWEEN", "IN", "NOT IN"
  Value value;              // Einzel-Literal bzw. BETWEEN-Untergrenze
  Value second;             // BETWEEN-Obergrenze (sonst NULL)
  std::vector<Value> list;  // IN-Wertliste (sonst leer)
};

// Arithmetischer Ausdruck als Aggregat-Argument (Q6): Spalte | Literal |
// binaer links-assoziativ mit Precedence (*,/ vor +,-), Klammern erlaubt.
// Auswertung in DOUBLE, NULL propagiert (Zeile wird geskippt).
struct AggExpr {
  enum class Kind { Column, Literal, Binary };
  Kind kind = Kind::Column;
  std::string column;                    // Kind::Column (lower-gefoldet)
  Value literal = Value{std::monostate{}};  // Kind::Literal
  char op = 0;                           // Kind::Binary: '+','-','*','/'
  std::shared_ptr<AggExpr> left;
  std::shared_ptr<AggExpr> right;
  std::string display;  // kanonisch ohne Spaces, z.B. "price*(1-disc)"
};

struct Aggregate {
  std::string func;  // upper: "SUM","AVG","MIN","MAX","COUNT"
  bool star = false;                     // nur COUNT(*)
  std::shared_ptr<AggExpr> arg;          // null bei star
  std::string display;  // z.B. "SUM(price*disc)", "COUNT(*)"
  std::string alias;  // optional: "SUM(x) AS s" (leer = display als Spaltenname)
};

// Ein Projektionseintrag in SELECT-Reihenfolge (nur Nicht-*-Pfad).
// Ohne GROUP BY ist die Liste homogen (nur Spalten oder nur Aggregate);
// mit GROUP BY mischt sie Gruppen-Spalten und Aggregate beliebig.
struct SelectItem {
  bool is_agg = false;    // true -> aggregates[index], false -> columns[index]
  std::size_t index = 0;  // Position in aggregates bzw. columns
};

// ORDER BY-Item: Spalte/Alias (column, lower-gefoldet) | Aggregat-Ausdruck
// (is_agg, z.B. SUM(x)) | Positions-Ordinal (is_ordinal, 1-basiert auf die
// Ausgabe-Spalten). Richtung je Item (desc), NULL-Platzierung effektiv in
// nulls_first (Default aus desc, explizit via NULLS FIRST/LAST).
struct OrderByItem {
  bool is_agg = false;
  Aggregate agg;      // gueltig wenn is_agg
  std::string column;  // gueltig wenn !is_agg && !is_ordinal
  bool is_ordinal = false;
  int64_t ordinal = 0;  // 1-basiert, wenn is_ordinal
  bool desc = false;
  bool has_nulls = false;    // explizites NULLS FIRST/LAST angegeben
  bool nulls_first = false;  // effektiv (Default: desc)
};

struct SelectStmt {
  std::string table;
  std::vector<std::string> columns;  // leer + select_all = "*"
  std::vector<std::string> column_aliases;  // parallel zu columns, "" = kein Alias
  bool select_all = true;
  bool count_star = false;  // legacy: alleiniges COUNT(*) (Verhalten fixiert)
  std::vector<Aggregate> aggregates;  // nicht-leer => skalares Aggregat ohne GROUP BY
  std::vector<Condition> where;  // AND-verknuepft; bei OR leer (s. where_groups)
  // DNF bei OR: Disjunktion von Konjunktionen. Leer = kein OR (where gilt).
  // Nicht-leer = where ist leer und diese Gruppen gelten (OR dazwischen).
  std::vector<std::vector<Condition>> where_groups;
  std::vector<std::string> group_by;  // leer = keine Gruppierung (1..n Spalten)
  std::vector<SelectItem> items;  // Projektionsreihenfolge (leer = legacy-Pfad)
  std::vector<OrderByItem> order_by;  // leer = unsortiert (Einfuege-/First-Seen-Reihenfolge)
  bool has_limit = false;
  int64_t limit = 0;  // >= 0 (negativ -> SqlError)
  bool has_offset = false;
  int64_t offset = 0;  // >= 0 (negativ -> SqlError)
};

using Statement =
    std::variant<CreateTableStmt, InsertStmt, SelectStmt>;

struct Result {
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
  std::string message;
  std::size_t affected = 0;
};

struct Table {
  std::vector<ColumnDef> columns;
  std::vector<std::vector<Value>> rows;
  int colIndex(const std::string& name) const;  // -1 wenn unbekannt
};

// In-Memory Executor. Falls spaeter include/dbengine/kv/kv.h existiert, kann
// Database als Frontend vor einen KVStore gesetzt werden (Tabellen-Praefix
// "sql/<table>/..."); aktuell bewusst ohne harte kv-Abhaengigkeit (Fallback).
class Database {
 public:
  Database() = default;

  Result execute(const std::string& sql);

  Result execCreate(const CreateTableStmt& s);
  Result execInsert(const InsertStmt& s);
  Result execSelect(const SelectStmt& s);

  bool hasTable(const std::string& name) const;
  const Table& getTable(const std::string& name) const;

 private:
  std::map<std::string, Table> tables_;
};

Statement parseStatement(const std::string& sql);
std::string statementKind(const Statement& s);  // "CREATE"/"INSERT"/"SELECT"

}  // namespace dbengine::sql
