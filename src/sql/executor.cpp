// s11-executor: Parser + KV + MVCC + WAL-Verdrahtung.

#include "dbengine/sql/executor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dbengine::sql {
namespace {

// PG-Folding: unquoted Identifier -> lowercase (vgl. parser.cpp).
std::string toLower(std::string s) {
  for (auto& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

int schemaColIndex(const std::vector<ColumnDef>& cols, const std::string& name) {
  const std::string f = toLower(name);
  for (std::size_t i = 0; i < cols.size(); ++i) {
    std::string cn = toLower(cols[i].name);
    if (cn == f) return static_cast<int>(i);
  }
  return -1;
}

// ---- Row-Codec: Felder mit '|' getrennt, Typ-Praefix "X:" -----------------
// S-Payload escapet: '\\' -> "\\\\", '|' -> "\\p", '\n' -> "\\n", '\r' -> "\\r".
// Enthaelt nie '\x1f' -> WAL-Feldseparator bleibt eindeutig.
std::string escapeField(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    if (c == '\\')
      o += "\\\\";
    else if (c == '|')
      o += "\\p";
    else if (c == '\n')
      o += "\\n";
    else if (c == '\r')
      o += "\\r";
    else
      o += c;
  }
  return o;
}

std::string unescapeField(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char n = s[i + 1];
      if (n == '\\') {
        o += '\\';
        ++i;
      } else if (n == 'p') {
        o += '|';
        ++i;
      } else if (n == 'n') {
        o += '\n';
        ++i;
      } else if (n == 'r') {
        o += '\r';
        ++i;
      } else {
        throw SqlError("Korrupte Row-Kodierung (Escape)");
      }
    } else if (s[i] == '\\') {
      throw SqlError("Korrupte Row-Kodierung (trailing backslash)");
    } else {
      o += s[i];
    }
  }
  return o;
}

// Split an unescaptem '|' (Backslash-Sequenzen bleiben im Feld erhalten,
// werden erst danach via unescapeField aufgeloest).
std::vector<std::string> splitRowFields(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      cur += s[i];
      cur += s[i + 1];
      ++i;
    } else if (s[i] == '|') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += s[i];
    }
  }
  out.push_back(cur);
  return out;
}

// ---- WAL-Framing: Felder mit '\x1f' getrennt --------------------------------
// walEscape: '\\' -> "\\\\", '\x1f' -> "\\x", '\n' -> "\\n", '\r' -> "\\r".
constexpr char kWalSep = '\x1f';

std::string walEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    if (c == '\\')
      o += "\\\\";
    else if (c == kWalSep)
      o += "\\x";
    else if (c == '\n')
      o += "\\n";
    else if (c == '\r')
      o += "\\r";
    else
      o += c;
  }
  return o;
}

std::string walUnescape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\') {
      if (i + 1 >= s.size()) throw SqlError("Korrupter WAL-Record (Escape)");
      char n = s[i + 1];
      if (n == '\\') {
        o += '\\';
        ++i;
      } else if (n == 'x') {
        o += kWalSep;
        ++i;
      } else if (n == 'n') {
        o += '\n';
        ++i;
      } else if (n == 'r') {
        o += '\r';
        ++i;
      } else {
        throw SqlError("Korrupter WAL-Record (Escape-Code)");
      }
    } else {
      o += s[i];
    }
  }
  return o;
}

