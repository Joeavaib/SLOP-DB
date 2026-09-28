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
//   SELECT ... FROM a [AS x] [INNER] JOIN b [AS y]
//     ON a.c = b.d [AND a.e = b.f ...] (genau ein INNER JOIN; Equi-ON per
//     Hash-Join ueber die kleinere Seite, sonst Nested-Loop; WHERE/GROUP BY/
//     ORDER BY/LIMIT/OFFSET danach auf den Combined-Rows wie bisher).
//   UPDATE t SET c = v [, ...] [WHERE ...]  (WHERE-DNF wie SELECT;
//     SET-Werte sind Literale wie in INSERT, typkoerziert pro Spalte)
//   DELETE FROM t [WHERE ...]
//   DROP TABLE [IF EXISTS] t  (kein CASCADE, kein TRUNCATE)
//   GRANT SELECT|INSERT|UPDATE|DELETE|ALL [, ...] ON t TO r
//   REVOKE SELECT|INSERT|UPDATE|DELETE|ALL [, ...] ON t FROM r (TO toleriert)
//   SET ROLE r | RESET ROLE  (r = Identifier/String, Session-lokal, kein WAL;
//     SET ROLE NONE = RESET)
// RBAC minimal: Rollen als Strings (unquoted -> lowercase-Folding wie
// Identifier, "..."/'...' exakt). Privilegien pro (Tabelle, Rolle); ALL =
// alle vier DML-Privilegien. Default-Rolle "" = Admin (alles erlaubt wie
// bisher). SELECT/INSERT/UPDATE/DELETE brauchen das jeweilige Privileg auf
// allen gelesenen/geschriebenen Tabellen (JOIN-Seiten + Subquery-Tabellen
// inklusive); CREATE/DROP/GRANT/REVOKE nur als Admin. Ohne Recht -> SqlError
// mit SQLSTATE 42501. GRANT/REVOKE-Tags: "GRANT"/"REVOKE".
//   Qualifizierte Refs `t.c` (Tabelle oder Alias als Prefix) in SELECT, WHERE,
//     ON, GROUP BY, ORDER BY und Aggregat-Argumenten; unqualifiziert + in
//     beiden Tabellen vorhanden -> SqlError (ambiguous).
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
// V2-Luecken (bewusst): LEFT/RIGHT/FULL/OUTER/CROSS JOIN, TRUNCATE/CASCADE,
// Indexe, Typcheck streng, Prepared Statements / Extended Protocol.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
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

struct SelectStmt;  // forward fuer Condition::subquery

// Max. Verschachtelungstiefe unkorrelierter Subqueries (Parser + Executor).
inline constexpr int kMaxSubqueryDepth = 8;

struct Condition {
  std::string column;  // "c" oder qualifiziert "t.c" (JOIN); "" = Konstante
                       // (nur POLICY USING mit current_user auf LHS ohne Spalte)
  std::string op;  // "=", "<>", "<", "<=", ">", ">=", "LIKE", "ILIKE",
                   // "NOT LIKE", "NOT ILIKE", "IS NULL", "IS NOT NULL",
                   // "BETWEEN", "NOT BETWEEN", "IN", "NOT IN"
  Value value;              // Einzel-Literal bzw. BETWEEN-Untergrenze
  Value second;             // BETWEEN-Obergrenze (sonst NULL)
  std::vector<Value> list;  // IN-Wertliste (sonst leer)
  // Unkorrelierte Subquery (nullptr = Literal/Liste):
  // - "IN"/"NOT IN" + subquery = IN-Subquery (genau 1 Spalte, 0..n Zeilen,
  //   einmal ausgefuehrt, NULL-Semantik wie IN-Liste).
  // - Vergleichs-Op (=,<>,<,<=,>,>=) + subquery = Skalar (genau 1 Spalte,
  //   0 Zeilen -> NULL, >1 Zeile -> SqlError).
  std::shared_ptr<SelectStmt> subquery;
  // RLS (CREATE POLICY ... USING): current_user/current_role/session_user als
  // dynamischer Vergleich gegen die aktuelle Rolle (s. CreatePolicyStmt).
  // value_is_current/second_is_current: jeweiliger Vergleichswert ist die
  // aktuelle Rolle (statt value/second). list_is_current[i] parallel zu list.
  // lhs_is_current: USING der Form "current_user OP literal/[Liste]"
  // (spaltenlose Konstante, column leer). Spaltentausch
  // ("current_user OP col") wird beim Parsen normalisiert (column=col,
  // value_is_current=true, ggf. op invertiert), daher kein Extra-Flag.
  bool value_is_current = false;
  bool second_is_current = false;
  std::vector<char> list_is_current;  // parallel zu list (1 = current_user)
  bool lhs_is_current = false;  // spaltenlose Konstante (column leer)
};

