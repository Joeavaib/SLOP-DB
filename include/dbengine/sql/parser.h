#pragma once

// Mini-SQL V1 (PostgreSQL-kompatibler Subset, kein eigenes Dialekt-Silo).
// Unterstuetzte Syntax (case-insensitiv, unquoted Identifier werden wie in PG
// nach lowercase gefaltet):
//   CREATE TABLE [IF NOT EXISTS] t (a INT|INTEGER, b TEXT|VARCHAR|CHAR,
//                                   c DOUBLE|FLOAT|REAL, d BOOL|BOOLEAN,
//                                   j JSONB)
//   INSERT INTO t [(cols)] VALUES (v, ...), (...), ...
//   SELECT [* | col, ... | COUNT(*)] FROM t [WHERE cond [AND cond ...]]
//     cond := col (=|<>|!=|<|<=|>|>=) literal | col LIKE 'pat' | col ILIKE 'pat'
// Literale: INT, FLOAT, 'string' ('' = escape), NULL, TRUE/FALSE,
//           JSONB als Text-Literal ('{"a":1}'::jsonb wird als Text genommen).
// V2-Luecken (bewusst): Joins, ORDER BY/LIMIT, UPDATE/DELETE, Indexe, Typcheck
// streng, Prepared Statements / Extended Protocol.

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
  std::string op;  // "=", "<>", "!=", "<", "<=", ">", ">=", "LIKE", "ILIKE"
  Value value;
};

struct SelectStmt {
  std::string table;
  std::vector<std::string> columns;  // leer + select_all = "*"
  bool select_all = true;
  bool count_star = false;
  std::vector<Condition> where;  // AND-verknuepft
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