std::vector<std::string> splitWal(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == kWalSep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

// Typ-Koerzierung wie parser.cpp/coerceTo (V1, locker ausser INT/DOUBLE).
Value coerceValue(const Value& v, ColType type, const std::string& col) {
  if (valueIsNull(v)) return v;
  switch (type) {
    case ColType::Int:
      if (std::holds_alternative<int64_t>(v)) return v;
      if (auto* d = std::get_if<double>(&v))
        return Value{static_cast<int64_t>(*d)};
      if (auto* s = std::get_if<std::string>(&v)) {
        try {
          return Value{static_cast<int64_t>(std::stoll(*s))};
        } catch (...) {
          throw SqlError("Typfehler: '" + *s + "' kein INT (" + col + ")");
        }
      }
      if (auto* b = std::get_if<bool>(&v))
        return Value{static_cast<int64_t>(*b ? 1 : 0)};
      break;
    case ColType::Double:
      if (std::holds_alternative<double>(v)) return v;
      if (auto* i = std::get_if<int64_t>(&v))
        return Value{static_cast<double>(*i)};
      if (auto* s = std::get_if<std::string>(&v)) {
        try {
          return Value{std::stod(*s)};
        } catch (...) {
          throw SqlError("Typfehler: '" + *s + "' kein FLOAT (" + col + ")");
        }
      }
      break;
    case ColType::Bool:
      if (std::holds_alternative<bool>(v)) return v;
      break;
    case ColType::Text:
    case ColType::Jsonb:
      if (std::holds_alternative<std::string>(v)) return v;
      return Value{valueToString(v)};
  }
  return v;
}

// Alle Tabellen eines SELECTs inkl. genesteter WHERE-Subqueries (normiert).
// Unkorrelierte Subqueries duerfen beliebige (eigene) Tabellen lesen; der
// KV-Executor muss sie alle in die tmp-Database spiegeln, sonst wuerde die
// Exec-Delegation an Database mit "Tabelle unbekannt" scheitern.
void collectSelectTables(const SelectStmt& s, std::set<std::string>& out) {
  out.insert(Executor::normalizeTable(s.table));
  if (s.has_join) out.insert(Executor::normalizeTable(s.join_table));
  std::vector<const SelectStmt*> stack;
  for (auto& c : s.where)
    if (c.subquery) stack.push_back(c.subquery.get());
  for (auto& gr : s.where_groups)
    for (auto& c : gr)
      if (c.subquery) stack.push_back(c.subquery.get());
  while (!stack.empty()) {
    const SelectStmt* q = stack.back();
    stack.pop_back();
    out.insert(Executor::normalizeTable(q->table));
    if (q->has_join) out.insert(Executor::normalizeTable(q->join_table));
    for (auto& c : q->where)
      if (c.subquery) stack.push_back(c.subquery.get());
    for (auto& gr : q->where_groups)
      for (auto& c : gr)
        if (c.subquery) stack.push_back(c.subquery.get());
  }
}

void collectWhereTables(const std::vector<Condition>& where,
                        const std::vector<std::vector<Condition>>& groups,
                        std::set<std::string>& out) {
  std::vector<const SelectStmt*> stack;
  for (auto& c : where)
    if (c.subquery) stack.push_back(c.subquery.get());
  for (auto& gr : groups)
    for (auto& c : gr)
      if (c.subquery) stack.push_back(c.subquery.get());
  while (!stack.empty()) {
    const SelectStmt* q = stack.back();
    stack.pop_back();
    out.insert(Executor::normalizeTable(q->table));
    if (q->has_join) out.insert(Executor::normalizeTable(q->join_table));
    for (auto& c : q->where)
      if (c.subquery) stack.push_back(c.subquery.get());
    for (auto& gr : q->where_groups)
      for (auto& c : gr)
        if (c.subquery) stack.push_back(c.subquery.get());
  }
}

// ---- Projektions-Pushdown: Bedarfsanalyse ----------------------------------
// need[tnorm][i] == 1 -> Spalte i von Tabelle tnorm wird gelesen (Projektion,
// WHERE, GROUP BY, ORDER BY, JOIN-ON, Aggregat-Argumente, inkl. aller
// genesteter Subqueries). Alles andere darf beim Dekodieren uebersprungen
// werden. Unbekannte/ambiguous Referenzen -> konservativ alles markieren:
// Table.columns bleibt immer vollstaendig, daher wirft die Database-Schicht
// bei solchen Queries exakt wie bisher (Fehlerpfade unveraendert); ein zu
// grosses need kostet nur Performance, nie Korrektheit.
using NeededMap = std::map<std::string, std::vector<char>>;

// Scope-Sicht EINES Select-Knotens (aeusseres Statement oder Subquery).
struct ScopeSchemas {
  std::string lNorm;
  const TableSchema* lSch = nullptr;
  std::string lAlias;
  bool hasRight = false;
  std::string rNorm;
  const TableSchema* rSch = nullptr;
  std::string rAlias;
};

void needMarkAll(NeededMap& need, const std::string& tnorm,
                 const TableSchema& sc) {
  need[tnorm].assign(sc.columns.size(), 1);
}

// Eine Spaltenreferenz ("c" / "t.c") im gegebenen Scope als benoetigt
// markieren. Aufloesung spiegelt Table::colIndex (single) bzw.
// resolveJoinCol (JOIN): unqualifiziert sucht links dann rechts (beidseitig
// vorhanden = ambiguous), qualifiziert matcht Tabellenname oder Alias je
// Seite. Zweifelsfaelle -> alles markieren (Database wirft dann exakt wie
// bisher).
void needAddRef(NeededMap& need, const ScopeSchemas& scope,
                const std::string& ref) {
  std::string pre, col;
  const std::string::size_type dot = ref.find('.');
  if (dot == std::string::npos) {
    pre.clear();
    col = ref;
  } else {
    pre = ref.substr(0, dot);
    col = ref.substr(dot + 1);
  }
  if (!pre.empty()) {
    const std::string f = toLower(pre);
    const bool lm =
        scope.lSch != nullptr &&
        (f == scope.lNorm ||
         (!scope.lAlias.empty() && f == toLower(scope.lAlias)));
    const bool rm =
        scope.hasRight && scope.rSch != nullptr &&
        (f == scope.rNorm ||
         (!scope.rAlias.empty() && f == toLower(scope.rAlias)));
    if (lm && !rm && scope.lSch != nullptr) {
      int idx = schemaColIndex(scope.lSch->columns, col);
      if (idx < 0) {
        needMarkAll(need, scope.lNorm, *scope.lSch);
      } else {
        auto& m = need[scope.lNorm];
        if (m.size() != scope.lSch->columns.size())
          m.assign(scope.lSch->columns.size(), 0);
        m[static_cast<std::size_t>(idx)] = 1;
      }
      return;
    }
    if (rm && !lm && scope.rSch != nullptr) {
      int idx = schemaColIndex(scope.rSch->columns, col);
      if (idx < 0) {
        needMarkAll(need, scope.rNorm, *scope.rSch);
      } else {
        auto& m = need[scope.rNorm];
        if (m.size() != scope.rSch->columns.size())
          m.assign(scope.rSch->columns.size(), 0);
        m[static_cast<std::size_t>(idx)] = 1;
      }
      return;
    }
    // Unbekannter Prefix oder beidseitiger Match (ambiguous): Database wirft;
    // Scope-Seiten voll markieren (harmlos, Query scheitert dort ohnehin).
    if (scope.lSch != nullptr) needMarkAll(need, scope.lNorm, *scope.lSch);
    if (scope.hasRight && scope.rSch != nullptr)
      needMarkAll(need, scope.rNorm, *scope.rSch);
    return;
  }
  const int li =
      scope.lSch != nullptr ? schemaColIndex(scope.lSch->columns, col) : -1;
  const int ri = (scope.hasRight && scope.rSch != nullptr)
                     ? schemaColIndex(scope.rSch->columns, col)
                     : -1;
  if (li >= 0 && ri < 0 && scope.lSch != nullptr) {
    auto& m = need[scope.lNorm];
    if (m.size() != scope.lSch->columns.size())
      m.assign(scope.lSch->columns.size(), 0);
    m[static_cast<std::size_t>(li)] = 1;
    return;
  }
  if (ri >= 0 && li < 0 && scope.rSch != nullptr) {
    auto& m = need[scope.rNorm];
    if (m.size() != scope.rSch->columns.size())
      m.assign(scope.rSch->columns.size(), 0);
    m[static_cast<std::size_t>(ri)] = 1;
    return;
  }
  // Ambiguous (JOIN, beidseitig) oder unbekannt: Database wirft exakt wie
  // bisher; Scope-Seiten voll markieren.
  if (scope.lSch != nullptr) needMarkAll(need, scope.lNorm, *scope.lSch);
  if (scope.hasRight && scope.rSch != nullptr)
    needMarkAll(need, scope.rNorm, *scope.rSch);
}

void needAggArg(NeededMap& need, const ScopeSchemas& scope,
                const std::shared_ptr<AggExpr>& e) {
  if (!e) return;
  if (e->kind == AggExpr::Kind::Column) {
    needAddRef(need, scope, e->column);
    return;
  }
  if (e->kind == AggExpr::Kind::Binary) {
    needAggArg(need, scope, e->left);
    needAggArg(need, scope, e->right);
  }
}

// Direkte Referenzen EINES Select-Knotens sammeln (ohne den Inhalt
// genesteter Subqueries; der wird per Rekursion mit eigenem Scope
// behandelt). ORDER BY auf Ausgabe-Alias/Projektion braucht nichts extra
// (Projektion ist bereits markiert); Ordinalia ebenfalls nicht.
void collectNodeRefs(NeededMap& need,
                     const std::map<std::string, const TableSchema*>& schemas,
                     const SelectStmt& q) {
  ScopeSchemas scope;
  scope.lNorm = Executor::normalizeTable(q.table);
  auto lit = schemas.find(scope.lNorm);
  scope.lSch = (lit != schemas.end()) ? lit->second : nullptr;
  scope.lAlias = q.table_alias;
  scope.hasRight = q.has_join;
  if (q.has_join) {
    scope.rNorm = Executor::normalizeTable(q.join_table);
    auto rit = schemas.find(scope.rNorm);
    scope.rSch = (rit != schemas.end()) ? rit->second : nullptr;
    scope.rAlias = q.join_alias;
  }
  if (scope.lSch == nullptr ||
      (scope.hasRight && scope.rSch == nullptr)) {
    // Unbekannte Tabelle: Database wirft "Tabelle unbekannt"; alle bekannten
    // als benoetigt markieren (reine Absicherung).
    for (const auto& [tn, sc] : schemas) needMarkAll(need, tn, *sc);
  } else if (q.select_all) {
    needMarkAll(need, scope.lNorm, *scope.lSch);
    if (scope.hasRight && scope.rSch != nullptr)
      needMarkAll(need, scope.rNorm, *scope.rSch);
  } else {
    for (const auto& c : q.columns) needAddRef(need, scope, c);
    for (const auto& a : q.aggregates) needAggArg(need, scope, a.arg);
    for (const auto& c : q.where) needAddRef(need, scope, c.column);
    for (const auto& gr : q.where_groups)
      for (const auto& c : gr) needAddRef(need, scope, c.column);
    for (const auto& g : q.group_by) needAddRef(need, scope, g);
    std::vector<std::string> outNames;
    outNames.reserve(q.columns.size() + q.aggregates.size());
    for (std::size_t i = 0; i < q.columns.size(); ++i) {
      std::string al =
          (i < q.column_aliases.size()) ? q.column_aliases[i] : "";
      outNames.push_back(al.empty() ? q.columns[i] : al);
    }
    for (const auto& a : q.aggregates)
      outNames.push_back(a.alias.empty() ? a.display : a.alias);
    for (const auto& o : q.order_by) {
      if (o.is_ordinal) continue;
      if (o.is_agg) {
        needAggArg(need, scope, o.agg.arg);
        continue;
      }
      bool proj = false;
      for (const auto& nm : outNames) {
        if (toLower(nm) == toLower(o.column)) {
          proj = true;
          break;
        }
      }
      if (!proj) needAddRef(need, scope, o.column);
    }
    for (const auto& j : q.join_on) {
      needAddRef(need, scope, j.left);
      if (j.right_is_col) needAddRef(need, scope, j.right);
    }
  }
  auto rec = [&](const Condition& c) {
    if (c.subquery) collectNodeRefs(need, schemas, *c.subquery);
  };
  for (const auto& c : q.where) rec(c);
  for (const auto& gr : q.where_groups)
    for (const auto& c : gr) rec(c);
}

NeededMap computeNeeded(
    const SelectStmt& s,
    const std::map<std::string, const TableSchema*>& schemas) {
  NeededMap need;
  for (const auto& [tn, sc] : schemas)
    need[tn].assign(sc->columns.size(), 0);
  collectNodeRefs(need, schemas, s);
  return need;
}

// ---- Partielle Row-Dekodierung (Codec-kompatibel, kein Format-Bruch) -------
// Gleicher Codec wie decodeRow (Felder mit '|' getrennt, Typ-Praefix "X:",
// S-Payload-Escapes); ein Durchlauf ohne Zwischen-Strings (string_view auf
// der Row-Lebensdauer, keine Kopie des MVCC-Werts).
// mask == nullptr oder mask[f] != 0 -> Feld f voll dekodieren (exakt wie
// decodeRow). Sonst: S-Felder nur Escape-validieren (kein String-Aufbau,
// Wert NULL), I/F/B/N voll dekodieren (keine/winzige Alloks). Feldzahl- und
// Typ-Prefix-Pruefung identisch zu decodeRow, daher werden korrupte Zeilen
// exakt wie bisher uebersprungen (Aufrufer: try/catch -> continue).
std::string unescapeView(std::string_view sv) {
  std::string o;
  o.reserve(sv.size());
  for (std::size_t i = 0; i < sv.size(); ++i) {
    if (sv[i] == '\\' && i + 1 < sv.size()) {
      const char n = sv[i + 1];
      if (n == '\\') {
        o += '\\';
        ++i;
      } else if (n == 'p') {
        o += '|';
        ++i;
      } else if (n == 'n') {
        o += '\n';
        ++i;
      } else if (n == 'r') {
        o += '\r';
        ++i;
      } else {
        throw SqlError("Korrupte Row-Kodierung (Escape)");
      }
    } else if (sv[i] == '\\') {
      throw SqlError("Korrupte Row-Kodierung (trailing backslash)");
    } else {
      o += sv[i];
    }
  }
  return o;
}

void validateEscapes(std::string_view sv) {
  for (std::size_t i = 0; i < sv.size(); ++i) {
    if (sv[i] == '\\' && i + 1 < sv.size()) {
      const char n = sv[i + 1];
      if (n == '\\' || n == 'p' || n == 'n' || n == 'r') {
        ++i;
        continue;
      }
      throw SqlError("Korrupte Row-Kodierung (Escape)");
    } else if (sv[i] == '\\') {
      throw SqlError("Korrupte Row-Kodierung (trailing backslash)");
    }
  }
}

std::vector<Value> decodeRowSelected(const std::string& s, std::size_t ncols,
                                     const std::vector<char>* mask) {
  // Feldgrenzen in einem Durchlauf: "\x" ist eine Einheit, '|' sonst Trenner
  // (exakt wie splitRowFields, aber ohne Feld-Strings zu materialisieren).
  std::vector<std::pair<std::size_t, std::size_t>> b;
  b.reserve(ncols);
  std::size_t start = 0;
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      i += 2;
    } else if (s[i] == '|') {
      b.emplace_back(start, i - start);
      start = i + 1;
      ++i;
    } else {
      ++i;
    }
  }
  b.emplace_back(start, s.size() - start);
  if (b.size() != ncols)
    throw SqlError("Row-Kodierung: Spaltenzahl passt nicht");
  std::vector<Value> out;
  out.reserve(ncols);
  for (std::size_t f = 0; f < ncols; ++f) {
    const char* p = s.data() + b[f].first;
    const std::size_t len = b[f].second;
    if (len < 2 || p[1] != ':') throw SqlError("Korrupte Row-Kodierung");
    const char t = p[0];
    const std::string_view pay(p + 2, len - 2);
    const bool needf =
        (mask == nullptr) || (f >= mask->size()) || ((*mask)[f] != 0);
    switch (t) {
      case 'N':
        out.emplace_back(std::monostate{});
        break;
      case 'I':
        out.emplace_back(static_cast<int64_t>(std::stoll(std::string(pay))));
        break;
      case 'F':
        out.emplace_back(std::stod(std::string(pay)));
        break;
      case 'S':
        if (needf) {
          out.emplace_back(unescapeView(pay));
        } else {
          validateEscapes(pay);
          out.emplace_back(std::monostate{});
        }
        break;
      case 'B':
        if (pay == "1")
          out.emplace_back(true);
        else if (pay == "0")
          out.emplace_back(false);
        else
          throw SqlError("Korrupte Row-Kodierung (BOOL)");
        break;
      default:
        throw SqlError("Korrupte Row-Kodierung (Typ)");
    }
  }
  return out;
}

// ---- RBAC minimal (Rollen-Map pro Executor) ---------------------------------
// executor.h ist fixiert (kein Member, keine setRole-Deklaration moeglich):
// Rolle + Grants leben file-statisch keyed by Executor* (nicht kopierbar ->
// Adresse stabil; Destruktor raeumt den Eintrag auf). Rollenumschaltung via
// SQL (SET ROLE/RESET ROLE, s. parser.h). Semantik wie Database (parser.cpp):
// "" = Admin (Default, alles erlaubt), DML braucht das jeweilige Privileg,
// CREATE/DROP/GRANT/REVOKE nur als Admin, ohne Recht -> SQLSTATE 42501.
// WAL-Opcodes 'G' (GRANT) / 'R' (REVOKE); Replay stellt die Rechte wieder
// her; Mirror-Sidecar ist KV-only -> G/R dort No-Op (Watermark darf vor).
struct RbacState {
  std::string role;  // "" = Admin
  // norm-Tabelle -> Rolle -> Privilegien (upper).
  std::map<std::string, std::map<std::string, std::set<std::string>>> grants;
};
std::map<const Executor*, RbacState> g_rbac;