// UPDATE t SET c=v [, ...] [WHERE ...]: SET-Spalten sind unquoted
// lowercase-gefoldet, Werte Literale (Zahl/String/NULL/TRUE/FALSE/DEFAULT,
// wie parseLiteral in INSERT). WHERE-DNF wie SelectStmt (where XOR
// where_groups).
struct UpdateStmt {
  std::string table;
  std::vector<std::pair<std::string, Value>> sets;
  std::vector<Condition> where;  // AND-verknuepft; bei OR leer (s. where_groups)
  std::vector<std::vector<Condition>> where_groups;  // DNF bei OR, sonst leer
};

// DELETE FROM t [WHERE ...]: WHERE-DNF wie UpdateStmt. Ohne WHERE sind alle
// Zeilen betroffen.
struct DeleteStmt {
  std::string table;
  std::vector<Condition> where;
  std::vector<std::vector<Condition>> where_groups;
};

// DROP TABLE [IF EXISTS] t: genau eine Tabelle, kein CASCADE/TRUNCATE.
struct DropTableStmt {
  std::string table;
  bool if_exists = false;
};

// RBAC: GRANT/REVOKE pro (Tabelle, Rolle). privs upper-kanonisch
// ("SELECT"/"INSERT"/"UPDATE"/"DELETE", ALL bereits expandiert, deduped).
// role lower-gefoldet bei unquoted Identifiern, exakt bei "..."/'...'.
struct GrantStmt {
  std::string table;
  std::vector<std::string> privs;
  std::string role;
};

struct RevokeStmt {
  std::string table;
  std::vector<std::string> privs;
  std::string role;
};

// SET ROLE r / RESET ROLE (r wie GrantStmt::role). Session-lokal.
struct SetRoleStmt {
  std::string role;  // "" bei reset
  bool reset = false;
};

// RLS: CREATE POLICY name ON t [FOR SELECT|INSERT|UPDATE|DELETE|ALL] [TO r]
// USING (cond). cond = WHERE-DNF ueber Zeilenwerte; statt Literal darf
// current_user/current_role/session_user stehen (dynamisch = aktuelle Rolle).
// command upper-kanonisch ("SELECT"/"INSERT"/"UPDATE"/"DELETE"/"ALL",
// Default "ALL"). role wie GrantStmt::role ("*" = alle Rollen, wenn TO fehlt).
// where/where_groups: USING-DNF (where bei AND, where_groups bei OR).
struct CreatePolicyStmt {
  std::string policy;
  std::string table;
  std::string command = "ALL";
  std::string role = "*";
  std::vector<Condition> where;
  std::vector<std::vector<Condition>> where_groups;
};

// ALTER TABLE t ENABLE|DISABLE ROW LEVEL SECURITY (RLS-Schalter, Default aus).
struct AlterTableRlsStmt {
  std::string table;
  bool enable = false;
};