RbacState& rbacFor(const Executor* self) { return g_rbac[self]; }

bool rbacHas(const RbacState& st, const std::string& normTable,
             const std::string& priv) {
  if (st.role.empty()) return true;
  auto tit = st.grants.find(normTable);
  if (tit == st.grants.end()) return false;
  auto rit = tit->second.find(st.role);
  if (rit == tit->second.end()) return false;
  return rit->second.count(priv) > 0;
}

void rbacRequire(const RbacState& st, const std::string& normTable,
                 const std::string& priv, const std::string& display) {
  if (!rbacHas(st, normTable, priv))
    throw SqlError("permission denied for table " + display +
                   " (SQLSTATE 42501)");
}

void rbacRequireAdmin(const RbacState& st, const std::string& what) {
  if (!st.role.empty())
    throw SqlError("permission denied (" + what +
                   " requires admin role, SQLSTATE 42501)");
}

bool rbacPrivValid(const std::string& p) {
  return p == "SELECT" || p == "INSERT" || p == "UPDATE" || p == "DELETE";
}

// "A,B,C" -> validierte Privilegien (unbekannt -> SqlError -> Skip im Replay).
std::vector<std::string> rbacSplitPrivs(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      if (!rbacPrivValid(cur)) throw SqlError("Korrupter GRANT-Record");
      if (std::find(out.begin(), out.end(), cur) == out.end())
        out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!rbacPrivValid(cur)) throw SqlError("Korrupter GRANT-Record");
  if (std::find(out.begin(), out.end(), cur) == out.end()) out.push_back(cur);
  return out;
}

}  // namespace

Executor::Executor(kv::KVStore& kv, txn::MvccStore& mvcc, storage::Wal* wal)
    : kv_(kv), mvcc_(mvcc), wal_(wal) {}

Executor::~Executor() {
  disableMirror();
  g_rbac.erase(this);
}

// ---- Mirror-Checkpoint: Format + Protokoll ----------------------------------
// Spiegel-Format (BTreeKV-Sidecar, geordnetes Latest-State-Abbild):
//   - Row-/Schema-Keys 1:1 wie im KVStore: "sql/<tabelle>/<pk>" -> Row-Codec,
//     "sql/__schema/<tabelle>" -> Schema-Codec (kein Format-Bruch, dieselben
//     Codec-Helper wie live).
//   - Meta-Key "\0mirror/lsn" (fuehrendes NUL: kann mit keinem "sql/..."-Key
//     kollidieren, faellt aus keinem Prefix-Scan) -> dezimale durable-LSN, bis
//     zu der der Spiegel den WAL 1:1 abbildet (mirror_lsn).
// Inkrementell-Protokoll: Statements spiegeln ihren gerade geschriebenen
// Batch via syncMirrorBatch (O(Batch), kein WAL-Re-Read); recover() nutzt
// syncMirrorFromWal (Vollscan, selten) als Catch-up.
// Reihenfolge auf den Spiegel an (Put/Delete 1:1, DROP = Prefix-Delete +
// Schema-Delete); danach mirror_lsn = durable in EINEM BTreeKV-Flush
// (Shadow-Paging -> atomar: Crash davor = alter Spiegel + alte LSN =
// konsistent, Tail-Replay holt den Rest).
// Sichtbarkeits-Regel beim Laden: Rows werden als frische Single-Version-
// Ketten unter je einem Chunk-Commit installiert (committed, trx_end = INF);
// im frischen Prozess ohne aktive Txns ist jede committed Version fuer jeden
// zukuenftigen Snapshot sichtbar (trx_begin <= snapshot). Tombstones/DROP-
// States existieren im Spiegel nicht als Historie, sondern als Abwesenheit
// (Latest-State) -> identische Sichtbarkeit wie Voll-Replay ohne aktive Txns.
namespace {
constexpr std::string_view kMirrorLsnKey{"\0mirror/lsn", 11};
constexpr std::string_view kMirrorMetaPrefix{"\0mirror/", 8};
constexpr std::string_view kMirrorSchemaPrefix{"sql/__schema/", 13};
constexpr std::string_view kMirrorSqlPrefix{"sql/", 4};
constexpr std::size_t kMirrorLoadChunk = 4096;
}  // namespace

bool Executor::enableMirror(const std::string& path, std::string* warn) {
  auto note = [&](const std::string& m) {
    if (warn != nullptr) {
      if (!warn->empty()) *warn += "; ";
      *warn += m;
    }
  };
  try {
    auto bt = std::make_unique<kv::BTreeKV>();
    if (!bt->Open(path)) {
      note("Spiegel '" + path + "' nicht oeffbar");
      return false;
    }
    std::uint64_t lsn = 0;
    bool degraded = false;
    if (auto v = bt->Get(std::string(kMirrorLsnKey))) {
      try {
        lsn = std::stoull(*v);
      } catch (...) {
        degraded = true;
        note("Spiegel-LSN korrupt");
        lsn = 0;
      }
    }
    try {
      const auto all = bt->Scan("");
      // Pass 1: Schemas zuerst (Rows brauchen bekannte Tabellen, wie recover).
      std::vector<std::pair<std::string, std::string>> rows;
      rows.reserve(all.size());
      for (const auto& [k, v] : all) {
        const std::string_view kvw(k);
        if (kvw.starts_with(kMirrorMetaPrefix)) continue;
        if (kvw.starts_with(kMirrorSchemaPrefix)) {
          try {
            applyCreateRecord(
                std::string(kvw.substr(kMirrorSchemaPrefix.size())), v);
          } catch (...) {
            degraded = true;
            note("korrupter Schema-Eintrag im Spiegel");
          }
        } else if (kvw.starts_with(kMirrorSqlPrefix)) {
          rows.emplace_back(k, v);
        } else {
          degraded = true;  // fremder Key: sicherheitshalber Voll-Replay
          note("fremder Key im Spiegel");
        }
      }
      // Pass 2: nur Rows bekannter Tabellen laden (recover skipt INSERTs ohne
      // Schema; Phantom-Rows duerfen bei spaeterem CREATE nicht erscheinen).
      std::vector<std::string> prefixes;
      prefixes.reserve(tables_.size());
      for (const auto& [t, sch] : tables_) {
        (void)sch;
        prefixes.push_back(tablePrefix(t));
      }
      std::vector<std::pair<std::string, std::string>> owned;
      owned.reserve(rows.size());
      for (auto& [k, v] : rows) {
        bool known = false;
        for (const auto& p : prefixes) {
          if (k.rfind(p, 0) == 0) {
            known = true;
            break;
          }
        }
        if (known) owned.emplace_back(std::move(k), std::move(v));
      }
      for (std::size_t b = 0; b < owned.size() && !degraded;
           b += kMirrorLoadChunk) {
        const std::size_t e = std::min(b + kMirrorLoadChunk, owned.size());
        kv::WriteBatch batch;
        for (std::size_t i = b; i < e; ++i)
          batch.Put(owned[i].first, owned[i].second);
        if (!kv_.Write(batch)) {
          degraded = true;
          note("Spiegel-Rows nicht in KV ladbar");
          break;
        }
        txn::Transaction w = mvcc_.BeginWriteBlocking();
        bool ok = true;
        for (std::size_t i = b; i < e; ++i) {
          if (!mvcc_.Write(w, owned[i].first, owned[i].second)) {
            ok = false;
            break;
          }
        }
        if (ok) ok = mvcc_.Commit(w);
        if (!ok) {
          mvcc_.Abort(w);
          degraded = true;
          note("Spiegel-Rows nicht in MVCC ladbar");
          break;
        }
      }
      for (const auto& [t, sch] : tables_) {
        (void)sch;
        rebuildReplicaForTable(t);
      }
    } catch (...) {
      degraded = true;
      note("Spiegel-Scan fehlgeschlagen");
    }
    // Degradiert: mirror_lsn = 0 -> Voll-Replay; WAL-Truth (last-wins)
    // ueberschreibt jeden geladenen Spiegel-Stand -> nie Datenverlust.
    if (degraded) lsn = 0;
    mirror_ = std::move(bt);
    mirror_path_ = path;
    mirror_lsn_ = lsn;
    mirror_on_ = true;
    return true;
  } catch (...) {
    note("Spiegel-Aktivierung fehlgeschlagen");
    return false;
  }
}

bool Executor::mirrorCheckpoint() {
  if (!mirror_on_ || !mirror_ || wal_ == nullptr) return false;
  try {
    if (!mirror_->IsOpen()) return false;
    std::uint64_t durable = 0;
    try {
      durable = wal_->durable_lsn();
    } catch (...) {
      return false;
    }
    if (durable <= mirror_lsn_) {
      mirror_pending_ = 0;
      return true;
    }
    if (!mirror_->Put(std::string(kMirrorLsnKey), std::to_string(durable)))
      return false;
    if (!mirror_->Flush()) return false;
    mirror_lsn_ = durable;
    mirror_pending_ = 0;
    return true;
  } catch (...) {
    return false;
  }
}

void Executor::disableMirror() {  try {
    if (mirror_) mirror_->Close();  // best effort Flush (BTreeKV::Close)
  } catch (...) {
  }
  mirror_.reset();
  mirror_path_.clear();
  mirror_lsn_ = 0;
  mirror_on_ = false;
}

bool Executor::applyMirrorRecord(const std::string& data) {
  auto parts = splitWal(data);
  if (parts.empty()) return true;  // recover skipt -> kein State -> Advance ok
  const std::string schemaPfx(kMirrorSchemaPrefix);
  if (parts[0] == "C" && parts.size() == 3) {
    try {
      (void)schemaDecode(parts[2]);  // validieren wie recover
    } catch (...) {
      return true;
    }
    return mirror_->Put(schemaPfx + normalizeTable(parts[1]), parts[2]);
  }
  if ((parts[0] == "I" || parts[0] == "U") && parts.size() == 4) {
    const std::string norm = normalizeTable(parts[1]);
    if (!mirror_->Get(schemaPfx + norm).has_value()) return true;  // Skip wie recover
    std::string key;
    try {
      key = walUnescape(parts[2]);
    } catch (...) {
      return true;
    }
    return mirror_->Put(key, parts[3]);
  }
  if (parts[0] == "D" && parts.size() == 3) {
    const std::string norm = normalizeTable(parts[1]);
    if (!mirror_->Get(schemaPfx + norm).has_value()) return true;  // Skip wie recover
    std::string key;
    try {
      key = walUnescape(parts[2]);
    } catch (...) {
      return true;
    }
    (void)mirror_->Delete(key);  // missing = idempotenter No-Op
    return true;
  }
  if (parts[0] == "T" && parts.size() == 2) {
    const std::string norm = normalizeTable(parts[1]);
    const std::string pfx = tablePrefix(norm);
    for (const auto& [k, v] : mirror_->Scan(pfx)) {
      (void)v;
      (void)mirror_->Delete(k);
    }
    (void)mirror_->Delete(schemaPfx + norm);  // DROP: Spiegel-Eintraege weg
    return true;
  }
  if ((parts[0] == "G" || parts[0] == "R") && parts.size() == 4) {
    return true;  // RBAC: Rechte sind WAL-persistiert, Spiegel ist KV-only
  }
  return true;  // unbekannter Opcode: recover skipt -> kein State -> Advance ok
}

void Executor::syncMirrorFromWal() { syncMirrorBatch(nullptr); }

void Executor::syncMirrorBatch(const std::vector<std::string>* batch) {
  if (!mirror_on_ || !mirror_ || wal_ == nullptr) return;
  try {
    if (!mirror_->IsOpen()) return;
    std::uint64_t durable = 0;
    try {
      durable = wal_->durable_lsn();
    } catch (...) {
      return;
    }
    if (durable <= mirror_lsn_) return;
    bool from_wal = (batch == nullptr);
    std::vector<storage::WalRecord> recs;
    if (from_wal) {
      try {
        recs = wal_->read_from(mirror_lsn_ + 1);  // NUR neue Records
      } catch (...) {
        return;
      }
    }
    bool all_ok = true;
    if (from_wal) {
      for (auto& r : recs) {
        try {
          if (!applyMirrorRecord(r.data)) all_ok = false;
        } catch (...) {
          all_ok = false;
        }
      }
    } else {
      for (auto& p : *batch) {
        try {
          if (!applyMirrorRecord(p)) all_ok = false;
        } catch (...) {
          all_ok = false;
        }
      }
    }
    if (all_ok) {
      // Watermark rueckt nur mit geflushtem Spiegel vor. Flush ist teuer
      // (B-Tree-Voll-Rewrite), daher gebatcht: alle mirror_interval_
      // Statements; Crash davor replayt den Tail erneut (idempotent).
      if (++mirror_pending_ >= mirror_interval_) {
        bool ok = false;
        try {
          ok = mirror_->Put(std::string(kMirrorLsnKey),
                            std::to_string(durable));
          if (ok) ok = mirror_->Flush();
        } catch (...) {
          ok = false;
        }
        if (ok) {
          mirror_lsn_ = durable;  // atomar geflusht
          mirror_pending_ = 0;
        }
      }
    } else {
      // Teilsync crashfest machen; Watermark bleibt alt -> Restart replayt
      // den Tail erneut (last-wins, idempotent).
      try {
        (void)mirror_->Flush();
      } catch (...) {
      }
    }
  } catch (...) {
    // Spiegel darf Writes niemals scheitern lassen.
  }
}

std::string Executor::normalizeTable(const std::string& table) {
  return toLower(table);
}

std::string Executor::tablePrefix(const std::string& table) {
  return "sql/" + normalizeTable(table) + "/";
}

std::string Executor::schemaEncode(const std::vector<ColumnDef>& cols) {
  std::string o;
  for (std::size_t i = 0; i < cols.size(); ++i) {
    if (i) o += ',';
    o += cols[i].name;
    o += ':';
    o += colTypeToString(cols[i].type);
  }
  return o;
}

std::vector<ColumnDef> Executor::schemaDecode(const std::string& s) {
  std::vector<ColumnDef> out;
  std::string cur;
  auto flush = [&](const std::string& field) {
    if (field.empty()) throw SqlError("Korrupte Schema-Kodierung");
    auto pos = field.find(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= field.size())
      throw SqlError("Korrupte Schema-Kodierung");
    ColumnDef c;
    c.name = field.substr(0, pos);
    c.type = colTypeFromString(field.substr(pos + 1));
    out.push_back(std::move(c));
  };
  for (char c : s) {
    if (c == ',') {
      flush(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  flush(cur);
  if (out.empty()) throw SqlError("Korrupte Schema-Kodierung (leer)");
  return out;
}

std::string Executor::encodeRow(const std::vector<Value>& row) {
  std::string o;
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (i) o += '|';
    const Value& v = row[i];
    if (std::holds_alternative<std::monostate>(v)) {
      o += "N:";
    } else if (auto* iv = std::get_if<int64_t>(&v)) {
      o += "I:";
      o += std::to_string(*iv);
    } else if (auto* dv = std::get_if<double>(&v)) {
      std::ostringstream oss;
      oss << std::setprecision(17) << *dv;
      o += "F:";
      o += oss.str();
    } else if (auto* sv = std::get_if<std::string>(&v)) {
      o += "S:";
      o += escapeField(*sv);
    } else if (auto* bv = std::get_if<bool>(&v)) {
      o += "B:";
      o += (*bv ? "1" : "0");
    } else {
      throw SqlError("Unkodierbarer Wert");
    }
  }
  return o;
}

std::vector<Value> Executor::decodeRow(const std::string& s, std::size_t ncols) {
  auto fields = splitRowFields(s);
  if (fields.size() != ncols)
    throw SqlError("Row-Kodierung: Spaltenzahl passt nicht");
  std::vector<Value> out;
  out.reserve(ncols);
  for (auto& f : fields) {
    if (f.size() < 2 || f[1] != ':') throw SqlError("Korrupte Row-Kodierung");
    char t = f[0];
    std::string p = f.substr(2);
    switch (t) {
      case 'N':
        out.emplace_back(std::monostate{});
        break;
      case 'I':
        out.emplace_back(static_cast<int64_t>(std::stoll(p)));
        break;
      case 'F':
        out.emplace_back(std::stod(p));
        break;
      case 'S':
        out.emplace_back(unescapeField(p));
        break;
      case 'B':
        if (p == "1")
          out.emplace_back(true);
        else if (p == "0")
          out.emplace_back(false);
        else
          throw SqlError("Korrupte Row-Kodierung (BOOL)");
        break;
      default:
        throw SqlError("Korrupte Row-Kodierung (Typ)");
    }
  }
  return out;
}

// ---- pg_stat_statements-light ------------------------------------------------
// Session-lokal, keine Persistenz: eine Map (normalisierter Text -> QueryStat)
// + ein steady_clock-Zeitstempel pro execute(). Overhead pro Statement: eine
// Normalisierung (O(n) ueber den SQL-String), ein chrono-Paar und genau ein
// Map-Lookup im Erfolgs-/Fehlerpfad; keine Alloks ausser dem normalisierten
// Key (einmalig je distinktem Query) und keine Locks ausser einem kurzen
// lock_guard beim Aktualisieren.
std::string Executor::normalizeQuery(const std::string& sql) {
  // Einfache Normalisierung: Single-quoted Strings ('...' mit ''-Escape) und
  // Double-quoted Stuecke ("..." mit ""-Escape) -> '?', Zahlen -> '?',
  // Linien-/Blockkommentare -> Space, Whitespace kollabiert + getrimmt,
  // trailing ';' entfernt. Keywords/Bezeichner bleiben unveraendert
  // (kein Lowercasing). Nie werfend (reine String-Ops).
  try {
    std::string out;
    out.reserve(sql.size());
    const std::size_t n = sql.size();
    std::size_t i = 0;
    auto prevOut = [&]() -> char { return out.empty() ? '\0' : out.back(); };
    while (i < n) {
      const char c = sql[i];
      // Linienkommentar -- bis EOL.
      if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
        i += 2;
        while (i < n && sql[i] != '\n') ++i;
        if (!out.empty() && out.back() != ' ') out += ' ';
        continue;
      }
      // Blockkommentar /* ... */ (unabgeschlossen -> Rest ist Kommentar).
      if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
        i += 2;
        while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
        if (i + 1 < n) i += 2;
        else i = n;
        if (!out.empty() && out.back() != ' ') out += ' ';
        continue;
      }
      // Single-quoted String -> '?'.
      if (c == '\'') {
        out += '?';
        ++i;
        while (i < n) {
          if (sql[i] == '\'') {
            if (i + 1 < n && sql[i + 1] == '\'') {
              i += 2;
              continue;
            }
            ++i;
            break;
          }
          ++i;
        }
        continue;
      }
      // Double-quoted Stueck (Identifier/Literal) -> '?'.
      if (c == '"') {
        out += '?';
        ++i;
        while (i < n) {
          if (sql[i] == '"') {
            if (i + 1 < n && sql[i + 1] == '"') {
              i += 2;
              continue;
            }
            ++i;
            break;
          }
          ++i;
        }
        continue;
      }
      // Zahl ab Ziffer (nicht Teil eines Identifiers): int/float/exp -> '?'.
      // Zusaetzlich .<digit> (fuehrender Punkt) -> '?'.
      const bool prevIsIdent =
          !out.empty() && (std::isalnum(static_cast<unsigned char>(prevOut())) ||
                           prevOut() == '_' || prevOut() == '.');
      const bool atNum =
          std::isdigit(static_cast<unsigned char>(c)) != 0 && !prevIsIdent;
      const bool atDotNum = c == '.' && i + 1 < n &&
                            std::isdigit(static_cast<unsigned char>(sql[i + 1])) != 0 &&
                            !prevIsIdent;
      if (atNum || atDotNum) {
        std::size_t j = i;
        bool dot = false;
        while (j < n && (std::isdigit(static_cast<unsigned char>(sql[j])) != 0 ||
                         (!dot && sql[j] == '.'))) {
          if (sql[j] == '.') dot = true;
          ++j;
        }
        if (j < n && (sql[j] == 'e' || sql[j] == 'E')) {
          std::size_t k = j + 1;
          if (k < n && (sql[k] == '+' || sql[k] == '-')) ++k;
          const std::size_t k0 = k;
          while (k < n && std::isdigit(static_cast<unsigned char>(sql[k])) != 0)
            ++k;
          if (k > k0) j = k;
        }
        out += '?';
        i = j;
        continue;
      }
      if (std::isspace(static_cast<unsigned char>(c)) != 0) {
        if (!out.empty() && out.back() != ' ') out += ' ';
        ++i;
        continue;
      }
      out += c;
      ++i;
    }
    // Trim + trailing ';' entfernen.
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.back() == ';') {
      out.pop_back();
      while (!out.empty() && out.back() == ' ') out.pop_back();
    }
    std::size_t b = 0;
    while (b < out.size() && out[b] == ' ') ++b;
    if (b > 0) out.erase(0, b);
    return out;
  } catch (...) {
    return sql;
  }
}