// Arithmetischer Ausdruck als Aggregat-Argument (Q6): Spalte | Literal |
// binaer links-assoziativ mit Precedence (*,/ vor +,-), Klammern erlaubt.
// Auswertung in DOUBLE, NULL propagiert (Zeile wird geskippt).
struct AggExpr {
  enum class Kind { Column, Literal, Binary, Case };
  Kind kind = Kind::Column;
  std::string column;                    // Kind::Column (lower-gefoldet,
                                         // ggf. "t.c" bei JOIN)
  Value literal = Value{std::monostate{}};  // Kind::Literal
  char op = 0;                           // Kind::Binary: '+','-','*','/'
  std::shared_ptr<AggExpr> left;
  std::shared_ptr<AggExpr> right;
  std::string display;  // kanonisch ohne Spaces, z.B. "price*(1-disc)"
  // Kind::Case: WHEN-DNF (OR von AND-Konjunktionen aus Condition, wie
  // WHERE) mit THEN-Zweig; else_ nullopt = ohne ELSE (NULL).
  struct CaseWhen {
    std::vector<std::vector<Condition>> dnf;
    std::shared_ptr<AggExpr> then;
  };
  std::vector<CaseWhen> whens;
  std::shared_ptr<AggExpr> else_;
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
  std::string column;  // gueltig wenn !is_agg && !is_ordinal ("c"/"t.c")
  bool is_ordinal = false;
  int64_t ordinal = 0;  // 1-basiert, wenn is_ordinal
  bool desc = false;
  bool has_nulls = false;    // explizites NULLS FIRST/LAST angegeben
  bool nulls_first = false;  // effektiv (Default: desc)
};

// JOIN ... ON-Bedingung (AND-Kette): Spalte-zu-Spalte (Equi "=" oder
// Vergleich "<,<=,>,>=,<>") oder Spalte-zu-Literal. Referenzen wie ueberall
// optional qualifiziert ("t.c", sonst "c"). Reine Equi-Kette ueber beide
// Seiten -> Hash-Join, sonst Nested-Loop.
struct JoinCond {
  std::string left;
  std::string op;  // "=", "<>", "<", "<=", ">", ">="
  bool right_is_col = true;
  std::string right;                // wenn right_is_col
  Value literal = Value{std::monostate{}};  // sonst
};

struct SelectStmt {
  std::string table;
  std::string table_alias;  // "" = kein Alias (Qualifizierer faellt auf table zurueck)
  // Optionaler INNER JOIN (genau einer): FROM t [AS x] [INNER] JOIN u [AS y]
  // ON <cond> [AND <cond> ...]. has_join=false = Single-Table wie bisher.
  // Self-Join (t == u) braucht zwei verschiedene Aliase.
  bool has_join = false;
  std::string join_table;
  std::string join_alias;  // "" = join_table
  std::vector<JoinCond> join_on;  // AND-Kette (>= 1 wenn has_join)
  std::vector<std::string> columns;  // leer + select_all = "*"
                                         // Eintraege "c" oder "t.c" (JOIN)
  std::vector<std::string> column_aliases;  // parallel zu columns, "" = kein Alias
  bool select_all = true;
  bool count_star = false;  // legacy: alleiniges COUNT(*) (Verhalten fixiert)
  std::vector<Aggregate> aggregates;  // nicht-leer => skalares Aggregat ohne GROUP BY
  std::vector<Condition> where;  // AND-verknuepft; bei OR leer (s. where_groups)
  // DNF bei OR: Disjunktion von Konjunktionen. Leer = kein OR (where gilt).
  // Nicht-leer = where ist leer und diese Gruppen gelten (OR dazwischen).
  std::vector<std::vector<Condition>> where_groups;
  std::vector<std::string> group_by;  // leer = keine Gruppierung (1..n Spalten,
                                        // "c"/"t.c")
  std::vector<SelectItem> items;  // Projektionsreihenfolge (leer = legacy-Pfad)
  std::vector<OrderByItem> order_by;  // leer = unsortiert (Einfuege-/First-Seen-Reihenfolge)
  bool has_limit = false;
  int64_t limit = 0;  // >= 0 (negativ -> SqlError)
  bool has_offset = false;
  int64_t offset = 0;  // >= 0 (negativ -> SqlError)
};

using Statement = std::variant<CreateTableStmt, InsertStmt, SelectStmt,
                                   UpdateStmt, DeleteStmt, DropTableStmt,
                                   GrantStmt, RevokeStmt, SetRoleStmt,
                                   CreatePolicyStmt, AlterTableRlsStmt>;

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