std::vector<QueryStat> Executor::queryStats(std::size_t top_n) const {
  std::lock_guard<std::mutex> lk(pgstat_mu_);
  std::vector<QueryStat> v;
  v.reserve(pgstat_.size());
  for (const auto& [k, st] : pgstat_) {
    (void)k;
    v.push_back(st);
  }
  std::sort(v.begin(), v.end(), [](const QueryStat& a, const QueryStat& b) {
    if (a.total_ms != b.total_ms) return a.total_ms > b.total_ms;
    if (a.calls != b.calls) return a.calls > b.calls;
    return a.query < b.query;
  });
  if (top_n != 0 && v.size() > top_n) v.resize(top_n);
  return v;
}

void Executor::clearQueryStats() {
  std::lock_guard<std::mutex> lk(pgstat_mu_);
  pgstat_.clear();
}

Result Executor::execute(const std::string& sql) {
  // Einstiegspunkt aller Statements: genau ein chrono-Paar + genau ein
  // Map-Lookup pro Aufruf (Erfolg oder Fehler). Fehler (parse/RBAC/exec)
  // zaehlen calls + errors, Erfolge calls + rows_out (rows.size()+affected).
  std::string key = normalizeQuery(sql);
  const auto t0 = std::chrono::steady_clock::now();
  try {
    Result r = executeInner(sql);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    const std::uint64_t n =
        static_cast<std::uint64_t>(r.rows.size()) +
        static_cast<std::uint64_t>(r.affected);
    {
      std::lock_guard<std::mutex> lk(pgstat_mu_);
      QueryStat& e = pgstat_[key];  // genau ein Lookup
      e.query = key;
      ++e.calls;
      e.total_ms += ms;
      e.rows_out += n;
    }
    return r;
  } catch (...) {
    const auto t1 = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    {
      std::lock_guard<std::mutex> lk(pgstat_mu_);
      QueryStat& e = pgstat_[key];  // genau ein Lookup
      e.query = key;
      ++e.calls;
      e.total_ms += ms;
      ++e.errors;
    }
    throw;
  }
}

Result Executor::executeInner(const std::string& sql) {
  Statement st = parseStatement(sql);
  // ---- RBAC: GRANT/REVOKE/SET-Handling + Enforcement vor jeder
  // KV/MVCC/WAL-Seiteneffekt (fail fast, keine Halb-Writes bei 42501).
  {
    RbacState& rs = rbacFor(this);
    if (std::holds_alternative<GrantStmt>(st)) {
      const GrantStmt& g = std::get<GrantStmt>(st);
      rbacRequireAdmin(rs, "GRANT");
      const std::string norm = normalizeTable(g.table);
      if (tables_.find(norm) == tables_.end())
        throw SqlError("Tabelle unbekannt: " + g.table);
      for (const auto& p : g.privs) rs.grants[norm][g.role].insert(p);
      if (wal_ != nullptr) {
        std::string payload;
        payload += 'G';
        payload += kWalSep;
        payload += norm;
        payload += kWalSep;
        payload += walEscape(g.role);
        payload += kWalSep;
        for (std::size_t i = 0; i < g.privs.size(); ++i) {
          if (i) payload += ',';
          payload += g.privs[i];
        }
        wal_->append(payload);
        wal_->flush();
        const std::vector<std::string> mirror_batch{payload};
        syncMirrorBatch(&mirror_batch);
      }
      return {{}, {}, "GRANT", 0};
    }
    if (std::holds_alternative<RevokeStmt>(st)) {
      const RevokeStmt& g = std::get<RevokeStmt>(st);
      rbacRequireAdmin(rs, "REVOKE");
      const std::string norm = normalizeTable(g.table);
      if (tables_.find(norm) == tables_.end())
        throw SqlError("Tabelle unbekannt: " + g.table);
      auto tit = rs.grants.find(norm);
      if (tit != rs.grants.end()) {
        auto rit = tit->second.find(g.role);
        if (rit != tit->second.end()) {
          for (const auto& p : g.privs) rit->second.erase(p);
          if (rit->second.empty()) tit->second.erase(rit);
        }
        if (tit->second.empty()) rs.grants.erase(tit);
      }
      if (wal_ != nullptr) {
        std::string payload;
        payload += 'R';
        payload += kWalSep;
        payload += norm;
        payload += kWalSep;
        payload += walEscape(g.role);
        payload += kWalSep;
        for (std::size_t i = 0; i < g.privs.size(); ++i) {
          if (i) payload += ',';
          payload += g.privs[i];
        }
        wal_->append(payload);
        wal_->flush();
        const std::vector<std::string> mirror_batch{payload};
        syncMirrorBatch(&mirror_batch);
      }
      return {{}, {}, "REVOKE", 0};
    }
    if (std::holds_alternative<SetRoleStmt>(st)) {
      const SetRoleStmt& sr = std::get<SetRoleStmt>(st);
      // sr.role aus parseRole (unquoted gefoldet, quoted exakt); kein Check
      // (sonst koennte eine Rolle nie zurueckwechseln). Session-lokal, kein WAL.
      rs.role = sr.reset ? "" : sr.role;
      return {{}, {}, sr.reset ? "RESET ROLE" : "SET ROLE", 0};
    }
    if (std::holds_alternative<CreateTableStmt>(st)) {
      rbacRequireAdmin(rs, "CREATE TABLE");
    } else if (std::holds_alternative<DropTableStmt>(st)) {
      rbacRequireAdmin(rs, "DROP TABLE");
    } else if (std::holds_alternative<InsertStmt>(st)) {
      const InsertStmt& q = std::get<InsertStmt>(st);
      const std::string norm = normalizeTable(q.table);
      if (tables_.find(norm) != tables_.end())
        rbacRequire(rs, norm, "INSERT", q.table);
      // Unbekannt meldet execInsert ("Tabelle unbekannt").
    } else if (std::holds_alternative<SelectStmt>(st)) {
      const SelectStmt& q = std::get<SelectStmt>(st);
      std::set<std::string> needed;
      collectSelectTables(q, needed);
      // Unbekannte Tabellen ueberspringen (execSelect meldet sie exakt).
      for (const auto& tn : needed) {
        auto jt = tables_.find(tn);
        if (jt == tables_.end()) continue;
        const std::string disp =
            (tn == normalizeTable(q.table)) ? q.table : tn;
        rbacRequire(rs, tn, "SELECT", disp);
      }
    } else if (std::holds_alternative<UpdateStmt>(st)) {
      const UpdateStmt& q = std::get<UpdateStmt>(st);
      const std::string norm = normalizeTable(q.table);
      if (tables_.find(norm) != tables_.end()) {
        rbacRequire(rs, norm, "UPDATE", q.table);
        std::set<std::string> needed;
        collectWhereTables(q.where, q.where_groups, needed);
        needed.erase(norm);
        for (const auto& tn : needed) {
          if (tables_.find(tn) == tables_.end()) continue;
          rbacRequire(rs, tn, "SELECT", tn);
        }
      }
    } else if (std::holds_alternative<DeleteStmt>(st)) {
      const DeleteStmt& q = std::get<DeleteStmt>(st);
      const std::string norm = normalizeTable(q.table);
      if (tables_.find(norm) != tables_.end()) {
        rbacRequire(rs, norm, "DELETE", q.table);
        std::set<std::string> needed;
        collectWhereTables(q.where, q.where_groups, needed);
        needed.erase(norm);
        for (const auto& tn : needed) {
          if (tables_.find(tn) == tables_.end()) continue;
          rbacRequire(rs, tn, "SELECT", tn);
        }
      }
    }
  }
  if (std::holds_alternative<CreateTableStmt>(st))
    return execCreate(std::get<CreateTableStmt>(st));
  if (std::holds_alternative<InsertStmt>(st))
    return execInsert(std::get<InsertStmt>(st));
  if (std::holds_alternative<UpdateStmt>(st)) {
    // UPDATE: sichtbare Zeilen matchen (Snapshot wie execSelect), Treffer als
    // neue Vollzeilen schreiben (KV-Put + neue MVCC-Version, alte Version via
    // trx_end abgeloest). WAL zuerst (Opcode 'U'), dann KV-Batch, dann
    // MVCC-Commit (eine Writer-Txn). Reads unveraendert (Snapshot-Semantik).
    const UpdateStmt& u = std::get<UpdateStmt>(st);
    const std::string norm = normalizeTable(u.table);
    auto it = tables_.find(norm);
    if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + u.table);
    const TableSchema& sch = it->second;
    std::vector<int> setIdx;
    setIdx.reserve(u.sets.size());
    for (const auto& [col, val] : u.sets) {
      (void)val;
      int idx = schemaColIndex(sch.columns, col);
      if (idx < 0) throw SqlError("Unbekannte Spalte: " + col);
      setIdx.push_back(idx);
    }
    Table shadow;
    shadow.columns = sch.columns;
    (void)shadow;
    // Subquery-Tabellen im selben Snapshot mitladen (fuer WHERE-Matching).
    std::set<std::string> needed;
    collectWhereTables(u.where, u.where_groups, needed);
    needed.erase(norm);
    auto snap = kv_.GetSnapshot();
    txn::Transaction rtxn = mvcc_.BeginRead();
    std::vector<std::pair<std::string, std::vector<Value>>> vis;
    for (const auto& [k, v] : snap->Scan(tablePrefix(norm))) {
      (void)v;  // Key-Menge aus KV, Wert NUR aus MVCC (kein Dirty-Read)
      auto mv = mvcc_.Read(rtxn, k);
      if (!mv.has_value()) continue;  // Tombstone/ung committed -> unsichtbar
      try {
        vis.emplace_back(k, decodeRow(*mv, sch.columns.size()));
      } catch (...) {
        continue;
      }
    }
    std::map<std::string, std::vector<std::vector<Value>>> extraRows;
    std::map<std::string, const TableSchema*> extraSch;
    for (auto& tn : needed) {
      auto jt = tables_.find(tn);
      if (jt == tables_.end()) continue;  // tmp.execSelect wirft "unbekannt"
      extraSch[tn] = &jt->second;
      std::vector<std::vector<Value>> rows;
      for (const auto& [k, v] : snap->Scan(tablePrefix(tn))) {
        (void)v;
        auto mv = mvcc_.Read(rtxn, k);
        if (!mv.has_value()) continue;
        try {
          rows.push_back(decodeRow(*mv, jt->second.columns.size()));
        } catch (...) {
          continue;
        }
      }
      extraRows[tn] = std::move(rows);
    }
    mvcc_.Commit(rtxn);
    // Matching erst nach Commit des Read-Snapshots (Fehler aus WHERE
    // hinterlassen keine offene Read-Txn): via tmp-SELECT (Subqueries einmal
    // aufgeloest), Rueckabbildung auf Keys per Wertevergleich.
    Database tmp;
    tmp.execCreate(CreateTableStmt{u.table, sch.columns, false});
    if (!vis.empty()) {
      InsertStmt ins;
      ins.table = u.table;
      ins.rows.reserve(vis.size());
      for (auto& [k, row] : vis) ins.rows.push_back(row);
      tmp.execInsert(ins);
    }
    for (auto& [tn, schp] : extraSch) {
      tmp.execCreate(CreateTableStmt{tn, schp->columns, false});
      auto& rows = extraRows[tn];
      if (!rows.empty()) {
        InsertStmt ins;
        ins.table = tn;
        ins.rows = rows;
        tmp.execInsert(ins);
      }
    }
    SelectStmt sel;
    sel.table = u.table;
    sel.select_all = true;
    sel.where = u.where;
    sel.where_groups = u.where_groups;
    Result matched = tmp.execSelect(sel);
    auto isMatch = [&](const std::vector<Value>& row) {
      for (auto& m : matched.rows) {
        if (m.size() != row.size()) continue;
        bool eq = true;
        for (std::size_t i = 0; i < row.size(); ++i) {
          if (!valueEquals(m[i], row[i])) {
            eq = false;
            break;
          }
        }
        if (eq) return true;
      }
      return false;
    };
    std::vector<std::pair<std::string, std::string>> hits;  // (key, newEnc)
    for (auto& [k, row] : vis) {
      if (!isMatch(row)) continue;
      for (std::size_t i = 0; i < u.sets.size(); ++i) {
        const std::size_t ti = static_cast<std::size_t>(setIdx[i]);
        row[ti] = coerceValue(u.sets[i].second, sch.columns[ti].type,
                              sch.columns[ti].name);
      }
      hits.emplace_back(k, encodeRow(row));
    }
    if (hits.empty()) return {{}, {}, "UPDATE 0", 0};
    if (wal_ != nullptr) {
      std::vector<std::string> mirror_batch;
      mirror_batch.reserve(hits.size());
      for (const auto& [k, enc] : hits) {
        std::string payload;
        payload += 'U';
        payload += kWalSep;
        payload += norm;
        payload += kWalSep;
        payload += walEscape(k);
        payload += kWalSep;
        payload += enc;
        wal_->append(payload);
        mirror_batch.push_back(std::move(payload));
      }
      wal_->flush();
      syncMirrorBatch(&mirror_batch);
    }
    {
      kv::WriteBatch batch;
      for (const auto& [k, enc] : hits) batch.Put(k, enc);
      if (!kv_.Write(batch)) throw SqlError("KV-Write fehlgeschlagen");
    }
    {
      txn::Transaction w = mvcc_.BeginWriteBlocking();
      for (const auto& [k, enc] : hits) {
        if (!mvcc_.Write(w, k, enc)) {
          mvcc_.Abort(w);
          throw SqlError("MVCC-Write fehlgeschlagen");
        }
      }
      if (!mvcc_.Commit(w)) throw SqlError("MVCC-Commit fehlgeschlagen");
    }
    // HTAP: committed Vollzeilen als neue Replika-Versionen spiegeln (alte
    // Versionen werden per end_ts geschlossen -> alte Snapshots intakt).
    if (!hits.empty()) {
      const std::uint64_t cts = commitTsOf(hits[0].first);
      if (cts == 0) {
        rebuildReplicaForTable(norm);
      } else {
        for (const auto& [k, enc] : hits) mirrorUpsertOne(norm, k, enc, cts);
      }
    }
    return {{},
            {},
            "UPDATE " + std::to_string(hits.size()),
            hits.size()};
  }
  if (std::holds_alternative<DeleteStmt>(st)) {
    // DELETE: Treffer per Tombstone loeschen (MVCC-Erase -> deleted Version,
    // SELECT unsichtbar, COUNT sinkt) + KV-Key hart entfernen. WAL zuerst
    // (Opcode 'D'), dann KV-Batch, dann MVCC-Commit.
    const DeleteStmt& d = std::get<DeleteStmt>(st);
    const std::string norm = normalizeTable(d.table);
    auto it = tables_.find(norm);
    if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + d.table);
    const TableSchema& sch = it->second;
    Table shadow;
    shadow.columns = sch.columns;
    (void)shadow;
    std::set<std::string> needed;
    collectWhereTables(d.where, d.where_groups, needed);
    needed.erase(norm);
    auto snap = kv_.GetSnapshot();
    txn::Transaction rtxn = mvcc_.BeginRead();
    std::vector<std::pair<std::string, std::vector<Value>>> vis;
    for (const auto& [k, v] : snap->Scan(tablePrefix(norm))) {
      (void)v;
      auto mv = mvcc_.Read(rtxn, k);
      if (!mv.has_value()) continue;
      try {
        vis.emplace_back(k, decodeRow(*mv, sch.columns.size()));
      } catch (...) {
        continue;
      }
    }
    std::map<std::string, std::vector<std::vector<Value>>> extraRows;
    std::map<std::string, const TableSchema*> extraSch;
    for (auto& tn : needed) {
      auto jt = tables_.find(tn);
      if (jt == tables_.end()) continue;
      extraSch[tn] = &jt->second;
      std::vector<std::vector<Value>> rows;
      for (const auto& [k, v] : snap->Scan(tablePrefix(tn))) {
        (void)v;
        auto mv = mvcc_.Read(rtxn, k);
        if (!mv.has_value()) continue;
        try {
          rows.push_back(decodeRow(*mv, jt->second.columns.size()));
        } catch (...) {
          continue;
        }
      }
      extraRows[tn] = std::move(rows);
    }
    mvcc_.Commit(rtxn);
    Database tmp;
    tmp.execCreate(CreateTableStmt{d.table, sch.columns, false});
    if (!vis.empty()) {
      InsertStmt ins;
      ins.table = d.table;
      ins.rows.reserve(vis.size());
      for (auto& [k, row] : vis) ins.rows.push_back(row);
      tmp.execInsert(ins);
    }
    for (auto& [tn, schp] : extraSch) {
      tmp.execCreate(CreateTableStmt{tn, schp->columns, false});
      auto& rows = extraRows[tn];
      if (!rows.empty()) {
        InsertStmt ins;
        ins.table = tn;
        ins.rows = rows;
        tmp.execInsert(ins);
      }
    }
    SelectStmt sel;
    sel.table = d.table;
    sel.select_all = true;
    sel.where = d.where;
    sel.where_groups = d.where_groups;
    Result matched = tmp.execSelect(sel);
    std::vector<std::string> keys;
    for (const auto& [k, row] : vis) {
      bool hit = false;
      for (auto& m : matched.rows) {
        if (m.size() != row.size()) continue;
        bool eq = true;
        for (std::size_t i = 0; i < row.size(); ++i) {
          if (!valueEquals(m[i], row[i])) {
            eq = false;
            break;
          }
        }
        if (eq) {
          hit = true;
          break;
        }
      }
      if (!hit) continue;
      keys.push_back(k);
    }
    if (keys.empty()) return {{}, {}, "DELETE 0", 0};
    if (wal_ != nullptr) {
      std::vector<std::string> mirror_batch;
      mirror_batch.reserve(keys.size());
      for (const auto& k : keys) {
        std::string payload;
        payload += 'D';
        payload += kWalSep;
        payload += norm;
        payload += kWalSep;
        payload += walEscape(k);
        wal_->append(payload);
        mirror_batch.push_back(std::move(payload));
      }
      wal_->flush();
      syncMirrorBatch(&mirror_batch);
    }
    {
      kv::WriteBatch batch;
      for (const auto& k : keys) batch.Delete(k);
      if (!kv_.Write(batch)) throw SqlError("KV-Write fehlgeschlagen");
    }
    {
      txn::Transaction w = mvcc_.BeginWriteBlocking();
      for (const auto& k : keys) {
        if (!mvcc_.Erase(w, k)) {
          mvcc_.Abort(w);
          throw SqlError("MVCC-Write fehlgeschlagen");
        }
      }
      if (!mvcc_.Commit(w)) throw SqlError("MVCC-Commit fehlgeschlagen");
    }
    // HTAP: Tombstones in der Replika spiegeln (enc bleibt fuer alte Snapshots).
    if (!keys.empty()) {
      const std::uint64_t cts = commitTsOf(keys[0]);
      if (cts == 0) {
        rebuildReplicaForTable(norm);
      } else {
        for (const auto& k : keys) mirrorEraseOne(norm, k, cts);
      }
    }
    return {{},
            {},
            "DELETE " + std::to_string(keys.size()),
            keys.size()};
  }
  if (std::holds_alternative<DropTableStmt>(st)) {
    // DROP TABLE: Schema-Key + alle Row-Keys aus KV entfernen, MVCC-Tombstones
    // je Row-Key, Registry-Eintrag loeschen. Danach ist die Tabelle fuer
    // SELECT/INSERT/UPDATE/DELETE unbekannt (SqlError). WAL zuerst
    // (Opcode 'T'), dann KV, dann MVCC, dann Registry.
    const DropTableStmt& d = std::get<DropTableStmt>(st);
    const std::string norm = normalizeTable(d.table);
    auto it = tables_.find(norm);
    if (it == tables_.end()) {
      if (d.if_exists) return {{}, {}, "DROP TABLE", 0};
      throw SqlError("Tabelle unbekannt: " + d.table);
    }
    auto snap = kv_.GetSnapshot();
    std::vector<std::string> keys;
    for (const auto& [k, v] : snap->Scan(tablePrefix(norm))) {
      (void)v;
      keys.push_back(k);
    }
    if (wal_ != nullptr) {
      std::string payload;
      payload += 'T';
      payload += kWalSep;
      payload += norm;
      wal_->append(payload);
      wal_->flush();
      const std::vector<std::string> mirror_batch{payload};
      syncMirrorBatch(&mirror_batch);
    }
    {
      kv::WriteBatch batch;
      for (const auto& k : keys) batch.Delete(k);
      batch.Delete("sql/__schema/" + norm);
      if (!kv_.Write(batch)) throw SqlError("KV-Write fehlgeschlagen");
    }
    if (!keys.empty()) {
      txn::Transaction w = mvcc_.BeginWriteBlocking();
      for (const auto& k : keys) {
        if (!mvcc_.Erase(w, k)) {
          mvcc_.Abort(w);
          throw SqlError("MVCC-Write fehlgeschlagen");
        }
      }
      if (!mvcc_.Commit(w)) throw SqlError("MVCC-Commit fehlgeschlagen");
    }
    tables_.erase(it);
    replica_.erase(norm);  // HTAP: Scan-Replika konsistent verwerfen
    rbacFor(this).grants.erase(norm);  // Rechte fallen mit der Tabelle (PG)
    return {{}, {}, "DROP TABLE", 0};
  }
  return execSelect(std::get<SelectStmt>(st));
}

bool Executor::hasTable(const std::string& name) const {
  return tables_.count(normalizeTable(name)) > 0;
}

const TableSchema* Executor::schemaOf(const std::string& name) const {
  auto it = tables_.find(normalizeTable(name));
  if (it == tables_.end()) return nullptr;
  return &it->second;
}