// Single-Table WHERE-Match (DNF wie SelectStmt: ohne OR gilt `where` (AND),
// mit OR gelten `where_groups`). Selbe Condition-Auswertung wie SELECT
// (inkl. SqlError bei unbekannter Spalte). Auch vom KV/MVCC-Executor fuer
// UPDATE/DELETE nutzbar.
bool rowMatchesWhere(const Table& t, const std::vector<Value>& row,
                     const std::vector<Condition>& where,
                     const std::vector<std::vector<Condition>>& where_groups);

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
   // Snapshot-Ausfuehrung fuer den KV/MVCC-Executor: fuehrt `s` direkt auf den
   // uebergebenen Snapshot-Tabellen aus (genau ein Durchlauf, ohne
   // execCreate/execInsert/Coerce). Semantik exakt wie execSelect (Filter,
   // NULL, LIKE, JOIN/GROUP/ORDER/Subqueries, Fehlermeldungen identisch);
   // stellt die eigenen Tabellen danach wieder her (auch bei SqlError).
   Result execSelectSnapshot(const SelectStmt& s,
                             std::map<std::string, Table> snapshot);
  Result execUpdate(const UpdateStmt& s);
  Result execDelete(const DeleteStmt& s);
  Result execDrop(const DropTableStmt& s);
   Result execGrant(const GrantStmt& s);
   Result execRevoke(const RevokeStmt& s);
   Result execSetRole(const SetRoleStmt& s);
   Result execCreatePolicy(const CreatePolicyStmt& s);
   Result execAlterRls(const AlterTableRlsStmt& s);

  // RBAC: aktuelle Rolle ("" = Admin, Default: alles erlaubt wie bisher).
  // Input wird wie ein unquoted Identifier nach lowercase gefaltet.
  void setRole(const std::string& role);
  const std::string& role() const { return role_; }

  bool hasTable(const std::string& name) const;
  const Table& getTable(const std::string& name) const;

  // RLS: true wenn ENABLE ROW LEVEL SECURITY fuer norm-Tabelle aktiv.
  // Oeffentlich, damit der KV/MVCC-Executor dieselbe USING-Semantik
  // (inkl. LIKE/current_user) via Temp-Database wiederverwenden kann.
  bool rlsEnabled(const std::string& normTable) const;
  bool rowPassesRls(const Table& t, const std::vector<Value>& row,
                    const std::string& normTable,
                    const std::string& op) const;

 private:
  // true wenn Admin ("") oder priv auf (Tabelle, Rolle) gewaehrt.
  bool hasPriv(const std::string& table, const std::string& priv) const;
  void requirePriv(const std::string& table, const std::string& priv) const;
  void requireAdmin(const std::string& what) const;
  // Eine USING-Bedingung gegen (row, aktuelle Rolle) auswerten
  // (current_user-Dynamik aufgeloest, sonst WHERE-Semantik).
  bool evalPolicyCond(const Table& t, const std::vector<Value>& row,
                      const Condition& c) const;
  std::map<std::string, Table> tables_;
  std::string role_;  // lower-gefoldet, "" = Admin
  // norm-Tabelle -> norm-Rolle -> Privilegien (upper).
  std::map<std::string, std::map<std::string, std::set<std::string>>> grants_;
  // RLS-Registry: norm-Tabelle -> Policies (Namen eindeutig je Tabelle);
  // rls_on_: norm-Tabellen mit ENABLE ROW LEVEL SECURITY (Default: aus).
  std::map<std::string, std::vector<CreatePolicyStmt>> policies_;
  std::unordered_set<std::string> rls_on_;
};

Statement parseStatement(const std::string& sql);
std::string statementKind(
    const Statement& s);  // "CREATE"/"INSERT"/"SELECT"/"UPDATE"/"DELETE"/"DROP"/
                          // "GRANT"/"REVOKE"/"SET"

}  // namespace dbengine::sql