Result Executor::execCreate(const CreateTableStmt& s) {
  const std::string norm = normalizeTable(s.table);
  auto it = tables_.find(norm);
  if (it != tables_.end()) {
    if (s.if_not_exists) return {{}, {}, "TABLE EXISTS " + s.table, 0};
    throw SqlError("Tabelle existiert bereits: " + s.table);
  }
  TableSchema sch;
  sch.columns = s.columns;
  sch.next_rowid = 0;
  tables_[norm] = sch;
  replica_.try_emplace(norm);  // HTAP: leere Scan-Replika (immer-sichtbar)

  const std::string enc = schemaEncode(s.columns);
  kv_.Put("sql/__schema/" + norm, enc);

  if (wal_ != nullptr) {
    std::string payload;
    payload += 'C';
    payload += kWalSep;
    payload += norm;
    payload += kWalSep;
    payload += enc;
    wal_->append(payload);
    wal_->flush();
    const std::vector<std::string> mirror_batch{payload};
    syncMirrorBatch(&mirror_batch);
  }
  return {{}, {}, "CREATE TABLE", 0};
}

Result Executor::execInsert(const InsertStmt& s) {
  const std::string norm = normalizeTable(s.table);
  auto it = tables_.find(norm);
  if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + s.table);
  TableSchema& sch = it->second;

  // Zielspalten aufloesen (wie Database::execInsert).
  std::vector<int> colMap;
  if (s.columns.empty()) {
    for (std::size_t i = 0; i < sch.columns.size(); ++i)
      colMap.push_back(static_cast<int>(i));
  } else {
    for (auto& c : s.columns) {
      int idx = schemaColIndex(sch.columns, c);
      if (idx < 0) throw SqlError("Unbekannte Spalte: " + c);
      colMap.push_back(idx);
    }
  }

  // Zeilen validieren + koerzieren.
  std::vector<std::vector<Value>> fullRows;
  fullRows.reserve(s.rows.size());
  for (auto& r : s.rows) {
    if (r.size() != colMap.size())
      throw SqlError("INSERT: Spaltenzahl passt nicht (" +
                     std::to_string(r.size()) + " vs " +
                     std::to_string(colMap.size()) + ")");
    std::vector<Value> full(sch.columns.size(), Value{std::monostate{}});
    for (std::size_t i = 0; i < r.size(); ++i) {
      const std::size_t ti = static_cast<std::size_t>(colMap[i]);
      full[ti] = coerceValue(r[i], sch.columns[ti].type, sch.columns[ti].name);
    }
    fullRows.push_back(std::move(full));
  }

  // Keys erzeugen (pk = erste Spalte; Kollision -> "#rowid"-Suffix).
  const std::string prefix = tablePrefix(norm);
  std::vector<std::string> keys;
  keys.reserve(fullRows.size());
  for (auto& full : fullRows) {
    std::string pkStr =
        full.empty() ? std::to_string(sch.next_rowid) : valueToString(full[0]);
    if (pkStr.empty()) pkStr = std::to_string(sch.next_rowid);
    std::string key = prefix + pkStr;
    if (kv_.Get(key).has_value()) {
      key += "#" + std::to_string(sch.next_rowid);
    }
    // Kollision innerhalb desselben Statements abfangen.
    for (auto& k : keys) {
      if (k == key) {
        key += "#" + std::to_string(sch.next_rowid);
        break;
      }
    }
    keys.push_back(key);
    sch.next_rowid++;
  }

  std::vector<std::string> encs;
  encs.reserve(fullRows.size());
  for (auto& full : fullRows) encs.push_back(encodeRow(full));

  // 1) WAL-append (1 Record/Zeile), dann Group-Commit via flush.
  if (wal_ != nullptr) {
    std::vector<std::string> mirror_batch;
    mirror_batch.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
      std::string payload;
      payload += 'I';
      payload += kWalSep;
      payload += norm;
      payload += kWalSep;
      payload += walEscape(keys[i]);
      payload += kWalSep;
      payload += encs[i];
      wal_->append(payload);
      mirror_batch.push_back(std::move(payload));
    }
    wal_->flush();
    syncMirrorBatch(&mirror_batch);
  }

  // 2) KV-WriteBatch atomar.
  {
    kv::WriteBatch batch;
    for (std::size_t i = 0; i < keys.size(); ++i)
      batch.Put(keys[i], encs[i]);
    if (!kv_.Write(batch)) throw SqlError("KV-Write fehlgeschlagen");
  }

  // 3) MVCC-Commit unter einer Writer-Txn (Single-Writer, blocking).
  {
    txn::Transaction w = mvcc_.BeginWriteBlocking();
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (!mvcc_.Write(w, keys[i], encs[i])) {
        mvcc_.Abort(w);
        throw SqlError("MVCC-Write fehlgeschlagen");
      }
    }
    if (!mvcc_.Commit(w)) throw SqlError("MVCC-Commit fehlgeschlagen");
  }

  // 4) HTAP: committed Rows in die Scan-Replika spiegeln (ein Commit-TS pro
  // Writer-Commit -> Statement atomar sichtbar; KV/MVCC bleibt Write-Truth).
  if (!keys.empty()) {
    const std::uint64_t cts = commitTsOf(keys[0]);
    if (cts == 0) {
      rebuildReplicaForTable(norm);  // unerreichbar nach erfolgreichem Commit
    } else {
      for (std::size_t i = 0; i < keys.size(); ++i)
        mirrorUpsertOne(norm, keys[i], encs[i], cts);
    }
  }

  return {{}, {}, "INSERT 0 " + std::to_string(fullRows.size()),
          fullRows.size()};
}

Result Executor::execSelect(const SelectStmt& s) {
  const std::string norm = normalizeTable(s.table);
  auto it = tables_.find(norm);
  if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + s.table);
  const TableSchema& sch = it->second;
  // Rechte Join-Seite ggf. vorab aufloesen (Fehler vor dem Snapshot-Read).
  const TableSchema* rsch = nullptr;
  std::string rnorm;
  const bool needRight =
      s.has_join && normalizeTable(s.join_table) != norm;
  if (s.has_join && !needRight) {
    // Self-Join (gleiche Tabelle): rechte Seite = linke (ein Scan genuegt).
    rsch = &sch;
    rnorm = norm;
  } else if (s.has_join) {
    rnorm = normalizeTable(s.join_table);
    auto jt = tables_.find(rnorm);
    if (jt == tables_.end())
      throw SqlError("Tabelle unbekannt: " + s.join_table);
    rsch = &jt->second;
  }

  // Snapshot-Read aus der HTAP Scan-Replika (statt KV-Prefix-Scan + MVCC-Read
  // je Zeile): genau EIN MVCC-Snapshot (rtxn) liefert die Sichtbarkeits-TS,
  // sichtbare Rows kommen aus der Replika (begin_ts <= snapshot < end_ts).
  // KV/MVCC bleibt Write-Truth (INSERT/UPDATE/DELETE/Matching unveraendert).
  //
  // Streaming-Ausfuehrung ohne Temp-Database: jede Tabelle wird genau einmal
  // dekodiert (nur benoetigte Spalten, s. computeNeeded/decodeRowSelected)
  // und per Move in Snapshot-Tabellen gestellt; Filter, Projektion, JOIN,
  // GROUP BY, ORDER BY, Aggregation und Subqueries wertet
  // Database::execSelectSnapshot danach in genau einem Durchlauf direkt auf
  // diesen Rows aus (kein Zweit-Insert, kein Coerce-Loop, keine
  // Doppel-Auswertung, keine Row-Duplikate).
  txn::Transaction rtxn = mvcc_.BeginRead();
  const std::uint64_t snapTs = mvcc_.SnapshotOf(rtxn);
  std::map<std::string, const TableSchema*> schemas;
  schemas[norm] = &sch;
  if (s.has_join) schemas[rnorm] = rsch;
  std::set<std::string> needed;
  collectSelectTables(s, needed);
  for (const auto& tn : needed) {
    if (schemas.count(tn) != 0) continue;
    auto jt = tables_.find(tn);
    if (jt == tables_.end()) continue;  // execSelectSnapshot wirft "unbekannt"
    schemas[tn] = &jt->second;
  }
  NeededMap need;
  try {
    need = computeNeeded(s, schemas);
  } catch (...) {
    need.clear();  // Fallback: alles voll dekodieren (s. decodeRowSelected)
  }
  auto loadRows = [&](const std::string& tnorm, const TableSchema& sc) {
    std::vector<std::vector<Value>> rows;
    auto nit = need.find(tnorm);
    const std::vector<char>* mask =
        (nit != need.end()) ? &nit->second : nullptr;
    auto rit = replica_.find(tnorm);
    if (rit == replica_.end()) {
      // Invarianten-Bruch (sollte nie passieren: CREATE legt die Replika an):
      // aus KV/MVCC-Truth heilen, dann lesen (einmaliger Legacy-Scan statt
      // falschem Leer-Ergebnis).
      rebuildReplicaForTable(tnorm);
      rit = replica_.find(tnorm);
      if (rit == replica_.end()) return rows;
    }
    const columnar::ColumnarStore& rep = rit->second;
    const std::vector<std::size_t> vis = rep.ReplicaVisible(snapTs);
    const auto& rrows = rep.ReplicaRows();
    rows.reserve(vis.size());
    for (std::size_t idx : vis) {
      if (idx >= rrows.size()) continue;  // defensive (unreachable)
      try {
        rows.push_back(
            decodeRowSelected(rrows[idx].enc, sc.columns.size(), mask));
      } catch (...) {
        continue;  // korrupte Zeile ueberspringen (sollte nicht passieren)
      }
    }
    return rows;
  };
  std::map<std::string, Table> snapTables;
  {
    Table t;
    t.columns = sch.columns;
    t.rows = loadRows(norm, sch);
    snapTables.emplace(norm, std::move(t));
  }
  if (needRight) {
    Table t;
    t.columns = rsch->columns;
    t.rows = loadRows(rnorm, *rsch);
    snapTables.emplace(rnorm, std::move(t));
  }
  // Subquery-Tabellen im selben Snapshot mitladen (unkorreliert -> konsistent).
  needed.erase(norm);
  if (s.has_join) needed.erase(rnorm);
  for (const auto& tn : needed) {
    auto jt = schemas.find(tn);
    if (jt == schemas.end()) continue;  // execSelectSnapshot wirft "unbekannt"
    Table t;
    t.columns = jt->second->columns;
    t.rows = loadRows(tn, *jt->second);
    snapTables.emplace(tn, std::move(t));
  }
  mvcc_.Commit(rtxn);

  Database db;
  return db.execSelectSnapshot(s, std::move(snapTables));
}

void Executor::applyCreateRecord(const std::string& table,
                                 const std::string& schemaEnc) {
  const std::string norm = normalizeTable(table);
  auto cols = schemaDecode(schemaEnc);
  auto it = tables_.find(norm);
  if (it == tables_.end()) {
    TableSchema sch;
    sch.columns = std::move(cols);
    sch.next_rowid = 0;
    tables_[norm] = std::move(sch);
  } else {
    it->second.columns = std::move(cols);
  }
  kv_.Put("sql/__schema/" + norm, schemaEnc);
  replica_.try_emplace(norm);  // HTAP: Replika-Eintrag (kein Clear bei Replay)
}

bool Executor::applyInsertRecord(const std::string& table, const std::string& key,
                                 const std::string& rowEnc) {
  const std::string norm = normalizeTable(table);
  if (tables_.find(norm) == tables_.end()) return false;  // ohne Schema nicht replaybar
  kv_.Put(key, rowEnc);
  txn::Transaction w = mvcc_.BeginWriteBlocking();
  if (mvcc_.Write(w, key, rowEnc)) {
    if (mvcc_.Commit(w)) {
      // HTAP: replayte Version mit ihrem eigenen Commit-TS spiegeln (WAL-
      // Reihenfolge = Versionsreihenfolge -> gleiche Sichtbarkeit wie live).
      mirrorUpsertOne(norm, key, rowEnc, commitTsOf(key));
      return true;
    }
    mvcc_.Abort(w);
    return false;
  }
  mvcc_.Abort(w);
  return false;
}

std::uint64_t Executor::commitTsOf(const std::string& key) {
  // Exakte Commit-TS aus der MVCC-Kette (keine NextTimestamp-Heuristik):
  // Commit installiert jede Version mit genau einer commit_ts (s. mvcc.cpp),
  // die neueste Kette traegt sie in trx_begin.
  const auto chain = mvcc_.GetChain(key);
  if (chain.empty()) return 0;
  return chain.back().trx_begin;
}

void Executor::mirrorUpsertOne(const std::string& norm, const std::string& key,
                               const std::string& enc,
                               std::uint64_t commit_ts) {
  auto it = replica_.find(norm);
  if (it == replica_.end() || commit_ts == 0) {
    rebuildReplicaForTable(norm);  // Selbstheilung (unreachable im Normalfall)
    return;
  }
  it->second.ReplicaAppend(key, enc, commit_ts);
}

void Executor::mirrorEraseOne(const std::string& norm, const std::string& key,
                              std::uint64_t commit_ts) {
  auto it = replica_.find(norm);
  if (it == replica_.end() || commit_ts == 0) {
    rebuildReplicaForTable(norm);  // Selbstheilung (unreachable im Normalfall)
    return;
  }
  it->second.ReplicaErase(key, commit_ts);
}

void Executor::rebuildReplicaForTable(const std::string& norm) {
  // Latest-State aus KV-Keymenge + jeweils neuester MVCC-Version (Tombstones
  // entfallen; begin_ts aus der Kette). Nur Fallback/Selbstheilung: im
  // Normalfall spiegeln Writes inkrementell (Versionen exakt, kein Re-Scan).
  auto it = tables_.find(norm);
  if (it == tables_.end()) {
    replica_.erase(norm);
    return;
  }
  columnar::ColumnarStore fresh;
  for (const auto& [k, v] : kv_.Scan(tablePrefix(norm))) {
    (void)v;  // Wert aus der MVCC-Kette (kein Dirty-Read)
    const auto chain = mvcc_.GetChain(k);
    if (chain.empty()) continue;
    const auto& newest = chain.back();
    if (newest.deleted) continue;
    fresh.ReplicaAppend(k, newest.value, newest.trx_begin);
  }
  replica_[norm] = std::move(fresh);
}

std::size_t Executor::recover() {
  recover_skipped_ = 0;
  recover_applied_ = 0;
  if (wal_ == nullptr) return 0;
  // Mit aktivem Spiegel nur den Tail seit mirror_lsn replayen (der Rest
  // steckt als Latest-State im Spiegel, s. enableMirror). Ohne Spiegel oder
  // bei degradiertem Spiegel (lsn 0) exakt Altverhalten: Voll-Replay.
  std::vector<storage::WalRecord> recs;
  const bool use_tail = mirror_on_ && mirror_ != nullptr &&
                        mirror_->IsOpen() && mirror_lsn_ > 0;
  if (use_tail) {
    try {
      recs = wal_->read_from(mirror_lsn_ + 1);
    } catch (...) {
      recs = wal_->replay();  // Fallback: WAL-Truth gewinnt immer
    }
  } else {
    recs = wal_->replay();
  }
  for (auto& r : recs) {
    auto parts = splitWal(r.data);
    if (parts.empty()) {
      ++recover_skipped_;
      continue;
    }
    if (parts[0] == "C" && parts.size() == 3) {
      try {
        applyCreateRecord(parts[1], parts[2]);
        ++recover_applied_;
      } catch (...) {
        ++recover_skipped_;
      }
    } else if (parts[0] == "I" && parts.size() == 4) {
      try {
        if (applyInsertRecord(parts[1], walUnescape(parts[2]), parts[3])) {
          ++recover_applied_;
        } else {
          ++recover_skipped_;
        }
      } catch (...) {
        ++recover_skipped_;
      }
    } else if (parts[0] == "U" && parts.size() == 4) {
      // UPDATE-Replay: mechanisch wie INSERT (last-wins KV-Put + neue
      // MVCC-Version). Ohne Schema nicht replaybar -> Skip.
      try {
        if (applyInsertRecord(parts[1], walUnescape(parts[2]), parts[3])) {
          ++recover_applied_;
        } else {
          ++recover_skipped_;
        }
      } catch (...) {
        ++recover_skipped_;
      }
    } else if (parts[0] == "D" && parts.size() == 3) {
      // DELETE-Replay: KV-Key entfernen + MVCC-Tombstone. Ohne Schema -> Skip.
      try {
        const std::string norm = normalizeTable(parts[1]);
        if (tables_.find(norm) == tables_.end()) {
          ++recover_skipped_;
        } else {
          const std::string key = walUnescape(parts[2]);
          kv_.Delete(key);
          txn::Transaction w = mvcc_.BeginWriteBlocking();
          if (mvcc_.Erase(w, key) && mvcc_.Commit(w)) {
            mirrorEraseOne(norm, key, commitTsOf(key));  // HTAP: Tombstone spiegeln
            ++recover_applied_;
          } else {
            mvcc_.Abort(w);
            ++recover_skipped_;
          }
        }
      } catch (...) {
        ++recover_skipped_;
      }
    } else if (parts[0] == "T" && parts.size() == 2) {
      // DROP-Replay (idempotent): alle Row-Keys + Schema-Key aus KV
      // entfernen, MVCC-Tombstones je Row-Key, Registry-Eintrag loeschen.
      try {
        const std::string norm = normalizeTable(parts[1]);
        auto kvs = kv_.Scan(tablePrefix(norm));
        kv::WriteBatch batch;
        std::vector<std::string> keys;
        keys.reserve(kvs.size());
        for (auto& [k, v] : kvs) {
          (void)v;
          batch.Delete(k);
          keys.push_back(k);
        }
        batch.Delete("sql/__schema/" + norm);
        if (!kv_.Write(batch)) {
          ++recover_skipped_;
        } else {
          bool ok = true;
          if (!keys.empty()) {
            txn::Transaction w = mvcc_.BeginWriteBlocking();
            for (auto& k : keys) {
              if (!mvcc_.Erase(w, k)) {
                ok = false;
                break;
              }
            }
            if (ok) {
              ok = mvcc_.Commit(w);
            } else {
              mvcc_.Abort(w);
            }
          }
          tables_.erase(norm);
          replica_.erase(norm);  // HTAP: Scan-Replika konsistent verwerfen
          rbacFor(this).grants.erase(norm);  // Rechte fallen mit (wie live)
          if (ok) {
            ++recover_applied_;
          } else {
            ++recover_skipped_;
          }
        }
      } catch (...) {
        ++recover_skipped_;
      }
    } else if ((parts[0] == "G" || parts[0] == "R") && parts.size() == 4) {
      // RBAC-Replay: Rechte wiederherstellen (kein Schema noetig, kein
      // WAL-Append, kein Mirror-State). Ungueltig -> Skip.
      try {
        const std::string norm = normalizeTable(parts[1]);
        const std::string role = walUnescape(parts[2]);
        const std::vector<std::string> privs = rbacSplitPrivs(parts[3]);
        RbacState& rs = rbacFor(this);
        if (parts[0] == "G") {
          for (const auto& p : privs) rs.grants[norm][role].insert(p);
        } else {
          auto tit = rs.grants.find(norm);
          if (tit != rs.grants.end()) {
            auto rit = tit->second.find(role);
            if (rit != tit->second.end()) {
              for (const auto& p : privs) rit->second.erase(p);
              if (rit->second.empty()) tit->second.erase(rit);
            }
            if (tit->second.empty()) rs.grants.erase(tit);
          }
        }
        ++recover_applied_;
      } catch (...) {
        ++recover_skipped_;
      }
    } else {
      ++recover_skipped_;  // unbekannter Opcode / falsche Arity
    }
  }
  // RowID-Counter aus MAX-Suffix aller "#<id>"-Keys heben (statt Key-Anzahl):
  // Suffixe wurden aus next_rowid generiert, daher ist max+1 der naechste
  // kollisionsfreie Suffix. Nur anheben, nie absenken.
  for (auto& [t, sch] : tables_) {
    const auto kvs = kv_.Scan(tablePrefix(t));
    bool found = false;
    std::uint64_t maxId = 0;
    for (auto& [key, val] : kvs) {
      (void)val;
      const std::size_t pos = key.rfind('#');
      if (pos == std::string::npos || pos + 1 >= key.size()) continue;
      bool allDigits = true;
      for (std::size_t i = pos + 1; i < key.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(key[i]))) {
          allDigits = false;
          break;
        }
      }
      if (!allDigits) continue;
      try {
        const auto id =
            static_cast<std::uint64_t>(std::stoull(key.substr(pos + 1)));
        if (!found || id > maxId) maxId = id;
        found = true;
      } catch (...) {
        continue;
      }
    }
    if (found) {
      if (maxId == std::numeric_limits<std::uint64_t>::max()) {
        sch.next_rowid = maxId;  // Overflow-Schutz (praktisch unerreichbar)
      } else if (maxId + 1 > sch.next_rowid) {
        sch.next_rowid = maxId + 1;
      }
    }
  }
  // Checkpoint-Catch-up: gerade replayten Tail (oder leeren Tail) in den
  // Spiegel spiegeln, damit der naechste Restart den Tail nicht erneut zahlt.
  if (mirror_on_) syncMirrorFromWal();
  return recover_skipped_;
}

}  // namespace dbengine::sql
