// Mini-SQL V1: Tokenizer + Recursive-Descent-Parser + In-Memory-Executor.
// PG-kompatibler Subset (siehe parser.h). Kein Flex/Bison.

#include "dbengine/sql/parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace dbengine::sql {
namespace {

// ---------- Helpers ----------

std::string toUpper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper((unsigned char)c));
  return s;
}
std::string toLower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
  return s;
}
// PG: ungefaltete Identifier -> lowercase
std::string foldIdent(const std::string& s) { return toLower(s); }

// ---------- Tokenizer ----------

enum class TokKind {
  Eof,
  Ident,    // inkl. Keywords als Ident mit upper-Cache
  Integer,
  Float,
  String,   // Single-quote, bereits ent-escaped
  QuotedIdent,  // "..." (case-sensitiv, kein Folding)
  Symbol,   // Einzelnes Satzzeichen bzw. 2-Zeichen-Op (<>, !=, <=, >=)
  Star,     // '*' (fuer SELECT * / COUNT(*))
  Param,    // $1 Platzhalter -> V1: Fehler (fuer saubere Meldung)
};

struct Token {
  TokKind kind = TokKind::Eof;
  std::string text;  // Ident original, String-Inhalt, Symbol-Text
};

class Tokenizer {
 public:
  explicit Tokenizer(const std::string& in) : s_(in) {}

  std::vector<Token> run() {
    std::vector<Token> out;
    while (true) {
      skipWsAndComments();
      if (pos_ >= s_.size()) {
        out.push_back({TokKind::Eof, ""});
        return out;
      }
      char c = s_[pos_];
      if (c == '\'') {
        out.push_back({TokKind::String, readString()});
      } else if (c == '"') {
        out.push_back({TokKind::QuotedIdent, readQuoted()});
      } else if (std::isalpha((unsigned char)c) || c == '_') {
        out.push_back({TokKind::Ident, readIdent()});
      } else if (std::isdigit((unsigned char)c) ||
                 (c == '.' && pos_ + 1 < s_.size() &&
                  std::isdigit((unsigned char)s_[pos_ + 1]))) {
        out.push_back(readNumber());
      } else if (c == '$' && pos_ + 1 < s_.size() &&
                 std::isdigit((unsigned char)s_[pos_ + 1])) {
        std::string t;
        t += s_[pos_++];
        while (pos_ < s_.size() && std::isdigit((unsigned char)s_[pos_]))
          t += s_[pos_++];
        out.push_back({TokKind::Param, t});
      } else if (c == '*' || c == ',' || c == ';' || c == '(' || c == ')' ||
                 c == '.' || c == '=') {
        std::string t(1, c);
        ++pos_;
        out.push_back(
            {c == '*' ? TokKind::Star : TokKind::Symbol, std::move(t)});
      } else if (c == '+' || c == '-' || c == '/') {
        // Arithmetik-Ops (Q6). Kommentare ("--"/"/*") wurden oben in
        // skipWsAndComments bereits konsumiert, daher ist ein einzelnes
        // '-'/'/' hier sicher ein Operator. Negative Literale werden in
        // parseLiteral() via fuehrendem Vorzeichen-Symbol erkannt.
        std::string t(1, c);
        ++pos_;
        out.push_back({TokKind::Symbol, std::move(t)});
      } else if (c == '<' || c == '>' || c == '!') {
        std::string t(1, c);
        ++pos_;
        if (pos_ < s_.size() && s_[pos_] == '=') t += s_[pos_++];
        // "<>" wird als "<" + ">" gelesen -> hier zusammenfassen
        if (t == "<" && pos_ < s_.size() && s_[pos_] == '>') t += s_[pos_++];
        out.push_back({TokKind::Symbol, std::move(t)});
      } else {
        throw SqlError(std::string("Unerwartetes Zeichen: '") + c + "'");
      }
    }
  }

 private:
  void skipWsAndComments() {
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (std::isspace((unsigned char)c)) {
        ++pos_;
        continue;
      }
      if (c == '-' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '-') {
        pos_ += 2;
        while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
        continue;
      }
      if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < s_.size() &&
               !(s_[pos_] == '*' && s_[pos_ + 1] == '/'))
          ++pos_;
        if (pos_ + 1 >= s_.size()) throw SqlError("Unterminierter Kommentar");
        pos_ += 2;
        continue;
      }
      break;
    }
  }

  std::string readString() {
    ++pos_;  // '
    std::string out;
    while (pos_ < s_.size()) {
      char c = s_[pos_++];
      if (c == '\'') {
        if (pos_ < s_.size() && s_[pos_] == '\'') {
          out += '\'';
          ++pos_;
        } else {
          return out;
        }
      } else {
        out += c;
      }
    }
    throw SqlError("Unterminiertes String-Literal");
  }

  std::string readQuoted() {
    ++pos_;  // "
    std::string out;
    while (pos_ < s_.size()) {
      char c = s_[pos_++];
      if (c == '"') {
        if (pos_ < s_.size() && s_[pos_] == '"') {
          out += '"';
          ++pos_;
        } else {
          return out;
        }
      } else {
        out += c;
      }
    }
    throw SqlError("Unterminierter quoted Identifier");
  }

  std::string readIdent() {
    std::size_t start = pos_;
    while (pos_ < s_.size() &&
           (std::isalnum((unsigned char)s_[pos_]) || s_[pos_] == '_' ||
            s_[pos_] == '$'))
      ++pos_;
    return s_.substr(start, pos_ - start);
  }

  Token readNumber() {
    std::size_t start = pos_;
    bool isFloat = false;
    while (pos_ < s_.size() && std::isdigit((unsigned char)s_[pos_])) ++pos_;
    if (pos_ < s_.size() && s_[pos_] == '.') {
      isFloat = true;
      ++pos_;
      while (pos_ < s_.size() && std::isdigit((unsigned char)s_[pos_])) ++pos_;
    }
    if (pos_ < s_.size() &&
        (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      isFloat = true;
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      if (pos_ >= s_.size() || !std::isdigit((unsigned char)s_[pos_]))
        throw SqlError("Ungueltige numerische Konstante");
      while (pos_ < s_.size() && std::isdigit((unsigned char)s_[pos_])) ++pos_;
    }
    std::string t = s_.substr(start, pos_ - start);
    return {isFloat ? TokKind::Float : TokKind::Integer, t};
  }

  const std::string& s_;
  std::size_t pos_ = 0;
};

// ---------- Parser ----------

class Parser {
 public:
  explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

  Statement run() {
    if (matchKeyword("CREATE")) {
      if (peekKeyword("POLICY")) return parseCreatePolicy();
      return parseCreate();
    }
    if (matchKeyword("ALTER")) return parseAlterRls();
    if (matchKeyword("INSERT")) return parseInsert();
    if (matchKeyword("SELECT")) return parseSelect();
    if (matchKeyword("UPDATE")) return parseUpdate();
    if (matchKeyword("DELETE")) return parseDelete();
    if (matchKeyword("DROP")) return parseDropTable();
    if (matchKeyword("GRANT")) return parseGrant();
    if (matchKeyword("REVOKE")) return parseRevoke();
    if (matchKeyword("SET")) return parseSetRole();
    if (matchKeyword("RESET")) return parseResetRole();
    if (peekKeyword("TRUNCATE"))
      throw SqlError(
          "TRUNCATE wird nicht unterstuetzt (DELETE FROM ... verwenden)");
    throw SqlError(
        "Nur CREATE TABLE / CREATE POLICY / ALTER TABLE ... ROW LEVEL "
        "SECURITY / INSERT / SELECT / UPDATE / DELETE / DROP TABLE / "
        "GRANT / REVOKE / SET ROLE / RESET ROLE werden unterstuetzt");
  }

 private:
  const Token& peek(std::size_t off = 0) const {
    std::size_t i = pos_ + off;
    if (i >= toks_.size()) return toks_.back();
    return toks_[i];
  }
  const Token& next() { return toks_[pos_++]; }

  bool peekKeyword(const std::string& kw, std::size_t off = 0) const {
    const Token& t = peek(off);
    return t.kind == TokKind::Ident && toUpper(t.text) == kw;
  }
  bool matchKeyword(const std::string& kw) {
    if (peekKeyword(kw)) {
      ++pos_;
      return true;
    }
    return false;
  }
  void expectKeyword(const std::string& kw) {
    if (!matchKeyword(kw)) throw SqlError("Erwartet Keyword " + kw);
  }
  bool matchSymbol(const std::string& sym) {
    const Token& t = peek();
    if ((t.kind == TokKind::Symbol && t.text == sym) ||
        (sym == "*" && t.kind == TokKind::Star)) {
      ++pos_;
      return true;
    }
    return false;
  }
  void expectSymbol(const std::string& sym) {
    if (!matchSymbol(sym)) throw SqlError("Erwartet '" + sym + "'");
  }
  // Identifier lesen: unquoted -> lowercase-Folding, "..." -> exakt
  std::string parseIdent() {
    const Token& t = peek();
    if (t.kind == TokKind::Ident) {
      ++pos_;
      return foldIdent(t.text);
    }
    if (t.kind == TokKind::QuotedIdent) {
      ++pos_;
      return t.text;
    }
    throw SqlError("Erwartet Identifier");
  }

  // Spaltenreferenz mit optionalem Tabellen-Prefix: col | t.col (JOIN).
  // Beide Teile via parseIdent (unquoted -> lowercase-Folding).
  std::string parseColRef() {
    std::string first = parseIdent();
    if (matchSymbol(".")) {
      std::string second = parseIdent();
      if (peek().kind == TokKind::Symbol && peek().text == ".")
        throw SqlError("Nur ein Tabellen-Prefix (t.c) wird unterstuetzt");
      return first + "." + second;
    }
    return first;
  }

  CreateTableStmt parseCreate() {
    expectKeyword("TABLE");
    CreateTableStmt s;
    if (peekKeyword("IF")) {
      ++pos_;
      expectKeyword("NOT");
      expectKeyword("EXISTS");
      s.if_not_exists = true;
    }
    s.table = parseIdent();
    expectSymbol("(");
    bool first = true;
    while (true) {
      if (!first) expectSymbol(",");
      first = false;
      // allow trailing? nein
      ColumnDef c;
      c.name = parseIdent();
      const Token& tt = next();
      if (tt.kind != TokKind::Ident)
        throw SqlError("Erwartet Spaltentyp fuer '" + c.name + "'");
      c.type = colTypeFromString(tt.text);
      // Laengenangaben (VARCHAR(32)) + Constraints ueberlesen/tolerieren:
      // "(n)" skippen, PRIMARY KEY / NOT NULL / DEFAULT ... bis ',' oder ')'
      if (peek().kind == TokKind::Symbol && peek().text == "(") {
        ++pos_;
        int depth = 1;
        while (depth > 0) {
          const Token& x = next();
          if (x.kind == TokKind::Eof) throw SqlError("Unterminierte Typ-Liste");
          if (x.kind == TokKind::Symbol && x.text == "(") ++depth;
          if (x.kind == TokKind::Symbol && x.text == ")") --depth;
        }
      }
      // Constraints bis zum naechsten ',' oder ')' auf Top-Level ueberspringen
      while (!(peek().kind == TokKind::Symbol &&
               (peek().text == "," || peek().text == ")")) &&
             peek().kind != TokKind::Eof) {
        // DEFAULT <literal> konsumieren ohne Validierung
        ++pos_;
      }
      s.columns.push_back(std::move(c));
      const Token& n = peek();
      if (n.kind == TokKind::Symbol && n.text == ",") continue;
      if (n.kind == TokKind::Symbol && n.text == ")") {
        ++pos_;
        break;
      }
      throw SqlError("Erwartet ',' oder ')' in CREATE TABLE");
    }
    if (s.columns.empty()) throw SqlError("CREATE TABLE ohne Spalten");
    return s;
  }

  InsertStmt parseInsert() {
    expectKeyword("INTO");
    InsertStmt s;
    s.table = parseIdent();
    if (matchSymbol("(")) {
      // Spaltenliste
      if (!(peek().kind == TokKind::Symbol && peek().text == ")")) {
        while (true) {
          s.columns.push_back(parseIdent());
          if (matchSymbol(",")) continue;
          break;
        }
      }
      expectSymbol(")");
    }
    expectKeyword("VALUES");
    while (true) {
      expectSymbol("(");
      std::vector<Value> row;
      if (!(peek().kind == TokKind::Symbol && peek().text == ")")) {
        while (true) {
          row.push_back(parseLiteral());
          if (matchSymbol(",")) continue;
          break;
        }
      }
      expectSymbol(")");
      s.rows.push_back(std::move(row));
      if (matchSymbol(",")) continue;  // weitere Tupel
      break;
    }
    if (s.rows.empty()) throw SqlError("INSERT ohne VALUES-Zeilen");
    return s;
  }

  UpdateStmt parseUpdate() {
    UpdateStmt s;
    s.table = parseIdent();
    expectKeyword("SET");
    while (true) {
      std::string col = parseIdent();
      expectSymbol("=");
      Value v = parseLiteral();
      s.sets.emplace_back(std::move(col), std::move(v));
      if (matchSymbol(",")) continue;
      break;
    }
    if (matchKeyword("WHERE")) parseWhereClause(s.where, s.where_groups);
    return s;
  }

  DeleteStmt parseDelete() {
    expectKeyword("FROM");
    DeleteStmt s;
    s.table = parseIdent();
    if (matchKeyword("WHERE")) parseWhereClause(s.where, s.where_groups);
    return s;
  }

  DropTableStmt parseDropTable() {
    expectKeyword("TABLE");
    DropTableStmt s;
    if (peekKeyword("IF")) {
      ++pos_;
      expectKeyword("EXISTS");
      s.if_exists = true;
    }
    s.table = parseIdent();
    if (peekKeyword("CASCADE"))
      throw SqlError(
          "CASCADE wird nicht unterstuetzt (nur DROP TABLE ohne CASCADE)");
    (void)matchKeyword("RESTRICT");  // Default-Semantik, toleriert
    const Token& t = peek();
    if (t.kind == TokKind::Symbol && t.text == ",")
      throw SqlError("Nur eine Tabelle pro DROP TABLE wird unterstuetzt");
    return s;
  }

  // RBAC: Privilegliste "SELECT [, INSERT ...]" / ALL (expandiert, deduped).
  std::vector<std::string> parsePrivList() {
    std::vector<std::string> out;
    auto add = [&](const std::string& p) {
      if (p == "ALL") {
        for (const char* q : {"SELECT", "INSERT", "UPDATE", "DELETE"}) {
          if (std::find(out.begin(), out.end(), q) == out.end())
            out.emplace_back(q);
        }
        return;
      }
      if (std::find(out.begin(), out.end(), p) == out.end())
        out.push_back(p);
    };
    while (true) {
      const Token& t = peek();
      if (t.kind != TokKind::Ident)
        throw SqlError(
            "Erwartet Privileg (SELECT/INSERT/UPDATE/DELETE/ALL)");
      std::string u = toUpper(t.text);
      if (u != "SELECT" && u != "INSERT" && u != "UPDATE" &&
          u != "DELETE" && u != "ALL")
        throw SqlError("Unbekanntes Privileg: " + t.text +
                       " (SELECT/INSERT/UPDATE/DELETE/ALL erwartet)");
      ++pos_;
      add(u);
      if (matchSymbol(",")) continue;
      break;
    }
    if (out.empty())
      throw SqlError("GRANT/REVOKE ohne Privilegien");
    return out;
  }

  // Rolle: unquoted Identifier -> lowercase-Folding (PG), "..."/'...' exakt.
  std::string parseRole() {
    const Token& t = peek();
    if (t.kind == TokKind::Ident) {
      ++pos_;
      return foldIdent(t.text);
    }
    if (t.kind == TokKind::QuotedIdent || t.kind == TokKind::String) {
      ++pos_;
      return t.text;
    }
    throw SqlError("Erwartet Rolle nach TO/FROM");
  }

  GrantStmt parseGrant() {
    GrantStmt s;
    s.privs = parsePrivList();
    expectKeyword("ON");
    s.table = parseIdent();
    expectKeyword("TO");
    s.role = parseRole();
    return s;
  }

  RevokeStmt parseRevoke() {
    RevokeStmt s;
    s.privs = parsePrivList();
    expectKeyword("ON");
    s.table = parseIdent();
    if (matchKeyword("FROM")) {
      // PG-Standard
    } else if (matchKeyword("TO")) {
      // toleriert (symmetrisch zu GRANT ... TO)
    } else {
      throw SqlError("Erwartet FROM nach Tabellennamen in REVOKE");
    }
    s.role = parseRole();
    return s;
  }

  SetRoleStmt parseSetRole() {
    expectKeyword("ROLE");
    SetRoleStmt s;
    const Token& t = peek();
    if (t.kind == TokKind::Ident && toUpper(t.text) == "NONE") {
      ++pos_;
      s.reset = true;
      return s;
    }
    s.role = parseRole();
    return s;
  }

  SetRoleStmt parseResetRole() {
    expectKeyword("ROLE");
    SetRoleStmt s;
    s.reset = true;
    return s;
  }

  // RLS: CREATE POLICY name ON t [FOR SELECT|INSERT|UPDATE|DELETE|ALL]
  // [TO r] USING (cond). FOR-Default ALL, TO-Default "*" (alle Rollen).
  CreatePolicyStmt parseCreatePolicy() {
    expectKeyword("POLICY");
    CreatePolicyStmt s;
    s.policy = parseIdent();
    expectKeyword("ON");
    s.table = parseIdent();
    if (peekKeyword("FOR")) {
      ++pos_;
      const Token& t = peek();
      if (t.kind != TokKind::Ident)
        throw SqlError("Erwartet FOR-Ziel (SELECT/INSERT/UPDATE/DELETE/ALL)");
      std::string u = toUpper(t.text);
      if (u != "SELECT" && u != "INSERT" && u != "UPDATE" &&
          u != "DELETE" && u != "ALL")
        throw SqlError("Unbekanntes POLICY-Ziel: " + t.text);
      ++pos_;
      s.command = u;
    }
    if (peekKeyword("TO")) {
      ++pos_;
      // PUBLIC (unquoted) = alle Rollen, wie PG.
      if (peekKeyword("PUBLIC")) {
        ++pos_;
        s.role = "*";
      } else {
        s.role = parseRole();
      }
    }
    expectKeyword("USING");
    expectSymbol("(");
    parsePolicyWhereClause(s.where, s.where_groups);
    expectSymbol(")");
    if (s.policy.empty()) throw SqlError("CREATE POLICY ohne Namen");
    return s;
  }

  AlterTableRlsStmt parseAlterRls() {
    expectKeyword("TABLE");
    AlterTableRlsStmt s;
    s.table = parseIdent();
    if (matchKeyword("ENABLE")) {
      s.enable = true;
    } else if (matchKeyword("DISABLE")) {
      s.enable = false;
    } else {
      throw SqlError("Erwartet ENABLE oder DISABLE in ALTER TABLE");
    }
    expectKeyword("ROW");
    expectKeyword("LEVEL");
    expectKeyword("SECURITY");
    return s;
  }

  // current_user/current_role/session_user (case-insensitiv, unquoted Ident)?
  bool peekCurrentUser() const {
    const Token& t = peek();
    if (t.kind != TokKind::Ident) return false;
    std::string u = toUpper(t.text);
    return u == "CURRENT_USER" || u == "CURRENT_ROLE" ||
           u == "SESSION_USER";
  }

  // Literal oder current_user als Vergleichswert (USING-RHS).
  // Rueckgabe: (Wert, ist_current_user). Subqueries in USING verboten.
  std::pair<Value, bool> parsePolicyValue() {
    if (peekCurrentUser()) {
      ++pos_;
      return {Value{std::monostate{}}, true};
    }
    // Skalare Subquery ablehnen (explizit statt kryptischem Literal-Fehler).
    if (peek().kind == TokKind::Symbol && peek().text == "(" &&
        peekKeyword("SELECT", 1))
      throw SqlError("Subquery in POLICY USING wird nicht unterstuetzt");
    return {parseLiteral(), false};
  }

  static std::string invertCmpOp(const std::string& op) {
    if (op == "<") return ">";
    if (op == "<=") return ">=";
    if (op == ">") return "<";
    if (op == ">=") return "<=";
    return op;  // "=","<>" symmetrisch
  }

  // USING-Condition: LHS Spalte oder current_user; RHS Literal oder
  // current_user (bzw. Spalte bei getauschtem current_user-LHS). Sonst exakt
  // die WHERE-Operatoren (LIKE/ILIKE/BETWEEN/IN/IS NULL inklusive).
  Condition parsePolicyCondition() {
    Condition c;
    bool lhsCur = peekCurrentUser();
    std::string lhsCol;
    if (lhsCur) {
      ++pos_;
    } else {
      lhsCol = parseColRef();
    }
    bool neg = false;
    if (peekKeyword("NOT")) {
      ++pos_;
      neg = true;
    }
    const Token& t = next();
    if (t.kind == TokKind::Ident) {
      std::string kw = toUpper(t.text);
      if (kw == "LIKE" || kw == "ILIKE") {
        if (lhsCur) {
          if (!lhsCol.empty()) throw SqlError("Unerwarteter Operator");
          // current_user LIKE 'pat': spaltenlose Konstante.
          c.lhs_is_current = true;
          c.op = neg ? ("NOT " + kw) : kw;
          auto [v, cur] = parsePolicyValue();
          if (cur) throw SqlError("current_user LIKE current_user sinnlos");
          c.value = v;
          return c;
        }
        c.column = lhsCol;
        c.op = neg ? ("NOT " + kw) : kw;
        auto [v, cur] = parsePolicyValue();
        c.value = v;
        c.value_is_current = cur;
        return c;
      }
      if (kw == "BETWEEN") {
        if (lhsCur) throw SqlError("BETWEEN mit current_user-LHS sinnlos");
        c.column = lhsCol;
        c.op = neg ? "NOT BETWEEN" : "BETWEEN";
        auto [lo, loCur] = parsePolicyValue();
        c.value = lo;
        c.value_is_current = loCur;
        expectKeyword("AND");
        auto [hi, hiCur] = parsePolicyValue();
        c.second = hi;
        c.second_is_current = hiCur;
        return c;
      }
      if (kw == "IN") {
        if (peekKeyword("SELECT"))
          throw SqlError("Subquery in POLICY USING wird nicht unterstuetzt");
        if (lhsCur) {
          // current_user IN (literals...): spaltenlose Konstante.
          c.lhs_is_current = true;
          c.op = neg ? "NOT IN" : "IN";
          expectSymbol("(");
          if (peekKeyword("SELECT"))
            throw SqlError("Subquery in POLICY USING wird nicht unterstuetzt");
          if (peek().kind == TokKind::Symbol && peek().text == ")")
            throw SqlError("IN-Liste darf nicht leer sein");
          while (true) {
            auto [v, cur] = parsePolicyValue();
            if (cur) throw SqlError("current_user IN (...) mit current_user");
            c.list.push_back(v);
            c.list_is_current.push_back(0);
            if (matchSymbol(",")) continue;
            break;
          }
          expectSymbol(")");
          return c;
        }
        c.column = lhsCol;
        c.op = neg ? "NOT IN" : "IN";
        expectSymbol("(");
        if (peekKeyword("SELECT"))
          throw SqlError("Subquery in POLICY USING wird nicht unterstuetzt");
        if (peek().kind == TokKind::Symbol && peek().text == ")")
          throw SqlError("IN-Liste darf nicht leer sein");
        while (true) {
          auto [v, cur] = parsePolicyValue();
          c.list.push_back(v);
          c.list_is_current.push_back(cur ? 1 : 0);
          if (matchSymbol(",")) continue;
          break;
        }
        expectSymbol(")");
        return c;
      }
      if (kw == "IS") {
        if (neg) throw SqlError("Unbekannter Operator: NOT IS");
        bool is_not = false;
        if (peekKeyword("NOT")) {
          ++pos_;
          is_not = true;
        }
        expectKeyword("NULL");
        if (lhsCur) {
          c.lhs_is_current = true;
          c.op = is_not ? "IS NOT NULL" : "IS NULL";
          c.value = Value{std::monostate{}};
          return c;
        }
        c.column = lhsCol;
        c.op = is_not ? "IS NOT NULL" : "IS NULL";
        c.value = Value{std::monostate{}};
        return c;
      }
      throw SqlError("Unbekannter Operator: " +
                     (neg ? ("NOT " + t.text) : t.text));
    }
    if (t.kind == TokKind::Symbol) {
      if (neg) throw SqlError("Unbekannter Operator: NOT " + t.text);
      static const char* kOps[] = {"=", "<", "<=", ">", ">=", "<>", "!="};
      bool ok = false;
      for (auto o : kOps)
        if (t.text == o) ok = true;
      if (!ok) throw SqlError("Unbekannter Operator: " + t.text);
      std::string op = t.text;
      if (op == "!=") op = "<>";
      // RHS: current_user? Literal? Spalte (nur bei current_user-LHS)?
      if (peekCurrentUser()) {
        ++pos_;
        if (lhsCur) throw SqlError("current_user = current_user sinnlos");
        c.column = lhsCol;
        c.op = op;
        c.value_is_current = true;
        return c;
      }
      // Literal-Erkennung wie parseJoinCond (Zahl/String/NULL/TRUE/FALSE).
      const Token& u = peek();
      bool isLit =
          (u.kind == TokKind::Integer || u.kind == TokKind::Float ||
           u.kind == TokKind::String);
      if (!isLit && u.kind == TokKind::Symbol &&
          (u.text == "-" || u.text == "+"))
        isLit = true;
      if (!isLit && u.kind == TokKind::Ident) {
        std::string ku = toUpper(u.text);
        if (ku == "NULL" || ku == "TRUE" || ku == "FALSE" ||
            ku == "DEFAULT")
          isLit = true;
      }
      if (isLit) {
        auto [v, cur] = parsePolicyValue();
        (void)cur;
        if (lhsCur) {
          // current_user OP literal: spaltenlose Konstante.
          c.lhs_is_current = true;
          c.op = op;
          c.value = v;
          return c;
        }
        c.column = lhsCol;
        c.op = op;
        c.value = v;
        return c;
      }
      // Nicht-Literal: nur als getauschte Spalte bei current_user-LHS.
      if (!lhsCur)
        throw SqlError("Erwartet Literal oder current_user in POLICY USING");
      if (peek().kind == TokKind::Symbol && peek().text == "(")
        throw SqlError("Subquery in POLICY USING wird nicht unterstuetzt");
      std::string rhsCol = parseColRef();
      // LIKE-Familie nicht tauschbar (sinnlos) -> nur Vergleichs-Ops.
      c.column = rhsCol;
      c.op = invertCmpOp(op);
      c.value_is_current = true;
      return c;
    }
    throw SqlError("Erwartet Operator nach Spaltenname");
  }

  void parsePolicyWhereClause(std::vector<Condition>& where,
                              std::vector<std::vector<Condition>>& groups) {
    std::vector<std::vector<Condition>> tmp;
    while (true) {
      std::vector<Condition> conj;
      conj.push_back(parsePolicyCondition());
      while (matchKeyword("AND")) conj.push_back(parsePolicyCondition());
      tmp.push_back(std::move(conj));
      if (matchKeyword("OR")) continue;
      break;
    }
    if (tmp.size() == 1) {
      where = std::move(tmp[0]);
    } else {
      where.clear();
      groups = std::move(tmp);
    }
  }

  // WHERE als DNF (AND bindet staerker als OR): eine Konjunktion -> `where`,
  // mehrere OR-Gruppen -> `groups`. Von SELECT/UPDATE/DELETE gemeinsam
  // genutzt (Semantik identisch).
  void parseWhereClause(std::vector<Condition>& where,
                        std::vector<std::vector<Condition>>& groups) {
    std::vector<std::vector<Condition>> tmp;
    while (true) {
      std::vector<Condition> conj;
      conj.push_back(parseCondition());
      while (matchKeyword("AND")) conj.push_back(parseCondition());
      tmp.push_back(std::move(conj));
      if (matchKeyword("OR")) continue;
      break;
    }
    if (tmp.size() == 1) {
      where = std::move(tmp[0]);
    } else {
      where.clear();
      groups = std::move(tmp);
    }
  }

  Value parseLiteral() {
    const Token& t = peek();
    if (t.kind == TokKind::Param)
      throw SqlError(
          "Prepared-Statement-Parameter ($1) erst ab V2 (Extended Protocol)");
    // negatives Vorzeichen
    bool neg = false;
    if (t.kind == TokKind::Symbol && (t.text == "-" || t.text == "+")) {
      neg = (t.text == "-");
      ++pos_;
    }
    const Token& u = next();
    if (u.kind == TokKind::Integer) {
      int64_t v = std::stoll(u.text);
      return Value{neg ? -v : v};
    }
    if (u.kind == TokKind::Float) {
      double v = std::stod(u.text);
      return Value{neg ? -v : v};
    }
    if (u.kind == TokKind::String) {
      if (neg) throw SqlError("Ungueltiges Literal");
      // Optionaler :: Typ-Cast (z.B. '{"a":1}'::jsonb) tolerieren
      if (peek().kind == TokKind::Symbol && peek().text == ":") {
        // "::" als zwei ':' lesen
        if (peek(0).text == ":" && peek(1).text == ":") {
          pos_ += 2;
          (void)parseIdent();  // Typname verwerfen
        }
      }
      return Value{u.text};
    }
    if (u.kind == TokKind::Ident) {
      std::string kw = toUpper(u.text);
      if (kw == "NULL") {
        if (neg) throw SqlError("Ungueltiges Literal");
        return Value{std::monostate{}};
      }
      if (kw == "TRUE") return Value{true};
      if (kw == "FALSE") return Value{false};
      // DEFAULT in INSERT -> NULL-Semantik V1
      if (kw == "DEFAULT") return Value{std::monostate{}};
      throw SqlError("Unerwartetes Keyword als Literal: " + u.text);
    }
    throw SqlError("Erwartet Literal (Zahl, String, NULL, TRUE/FALSE)");
  }

  static bool isAggFuncName(const std::string& name) {
    std::string u = toUpper(name);
    return u == "SUM" || u == "AVG" || u == "MIN" || u == "MAX" ||
           u == "COUNT";
  }
  // Aggregat-Start? Ident(SUM/AVG/MIN/MAX/COUNT) gefolgt von '('.
  bool peekAggregate() const {
    const Token& t = peek();
    if (t.kind != TokKind::Ident) return false;
    if (!isAggFuncName(t.text)) return false;
    const Token& n = peek(1);
    return n.kind == TokKind::Symbol && n.text == "(";
  }
  bool peekMul() const {
    const Token& t = peek();
    return t.kind == TokKind::Star ||
           (t.kind == TokKind::Symbol && t.text == "*");
  }

  Aggregate parseAggregate() {
    Token f = next();  // Func-Name (Ident, bereits geprueft)
    std::string func = toUpper(f.text);
    expectSymbol("(");
    Aggregate a;
    a.func = func;
    const Token& t = peek();
    if (t.kind == TokKind::Star ||
        (t.kind == TokKind::Symbol && t.text == "*")) {
      if (func != "COUNT")
        throw SqlError("Nur COUNT(*) unterstuetzt, kein " + func + "(*)");
      ++pos_;
      expectSymbol(")");
      a.star = true;
      a.display = "COUNT(*)";
      return a;
    }
    auto arg = parseAggAddSub();
    expectSymbol(")");
    a.star = false;
    a.arg = std::move(arg);
    a.display = func + "(" + a.arg->display + ")";
    return a;
  }

  // Grammatik (Precedence, links-assoziativ):
  //   addsub := muldiv (('+'|'-') muldiv)*
  //   muldiv := unary (('*'|'/') unary)*
  //   unary  := ('+'|'-') unary | primary
  //   primary:= '(' addsub ')' | Zahl | Spalte | NULL/TRUE/FALSE
  std::shared_ptr<AggExpr> parseAggAddSub() {
    auto left = parseAggMulDiv();
    while (true) {
      const Token& t = peek();
      if (t.kind == TokKind::Symbol && (t.text == "+" || t.text == "-")) {
        char op = t.text[0];
        ++pos_;
        auto right = parseAggMulDiv();
        auto n = std::make_shared<AggExpr>();
        n->kind = AggExpr::Kind::Binary;
        n->op = op;
        n->left = std::move(left);
        n->right = std::move(right);
        n->display =
            n->left->display + std::string(1, op) + n->right->display;
        left = std::move(n);
      } else {
        break;
      }
    }
    return left;
  }

  std::shared_ptr<AggExpr> parseAggMulDiv() {
    auto left = parseAggUnary();
    while (true) {
      bool isMul = peekMul();
      const Token& t = peek();
      bool isDiv = (t.kind == TokKind::Symbol && t.text == "/");
      if (!isMul && !isDiv) break;
      char op = isMul ? '*' : '/';
      ++pos_;
      auto right = parseAggUnary();
      auto n = std::make_shared<AggExpr>();
      n->kind = AggExpr::Kind::Binary;
      n->op = op;
      n->left = std::move(left);
      n->right = std::move(right);
      n->display = n->left->display + std::string(1, op) + n->right->display;
      left = std::move(n);
    }
    return left;
  }

  std::shared_ptr<AggExpr> parseAggUnary() {
    const Token& t = peek();
    if (t.kind == TokKind::Symbol && (t.text == "+" || t.text == "-")) {
      char op = t.text[0];
      ++pos_;
      auto operand = parseAggUnary();
      if (op == '+') {
        auto n = std::make_shared<AggExpr>(*operand);
        n->display = "+" + operand->display;
        return n;
      }
      // unaeres Minus: numerische Literale direkt falten ...
      if (operand->kind == AggExpr::Kind::Literal) {
        if (auto* iv = std::get_if<int64_t>(&operand->literal)) {
          auto n = std::make_shared<AggExpr>();
          n->kind = AggExpr::Kind::Literal;
          n->literal = Value{-(*iv)};
          n->display = "-" + operand->display;
          return n;
        }
        if (auto* dv = std::get_if<double>(&operand->literal)) {
          auto n = std::make_shared<AggExpr>();
          n->kind = AggExpr::Kind::Literal;
          n->literal = Value{-(*dv)};
          n->display = "-" + operand->display;
          return n;
        }
      }
      // ... sonst 0 - operand (DOUBLE-Semantik, NULL propagiert).
      auto zero = std::make_shared<AggExpr>();
      zero->kind = AggExpr::Kind::Literal;
      zero->literal = Value{(int64_t)0};
      zero->display = "0";
      auto n = std::make_shared<AggExpr>();
      n->kind = AggExpr::Kind::Binary;
      n->op = '-';
      n->left = std::move(zero);
      n->right = std::move(operand);
      n->display = "-" + n->right->display;
      return n;
    }
    return parseAggPrimary();
  }

  std::shared_ptr<AggExpr> parseAggPrimary() {
    const Token& t = peek();
    if (t.kind == TokKind::Symbol && t.text == "(") {
      ++pos_;
      auto inner = parseAggAddSub();
      expectSymbol(")");
      auto n = std::make_shared<AggExpr>(*inner);
      n->display = "(" + inner->display + ")";
      return n;
    }
    if (t.kind == TokKind::Integer || t.kind == TokKind::Float) {
      Token u = next();
      auto n = std::make_shared<AggExpr>();
      n->kind = AggExpr::Kind::Literal;
      if (u.kind == TokKind::Integer)
        n->literal = Value{(int64_t)std::stoll(u.text)};
      else
        n->literal = Value{std::stod(u.text)};
      n->display = u.text;
      return n;
    }
    if (t.kind == TokKind::Ident || t.kind == TokKind::QuotedIdent) {
      // TRUE/FALSE/NULL als Literal zulassen (Evaluierung -> Skip/Fehlerpfad)
      if (t.kind == TokKind::Ident) {
        std::string u = toUpper(t.text);
        if (u == "NULL" || u == "TRUE" || u == "FALSE") {
          Token w = next();
          auto n = std::make_shared<AggExpr>();
          n->kind = AggExpr::Kind::Literal;
          if (u == "NULL")
            n->literal = Value{std::monostate{}};
          else
            n->literal = Value{(u == "TRUE")};
          n->display = u;
          (void)w;
          return n;
        }
      }
      std::string col = parseColRef();  // foldet unquoted nach lowercase
      auto n = std::make_shared<AggExpr>();
      n->kind = AggExpr::Kind::Column;
      n->column = col;
      n->display = col;
      return n;
    }
    throw SqlError("Erwartet Spalte, Zahl oder '(' in Aggregat-Argument");
  }

  // Klausel-Keywords: nach einem Aggregat kein Alias (sondern Fortsetzung).
  // Enthält zusätzlich alle reservierten Wörter, die eine Projektion
  // fortsetzen können (WHERE-/ORDER BY-Kontext), damit sie nie als
  // Blank-Alias geschluckt werden.
  bool peekClauseKeyword() const {
    static const char* kws[] = {"FROM", "WHERE", "GROUP", "ORDER",
                                "LIMIT", "OFFSET", "AND",  "OR",
                                "HAVING", "BY",   "ASC",  "DESC",
                                "NULLS",  "FIRST", "LAST", "NOT",
                                "BETWEEN", "IN",  "IS",   "LIKE",
                                "ILIKE",  "NULL", "JOIN", "INNER",
                                "LEFT", "RIGHT", "FULL", "OUTER",
                                "CROSS", "ON"};
    for (auto k : kws)
      if (peekKeyword(k)) return true;
    return false;
  }

  // Optionaler Tabellen-Alias: [AS] name ("" = keiner). Klausel-Keywords
  // (u.a. JOIN/ON/WHERE/...) werden nie als Blank-Alias geschluckt.
  std::string parseOptAlias() {
    if (matchKeyword("AS")) return parseIdent();
    if (peek().kind == TokKind::Ident && !peekClauseKeyword())
      return parseIdent();
    return "";
  }

  // Eine ON-Bedingung: colref (cmp) colref | colref (cmp) literal.
  JoinCond parseJoinCond() {
    JoinCond j;
    j.left = parseColRef();
    const Token& t = next();
    if (t.kind != TokKind::Symbol)
      throw SqlError("Erwartet Vergleichsoperator in JOIN ... ON");
    static const char* kOps[] = {"=", "<", "<=", ">", ">=", "<>", "!="};
    bool ok = false;
    for (auto o : kOps)
      if (t.text == o) ok = true;
    if (!ok) throw SqlError("Unbekannter Operator in JOIN ... ON: " + t.text);
    j.op = t.text;
    if (j.op == "!=") j.op = "<>";
    // Rechte Seite: Literal oder Spaltenreferenz.
    const Token& u = peek();
    bool isLit = (u.kind == TokKind::Integer || u.kind == TokKind::Float ||
                  u.kind == TokKind::String);
    if (!isLit && u.kind == TokKind::Symbol &&
        (u.text == "-" || u.text == "+"))
      isLit = true;
    if (!isLit && u.kind == TokKind::Ident) {
      std::string kw = toUpper(u.text);
      if (kw == "NULL" || kw == "TRUE" || kw == "FALSE" || kw == "DEFAULT")
        isLit = true;
    }
    if (isLit) {
      j.right_is_col = false;
      j.literal = parseLiteral();
    } else {
      j.right_is_col = true;
      j.right = parseColRef();
    }
    return j;
  }

  // JOIN-Auftakt? INNER / JOIN starten einen (unterstuetzten) Inner-Join,
  // LEFT/RIGHT/FULL/OUTER/CROSS werden mit explizitem Fehler abgelehnt.
  bool peekJoinStart() const {
    return peekKeyword("INNER") || peekKeyword("JOIN") ||
           peekKeyword("LEFT") || peekKeyword("RIGHT") ||
           peekKeyword("FULL") || peekKeyword("OUTER") ||
           peekKeyword("CROSS");
  }

  SelectStmt parseSelect() {
    SelectStmt s;
    // Projektion (Reihenfolge in s.items erhalten; ohne GROUP BY homogen)
    if (matchSymbol("*")) {
      s.select_all = true;
    } else {
      s.select_all = false;
      s.count_star = false;
      while (true) {
        if (peekAggregate()) {
          Aggregate a = parseAggregate();
          // Optionaler Alias: "SUM(x) AS s" oder blank "SUM(x) s".
          if (matchKeyword("AS")) {
            a.alias = parseIdent();
          } else if (peek().kind == TokKind::Ident && !peekClauseKeyword()) {
            a.alias = parseIdent();
          }
          s.items.push_back(SelectItem{true, s.aggregates.size()});
          s.aggregates.push_back(std::move(a));
        } else {
          std::string col = parseColRef();
          std::string alias;
          // Optionaler Alias auch fuer Plain-Spalten: "rf AS g" / "rf g".
          if (matchKeyword("AS")) {
            alias = parseIdent();
          } else if (peek().kind == TokKind::Ident && !peekClauseKeyword()) {
            alias = parseIdent();
          }
          s.items.push_back(SelectItem{false, s.columns.size()});
          s.columns.push_back(std::move(col));
          s.column_aliases.push_back(std::move(alias));
        }
        if (matchSymbol(",")) continue;
        break;
      }
      // Legacy-Pfad exakt erhalten: alleiniges COUNT(*) ohne Alias, gefolgt
      // von FROM, -> count_star. Mit GROUP BY danach bleibt es Multi-Aggregat
      // (eine Zeile pro Gruppe statt einer Zeile total).
      if (s.columns.empty() && s.aggregates.size() == 1 &&
          s.items.size() == 1 && s.aggregates[0].star &&
          s.aggregates[0].alias.empty() && peekKeyword("FROM")) {
        s.count_star = true;
        s.aggregates.clear();
        s.items.clear();
      }
    }
    expectKeyword("FROM");
    if (peek().kind == TokKind::Symbol && peek().text == "(")
      throw SqlError(
          "FROM (SELECT ...) wird nicht unterstuetzt (keine Derived Tables)");
    s.table = parseIdent();
    s.table_alias = parseOptAlias();
    // Genau ein optionaler INNER JOIN: [INNER] JOIN u [AS y] ON ... [AND ...].
    if (peekJoinStart()) {
      if (peekKeyword("INNER")) {
        ++pos_;
        expectKeyword("JOIN");
      } else if (peekKeyword("JOIN")) {
        ++pos_;
      } else {
        throw SqlError(
            "Nur INNER JOIN wird unterstuetzt (kein LEFT/RIGHT/FULL/OUTER/"
            "CROSS JOIN)");
      }
      s.has_join = true;
      if (peek().kind == TokKind::Symbol && peek().text == "(")
        throw SqlError(
            "JOIN (SELECT ...) wird nicht unterstuetzt (keine Derived Tables)");
      s.join_table = parseIdent();
      s.join_alias = parseOptAlias();
      expectKeyword("ON");
      while (true) {
        s.join_on.push_back(parseJoinCond());
        if (matchKeyword("AND")) continue;
        break;
      }
      if (s.join_on.empty())
        throw SqlError("JOIN braucht mindestens eine ON-Bedingung");
      if (foldIdent(s.table) == foldIdent(s.join_table)) {
        if (s.table_alias.empty() || s.join_alias.empty() ||
            foldIdent(s.table_alias) == foldIdent(s.join_alias))
          throw SqlError(
              "Self-Join braucht zwei verschiedene Aliase "
              "(FROM t AS x JOIN t AS y ON ...)");
      }
      std::string lEff = s.table_alias.empty() ? s.table : s.table_alias;
      std::string rEff = s.join_alias.empty() ? s.join_table : s.join_alias;
      if (foldIdent(lEff) == foldIdent(rEff) &&
          foldIdent(s.table) != foldIdent(s.join_table))
        throw SqlError("Doppelter Tabellen-Alias im JOIN: " + rEff);
      if (peekJoinStart())
        throw SqlError("Nur ein JOIN pro SELECT wird unterstuetzt");
    }
    if (matchKeyword("WHERE")) {
      parseWhereClause(s.where, s.where_groups);
    }
    if (matchKeyword("GROUP")) {
      expectKeyword("BY");
      if (s.select_all)
        throw SqlError("SELECT * mit GROUP BY wird nicht unterstuetzt");
      while (true) {
        s.group_by.push_back(parseColRef());
        if (matchSymbol(",")) continue;
        break;
      }
      if (s.count_star) {
        // "SELECT COUNT(*) ... GROUP BY": keine Legacy-Einzeiler-Semantik,
        // sondern COUNT(*) pro Gruppe (Multi-Aggregat zurueckbauen).
        s.count_star = false;
        Aggregate a;
        a.func = "COUNT";
        a.star = true;
        a.display = "COUNT(*)";
        s.items.push_back(SelectItem{true, 0});
        s.aggregates.push_back(std::move(a));
      }
    }
    // ORDER BY (1..n Items), danach LIMIT / OFFSET (je max. einmal,
    // Reihenfolge egal). Anwendung: nach Filter/Gruppierung/Aggregation.
    if (peekKeyword("ORDER")) {
      ++pos_;
      expectKeyword("BY");
      while (true) {
        s.order_by.push_back(parseOrderItem());
        if (matchSymbol(",")) continue;
        break;
      }
    }
    while (true) {
      if (peekKeyword("LIMIT") && !s.has_limit) {
        ++pos_;
        parseLimitValue(s, true);
      } else if (peekKeyword("OFFSET") && !s.has_offset) {
        ++pos_;
        parseLimitValue(s, false);
      } else {
        break;
      }
    }
    if (peekKeyword("LIMIT") || peekKeyword("OFFSET"))
      throw SqlError("Doppeltes LIMIT / OFFSET");
    // HAVING -> klare V2-Fehlermeldung statt Silent-Ignore; verirrtes
    // JOIN/ON (zweite JOIN-Kette, ON ohne JOIN, OUTER-Varianten) ebenfalls.
    if (peekKeyword("HAVING"))
      throw SqlError("HAVING erst ab V2");
    if (peekKeyword("LEFT") || peekKeyword("RIGHT") || peekKeyword("FULL") ||
        peekKeyword("OUTER") || peekKeyword("CROSS"))
      throw SqlError(
          "Nur INNER JOIN wird unterstuetzt (kein LEFT/RIGHT/FULL/OUTER/"
          "CROSS JOIN)");
    if (peekKeyword("JOIN") || peekKeyword("INNER"))
      throw SqlError(
          "JOIN nur direkt nach FROM (nur ein JOIN pro SELECT wird "
          "unterstuetzt)");
    if (peekKeyword("ON"))
      throw SqlError("ON ohne JOIN wird nicht unterstuetzt");
    return s;
  }

  OrderByItem parseOrderItem() {
    OrderByItem o;
    if (peek().kind == TokKind::Integer) {
      Token u = next();
      int64_t v = 0;
      try {
        v = std::stoll(u.text);
      } catch (...) {
        throw SqlError("Ungueltige ORDER BY-Position");
      }
      if (v <= 0) throw SqlError("ORDER BY-Position muss >= 1 sein");
      o.is_ordinal = true;
      o.ordinal = v;
    } else if (peekAggregate()) {
      o.is_agg = true;
      o.agg = parseAggregate();
    } else {
      o.column = parseColRef();
    }
    if (matchKeyword("ASC")) {
      o.desc = false;
    } else if (matchKeyword("DESC")) {
      o.desc = true;
    }
    if (matchKeyword("NULLS")) {
      if (matchKeyword("FIRST")) {
        o.has_nulls = true;
        o.nulls_first = true;
      } else if (matchKeyword("LAST")) {
        o.has_nulls = true;
        o.nulls_first = false;
      } else {
        throw SqlError("Erwartet FIRST oder LAST nach NULLS");
      }
    } else {
      // PG-Default: ASC -> NULLS LAST, DESC -> NULLS FIRST.
      o.nulls_first = o.desc;
    }
    return o;
  }

  void parseLimitValue(SelectStmt& s, bool is_limit) {
    const char* what = is_limit ? "LIMIT" : "OFFSET";
    if (peekKeyword("ALL")) {
      if (!is_limit)
        throw SqlError("Erwartet OFFSET-Wert (nicht-negative Ganzzahl)");
      ++pos_;
      s.has_limit = false;  // LIMIT ALL = kein Limit (PG)
      return;
    }
    bool neg = false;
    if (peek().kind == TokKind::Symbol &&
        (peek().text == "-" || peek().text == "+")) {
      neg = (peek().text == "-");
      ++pos_;
    }
    if (peek().kind != TokKind::Integer)
      throw SqlError(std::string("Erwartet ") + what +
                     "-Wert (nicht-negative Ganzzahl)");
    Token w = next();
    int64_t v = 0;
    try {
      v = std::stoll(w.text);
    } catch (...) {
      throw SqlError(std::string("Ungueltiger ") + what + "-Wert");
    }
    if (neg) v = -v;
    if (v < 0) throw SqlError(std::string(what) + " darf nicht negativ sein");
    if (is_limit) {
      s.has_limit = true;
      s.limit = v;
    } else {
      s.has_offset = true;
      s.offset = v;
    }
  }

  Condition parseCondition() {
    Condition c;
    c.column = parseColRef();
    // Optionales NOT-Praefix: NOT BETWEEN / NOT IN / NOT LIKE / NOT ILIKE.
    bool neg = false;
    if (peekKeyword("NOT")) {
      ++pos_;
      neg = true;
    }
    const Token& t = next();
    if (t.kind == TokKind::Ident) {
      std::string kw = toUpper(t.text);
      if (kw == "LIKE" || kw == "ILIKE") {
        c.op = neg ? ("NOT " + kw) : kw;
        c.value = parseLiteral();
        return c;
      }
      if (kw == "BETWEEN") {
        c.op = neg ? "NOT BETWEEN" : "BETWEEN";
        c.value = parseLiteral();
        expectKeyword("AND");
        c.second = parseLiteral();
        return c;
      }
      if (kw == "IN") {
        c.op = neg ? "NOT IN" : "IN";
        expectSymbol("(");
        if (peekKeyword("SELECT")) {
          // Unkorrelierte IN-Subquery: col IN (SELECT c FROM t2 [WHERE ...]).
          if (subdepth_ >= kMaxSubqueryDepth)
            throw SqlError("Subquery-Tiefe ueberschritten (max 8)");
          expectKeyword("SELECT");
          ++subdepth_;
          SelectStmt sub;
          try {
            sub = parseSelect();
          } catch (...) {
            --subdepth_;
            throw;
          }
          --subdepth_;
          expectSymbol(")");
          c.subquery = std::make_shared<SelectStmt>(std::move(sub));
          return c;
        }
        if (peek().kind == TokKind::Symbol && peek().text == ")")
          throw SqlError("IN-Liste darf nicht leer sein");
        while (true) {
          c.list.push_back(parseLiteral());
          if (matchSymbol(",")) continue;
          break;
        }
        expectSymbol(")");
        return c;
      }
      if (kw == "IS") {
        if (neg) throw SqlError("Unbekannter Operator: NOT IS");
        // IS [NOT] NULL
        bool is_not = false;
        if (peekKeyword("NOT")) {
          ++pos_;
          is_not = true;
        }
        expectKeyword("NULL");
        c.op = is_not ? "IS NOT NULL" : "IS NULL";
        c.value = Value{std::monostate{}};
        return c;
      }
      throw SqlError("Unbekannter Operator: " +
                     (neg ? ("NOT " + t.text) : t.text));
    }
    if (t.kind == TokKind::Symbol) {
      if (neg) throw SqlError("Unbekannter Operator: NOT " + t.text);
      static const char* kOps[] = {"=", "<", "<=", ">", ">=", "<>", "!="};
      bool ok = false;
      for (auto o : kOps)
        if (t.text == o) ok = true;
      if (!ok) throw SqlError("Unbekannter Operator: " + t.text);
      c.op = t.text;
      if (c.op == "!=") c.op = "<>";
      // Skalare Subquery: col =(SELECT ...) / Vergleiche gegen
      // Single-Row-Single-Col-Subquery.
      if (peek().kind == TokKind::Symbol && peek().text == "(" &&
          peekKeyword("SELECT", 1)) {
        if (subdepth_ >= kMaxSubqueryDepth)
          throw SqlError("Subquery-Tiefe ueberschritten (max 8)");
        expectSymbol("(");
        expectKeyword("SELECT");
        ++subdepth_;
        SelectStmt sub;
        try {
          sub = parseSelect();
        } catch (...) {
          --subdepth_;
          throw;
        }
        --subdepth_;
        expectSymbol(")");
        c.subquery = std::make_shared<SelectStmt>(std::move(sub));
        return c;
      }
      c.value = parseLiteral();
      return c;
    }
    throw SqlError("Erwartet Operator nach Spaltenname");
  }

  std::vector<Token> toks_;
  std::size_t pos_ = 0;
  int subdepth_ = 0;  // Verschachtelungstiefe von WHERE-Subqueries (max 8)
};

struct LikeKey {
  std::string rx;
  bool ci = false;
  bool operator==(const LikeKey& o) const { return ci == o.ci && rx == o.rx; }
};
struct LikeKeyHash {
  std::size_t operator()(const LikeKey& k) const noexcept {
    std::size_t h = std::hash<std::string>{}(k.rx);
    return k.ci ? (h ^ 0x9e3779b9u) : h;
  }
};

bool likeMatch(const std::string& s, const std::string& pat, bool ci) {
  // SQL LIKE -> Regex: % -> .*, _ -> ., Rest escapen
  std::string rx;
  rx.reserve(pat.size() * 2 + 4);
  rx += "^";
  for (char c : pat) {
    if (c == '%')
      rx += ".*";
    else if (c == '_')
      rx += ".";
    else if (std::string(".^$|()[]{}*+?\\").find(c) != std::string::npos) {
      rx += '\\';
      rx += c;
    } else {
      rx += c;
    }
  }
  rx += "$";
  // Kompilierte Regexe pro (Muster, Case-Flag) wiederverwenden: gleiche
  // Semantik wie ein frisches std::regex pro Aufruf, aber kein Recompile pro
  // Row (der heisse Pfad wertet LIKE je Zeile aus). Thread-lokal (kein Lock),
  // gedeckelt (Speicher beschraenkt, Korrektheit unabhaengig vom
  // Cache-Inhalt). Fehlgeschlagene Kompilierung wirft wie bisher und wird
  // nicht gecacht.
  thread_local std::unordered_map<LikeKey, std::regex, LikeKeyHash> cache;
  LikeKey key{rx, ci};
  auto it = cache.find(key);
  if (it == cache.end()) {
    if (cache.size() >= 256) cache.clear();
    std::regex::flag_type f = std::regex::ECMAScript;
    if (ci) f |= std::regex::icase;
    std::regex re(rx, f);  // wirft ggf. wie bisher (regex_error)
    it = cache.emplace(std::move(key), std::move(re)).first;
  }
  return std::regex_match(s, it->second);
}

int compareValues(const Value& a, const Value& b) {
  // numerisch tolerant: int vs double
  if (valueIsNull(a) || valueIsNull(b)) return -2;  // NULL separat
  if (auto* ai = std::get_if<int64_t>(&a)) {
    if (auto* bi = std::get_if<int64_t>(&b))
      return (*ai < *bi) ? -1 : (*ai > *bi) ? 1 : 0;
    if (auto* bd = std::get_if<double>(&b)) {
      double ad = (double)*ai;
      return (ad < *bd) ? -1 : (ad > *bd) ? 1 : 0;
    }
  }
  if (auto* ad = std::get_if<double>(&a)) {
    if (auto* bi = std::get_if<int64_t>(&b)) {
      double bd = (double)*bi;
      return (*ad < bd) ? -1 : (*ad > bd) ? 1 : 0;
    }
    if (auto* bd = std::get_if<double>(&b))
      return (*ad < *bd) ? -1 : (*ad > *bd) ? 1 : 0;
  }
  if (auto* as = std::get_if<std::string>(&a)) {
    if (auto* bs = std::get_if<std::string>(&b))
      return as->compare(*bs);
    // Zahl vs Text: Textvergleich via valueToString
    std::string bs2 = valueToString(b);
    return as->compare(bs2);
  }
  if (auto* ab = std::get_if<bool>(&a)) {
    if (auto* bb = std::get_if<bool>(&b))
      return (*ab == *bb) ? 0 : (*ab ? 1 : -1);
  }
  // Fallback: String-Vergleich
  std::string sa = valueToString(a), sb = valueToString(b);
  return sa.compare(sb);
}

bool evalCondition(const Table& t, const std::vector<Value>& row,
                   const Condition& c) {
  if (c.subquery)
    throw SqlError("Subquery ohne Ausfuehrungskontext (interner Fehler)");
  int idx = t.colIndex(c.column);
  if (idx < 0) throw SqlError("Unbekannte Spalte in WHERE: " + c.column);
  const Value& v = row[(std::size_t)idx];
  if (c.op == "IS NULL") return valueIsNull(v);
  if (c.op == "IS NOT NULL") return !valueIsNull(v);
  if (c.op == "IN" || c.op == "NOT IN") {
    // c.value ist hier unbesetzt (NULL-Sentinel) -> eigene NULL-Behandlung:
    // Zeilen-NULL -> false; Listen-NULL matcht nie (IN), bzw. macht
    // NOT IN zu UNKNOWN -> filtern (PG).
    if (valueIsNull(v)) return false;
    bool has_null = false;
    for (auto& e : c.list) {
      if (valueIsNull(e)) {
        has_null = true;
        continue;
      }
      if (compareValues(v, e) == 0) return (c.op == "IN");
    }
    if (c.op == "IN") return false;
    return !has_null;
  }
  if (valueIsNull(v) || valueIsNull(c.value)) return false;  // NULL -> false
  if (c.op == "BETWEEN" || c.op == "NOT BETWEEN") {
    // Inklusiv, PG: NULL (Zeile oder Grenze) -> UNKNOWN -> filtern.
    if (valueIsNull(c.second)) return false;
    int lo = compareValues(v, c.value);
    int hi = compareValues(v, c.second);
    if (lo == -2 || hi == -2) return false;
    bool in = (lo >= 0 && hi <= 0);
    return (c.op == "BETWEEN") ? in : !in;
  }
  if (c.op == "LIKE" || c.op == "ILIKE" || c.op == "NOT LIKE" ||
      c.op == "NOT ILIKE") {
    auto* vs = std::get_if<std::string>(&v);
    auto* ps = std::get_if<std::string>(&c.value);
    if (!vs || !ps)
      throw SqlError(c.op + " braucht TEXT-Operanden");
    bool m = likeMatch(*vs, *ps, c.op == "ILIKE" || c.op == "NOT ILIKE");
    return (c.op == "LIKE" || c.op == "ILIKE") ? m : !m;
  }
  int cmp = compareValues(v, c.value);
  if (cmp == -2) return false;
  if (c.op == "=") return cmp == 0;
  if (c.op == "<>") return cmp != 0;
  if (c.op == "<") return cmp < 0;
  if (c.op == "<=") return cmp <= 0;
  if (c.op == ">") return cmp > 0;
  if (c.op == ">=") return cmp >= 0;
  throw SqlError("Unbekannter Operator: " + c.op);
}

// WHERE als DNF: ohne OR gilt where (AND), mit OR gelten where_groups
// (OR von AND-Konjunktionen, AND bindet staerker).
bool evalWhere(const Table& t, const std::vector<Value>& row,
               const SelectStmt& s) {
  if (!s.where_groups.empty()) {
    for (auto& conj : s.where_groups) {
      bool ok = true;
      for (auto& c : conj) {
        if (!evalCondition(t, row, c)) {
          ok = false;
          break;
        }
      }
      if (ok) return true;
    }
    return false;
  }
  for (auto& c : s.where)
    if (!evalCondition(t, row, c)) return false;
  return true;
}

// 3-Wege-Vergleich fuer ORDER BY (Richtung + NULLs bereits
// aufgeloest). NULL vs. non-NULL via nulls_first, non-NULL via compareValues
// (numerisch tolerant, bestehende Vergleichssemantik).
int compareOrdered(const Value& a, const Value& b, bool desc,
                   bool nulls_first) {
  bool an = valueIsNull(a);
  bool bn = valueIsNull(b);
  if (an && bn) return 0;
  if (an) return nulls_first ? -1 : 1;
  if (bn) return nulls_first ? 1 : -1;
  int c = compareValues(a, b);
  if (c == -2) return 0;  // unerreichbar (NULL oben behandelt)
  if (desc) c = -c;
  return (c < 0) ? -1 : ((c > 0) ? 1 : 0);
}

// LIMIT/OFFSET-Slice auf dem fertigen Result (nach Projektion/Aggregation).
// Negative Werte -> SqlError (Parse stellt das sicher, hier als zweite
// Huerde fuer handgebaute Statements). Aktualisiert message/affected.
void applyLimitOffset(Result& r, const SelectStmt& s) {
  if (s.has_limit && s.limit < 0)
    throw SqlError("LIMIT darf nicht negativ sein");
  if (s.has_offset && s.offset < 0)
    throw SqlError("OFFSET darf nicht negativ sein");
  std::size_t off =
      s.has_offset ? static_cast<std::size_t>(s.offset) : std::size_t{0};
  std::size_t lim = s.has_limit ? static_cast<std::size_t>(s.limit)
                                : std::numeric_limits<std::size_t>::max();
  std::vector<std::vector<Value>> out;
  if (off < r.rows.size()) {
    std::size_t avail = r.rows.size() - off;
    std::size_t take = std::min(avail, lim);
    out.reserve(take);
    for (std::size_t i = 0; i < take; ++i)
      out.push_back(std::move(r.rows[off + i]));
  }
  r.rows = std::move(out);
  r.message = "SELECT " + std::to_string(r.rows.size());
  r.affected = r.rows.size();
}

Value coerceTo(const Value& v, ColType type, const std::string& col) {
  if (valueIsNull(v)) return v;
  switch (type) {
    case ColType::Int:
      if (std::holds_alternative<int64_t>(v)) return v;
      if (auto* d = std::get_if<double>(&v)) return Value{(int64_t)*d};
      if (auto* s = std::get_if<std::string>(&v)) {
        try {
          return Value{(int64_t)std::stoll(*s)};
        } catch (...) {
          throw SqlError("Typfehler: '" + *s + "' kein INT (" + col + ")");
        }
      }
      if (auto* b = std::get_if<bool>(&v))
        return Value{(int64_t)(*b ? 1 : 0)};
      break;
    case ColType::Double:
      if (std::holds_alternative<double>(v)) return v;
      if (auto* i = std::get_if<int64_t>(&v)) return Value{(double)*i};
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
  // lockere Koerzierung: durchlassen (V1), ausser INT/DOUBLE oben
  return v;
}

// ---------- Skalare Aggregate (ohne GROUP BY) ----------

double aggToDouble(const Value& v) {
  if (auto* i = std::get_if<int64_t>(&v)) return static_cast<double>(*i);
  if (auto* d = std::get_if<double>(&v)) return *d;
  throw SqlError("Aggregat-Ausdruck braucht numerische Operanden");
}

// Wert eines Aggregat-Arguments fuer eine Row. Wirft SqlError bei unbekannter
// Spalte; Division durch 0 -> NULL (kein Fehler, Zeile wird geskippt).
Value evalAggExprNode(const Table& t, const std::vector<Value>& row,
                      const AggExpr& e) {
  switch (e.kind) {
    case AggExpr::Kind::Column: {
      int idx = t.colIndex(e.column);
      if (idx < 0) throw SqlError("Unbekannte Spalte: " + e.column);
      return row[static_cast<std::size_t>(idx)];
    }
    case AggExpr::Kind::Literal:
      return e.literal;
    case AggExpr::Kind::Binary: {
      Value lv = evalAggExprNode(t, row, *e.left);
      Value rv = evalAggExprNode(t, row, *e.right);
      if (valueIsNull(lv) || valueIsNull(rv))
        return Value{std::monostate{}};
      if (std::holds_alternative<std::string>(lv) ||
          std::holds_alternative<std::string>(rv) ||
          std::holds_alternative<bool>(lv) ||
          std::holds_alternative<bool>(rv))
        throw SqlError("Aggregat-Ausdruck braucht numerische Operanden");
      double a = aggToDouble(lv);
      double b = aggToDouble(rv);
      switch (e.op) {
        case '+':
          return Value{a + b};
        case '-':
          return Value{a - b};
        case '*':
          return Value{a * b};
        case '/':
          if (b == 0.0) return Value{std::monostate{}};
          return Value{a / b};
        default:
          break;
      }
      throw SqlError("Unbekannter Operator in Aggregat");
    }
  }
  throw SqlError("Ungueltiger Aggregat-Ausdruck");
}

void requireNumericForSumAvg(const Value& v, const std::string& func) {
  if (std::holds_alternative<std::string>(v) ||
      std::holds_alternative<bool>(v))
    throw SqlError(func + " braucht numerische Operanden");
}

// Ein Aggregat ueber eine Zeilenmenge (s39-Semantik, pro Gruppe wiederverwendet):
// COUNT(*)=Zeilen, COUNT(col)=non-null, SUM/AVG ueber non-null numerisch
// (leer -> NULL), MIN/MAX ueber non-null (leer -> NULL, Typ bleibt erhalten).
Value computeAggregate(const Table& t,
                       const std::vector<const std::vector<Value>*>& rows,
                       const Aggregate& a) {
  if (a.star) {  // COUNT(*)
    return Value{static_cast<int64_t>(rows.size())};
  }
  if (!a.arg) throw SqlError("Aggregat ohne Argument: " + a.func);
  if (a.func == "COUNT") {
    int64_t c = 0;
    for (auto rp : rows) {
      Value v = evalAggExprNode(t, *rp, *a.arg);
      if (!valueIsNull(v)) ++c;
    }
    return Value{c};
  }
  if (a.func == "SUM") {
    bool any = false;
    double sum = 0.0;
    for (auto rp : rows) {
      Value v = evalAggExprNode(t, *rp, *a.arg);
      if (valueIsNull(v)) continue;
      requireNumericForSumAvg(v, "SUM");
      sum += aggToDouble(v);
      any = true;
    }
    return any ? Value{sum} : Value{std::monostate{}};
  }
  if (a.func == "AVG") {
    double sum = 0.0;
    int64_t n = 0;
    for (auto rp : rows) {
      Value v = evalAggExprNode(t, *rp, *a.arg);
      if (valueIsNull(v)) continue;
      requireNumericForSumAvg(v, "AVG");
      sum += aggToDouble(v);
      ++n;
    }
    return n > 0 ? Value{sum / static_cast<double>(n)}
                 : Value{std::monostate{}};
  }
  if (a.func == "MIN" || a.func == "MAX") {
    bool any = false;
    Value best{std::monostate{}};
    for (auto rp : rows) {
      Value v = evalAggExprNode(t, *rp, *a.arg);
      if (valueIsNull(v)) continue;
      if (!any) {
        best = v;
        any = true;
      } else {
        int cmp = compareValues(v, best);
        if (a.func == "MIN" ? (cmp < 0) : (cmp > 0)) best = v;
      }
    }
    return any ? best : Value{std::monostate{}};
  }
  throw SqlError("Unbekannte Aggregatfunktion: " + a.func);
}

// ORDER BY-Referenzen auf 1-zeiligen Aggregat-Results (skalar oder
// COUNT(*)-Legacy) validieren. Sortieren ist dort ein No-Op, unbekannte
// Spalten/Positionen muessen aber trotzdem fehlschlagen. Aggregat-Items,
// die nicht projiziert sind, werden einmal ausgewertet und verworfen
// (nur zur Spalten-Validierung, PG-kompatible Fehlermeldung).
void validateOrderScalar(const Table& t,
                         const std::vector<std::vector<Value>>& kept,
                         const SelectStmt& s, const Result& r) {
  for (auto& o : s.order_by) {
    if (o.is_ordinal) {
      if (o.ordinal < 1 || static_cast<std::size_t>(o.ordinal) > r.columns.size())
        throw SqlError("ORDER BY-Position ausserhalb der Projektion");
    } else if (o.is_agg) {
      bool known = false;
      for (auto& a : s.aggregates)
        if (a.display == o.agg.display && a.func == o.agg.func) {
          known = true;
          break;
        }
      if (!known) {
        std::vector<const std::vector<Value>*> refs;
        refs.reserve(kept.size());
        for (auto& row : kept) refs.push_back(&row);
        (void)computeAggregate(t, refs, o.agg);
      }
    } else {
      bool ok = false;
      for (auto& c : r.columns)
        if (foldIdent(c) == foldIdent(o.column)) {
          ok = true;
          break;
        }
      if (!ok) throw SqlError("Unbekannte Spalte in ORDER BY: " + o.column);
    }
  }
}

// Ein-Zeilen-Result. Empty-Set: COUNT->0, Rest NULL. Sonst NULL-Skip.
// ORDER BY ist hier ein validierter No-Op, LIMIT/OFFSET schneiden die
// eine Zeile (LIMIT 0 / OFFSET >= 1 -> 0 Zeilen).
Result execScalarAggregates(const Table& t,
                            const std::vector<std::vector<Value>>& kept,
                            const SelectStmt& s) {
  Result r;
  r.columns.reserve(s.aggregates.size());
  for (auto& a : s.aggregates)
    r.columns.push_back(a.alias.empty() ? a.display : a.alias);
  std::vector<const std::vector<Value>*> refs;
  refs.reserve(kept.size());
  for (auto& row : kept) refs.push_back(&row);
  std::vector<Value> out;
  out.reserve(s.aggregates.size());
  for (auto& a : s.aggregates) out.push_back(computeAggregate(t, refs, a));
  r.rows.push_back(std::move(out));
  validateOrderScalar(t, kept, s, r);
  applyLimitOffset(r, s);
  return r;
}

// Gruppenschluessel-Feld als String (laengenpraefixiert -> kollisionssicher
// ueber 1..n Spalten; DOUBLE mit voller Roundtrip-Praezision; NULL gruppiert
// wie in PG als eigene Gruppe, d.h. NULL == NULL beim Gruppieren).
std::string groupKeyField(const Value& v) {
  if (std::holds_alternative<std::monostate>(v)) return "N;";
  if (auto* iv = std::get_if<int64_t>(&v))
    return "I:" + std::to_string(*iv) + ";";
  if (auto* dv = std::get_if<double>(&v)) {
    std::ostringstream o;
    o << std::setprecision(17) << *dv;
    std::string p = o.str();
    return "F:" + std::to_string(p.size()) + ":" + p + ";";
  }
  if (auto* sv = std::get_if<std::string>(&v))
    return "S:" + std::to_string(sv->size()) + ":" + *sv + ";";
  if (auto* bv = std::get_if<bool>(&v))
    return std::string("B:") + (*bv ? "1;" : "0;");
  return "X:" + valueToString(v) + ";";
}

// Eine GROUP BY-Gruppe: Key in group_by-Reihenfolge + Member-Zeilen.
// (Namespace-Scope, damit die ORDER BY-Sortierung darauf zugreifen kann;
// r.rows[i] korrespondiert zu groups[i].)
struct Group {
  std::vector<Value> key;
  std::vector<const std::vector<Value>*> rows;
};

// ORDER BY auf ungefiltert-projizierten Plain-Zeilen (kein GROUP BY, keine
// Aggregate): sortiert die vollen Tabellen-Zeilen VOR der Projektion, damit
// auch nicht-projizierte Spalten als Sortierschluessel taugen (PG).
// Aufloesung je Item: Ausgabe-Alias/Spalte zuerst, dann Tabellenspalte,
// sonst SqlError. Aggregate/Positionsfehler -> SqlError. Stabil (Ties
// behalten Einfuegereihenfolge).
void sortPlainRows(const Table& t, std::vector<std::vector<Value>>& kept,
                   const SelectStmt& s) {
  struct Key {
    int col = -1;
    bool desc = false;
    bool nulls_first = false;
  };
  auto resolveName = [&](const std::string& name) -> int {
    if (!s.select_all) {
      for (std::size_t i = 0; i < s.columns.size(); ++i) {
        std::string alias;
        if (i < s.column_aliases.size()) alias = s.column_aliases[i];
        const std::string& outname = alias.empty() ? s.columns[i] : alias;
        if (foldIdent(outname) == foldIdent(name)) {
          int ti = t.colIndex(s.columns[i]);
          if (ti < 0) throw SqlError("Unbekannte Spalte in ORDER BY: " + name);
          return ti;
        }
      }
    }
    int ti = t.colIndex(name);
    if (ti < 0) throw SqlError("Unbekannte Spalte in ORDER BY: " + name);
    return ti;
  };
  auto resolveOrdinal = [&](int64_t ord) -> int {
    std::size_t n_out =
        s.select_all ? t.columns.size() : s.columns.size();
    if (ord < 1 || static_cast<std::size_t>(ord) > n_out)
      throw SqlError("ORDER BY-Position ausserhalb der Projektion");
    if (s.select_all) return static_cast<int>(ord - 1);
    int ti = t.colIndex(s.columns[static_cast<std::size_t>(ord - 1)]);
    if (ti < 0) throw SqlError("Unbekannte Spalte in ORDER BY");
    return ti;
  };
  std::vector<Key> keys;
  keys.reserve(s.order_by.size());
  for (auto& o : s.order_by) {
    Key k;
    k.desc = o.desc;
    k.nulls_first = o.has_nulls ? o.nulls_first : o.desc;
    if (o.is_agg)
      throw SqlError(
          "ORDER BY mit Aggregat nur mit GROUP BY oder Aggregat-Projektion");
    else if (o.is_ordinal)
      k.col = resolveOrdinal(o.ordinal);
    else
      k.col = resolveName(o.column);
    keys.push_back(k);
  }
  std::stable_sort(kept.begin(), kept.end(),
                   [&](const std::vector<Value>& a,
                       const std::vector<Value>& b) {
                     for (auto& k : keys) {
                       int c = compareOrdered(
                           a[static_cast<std::size_t>(k.col)],
                           b[static_cast<std::size_t>(k.col)], k.desc,
                           k.nulls_first);
                       if (c != 0) return c < 0;
                     }
                     return false;
                   });
}

// Aufgeloester Gruppierungs-Sortierschluessel: Result-Spalte (Alias,
// Gruppen-Spalte, projiziertes Aggregat), GROUP BY-Key (gruppiert, aber
// nicht projiziert) oder frisch pro Gruppe berechnetes Aggregat
// (ORDER BY-Aggregat ausserhalb der Projektion).
struct GroupOrderKey {
  enum class Kind { Result, GroupKey, Computed };
  Kind kind = Kind::Result;
  std::size_t pos = 0;  // Result-Position bzw. group_by-Position
  Aggregate agg;        // bei Computed
  bool desc = false;
  bool nulls_first = false;
};

// ORDER BY auf gruppierten Results (nach Aggregation). Aufloesung je Item:
// Ordinal -> Ausgabeposition; Aggregat -> projiziert (Display-Match) oder
// pro Gruppe berechnet; Name -> Ausgabespalte/Alias, sonst GROUP BY-Spalte,
// sonst SqlError. Unsortiert bleibt First-Seen-Reihenfolge. Stabil.
void sortGroupedRows(const Table& t, const std::vector<Group>& groups,
                     const SelectStmt& s, Result& r) {
  std::vector<GroupOrderKey> keys;
  keys.reserve(s.order_by.size());
  for (auto& o : s.order_by) {
    GroupOrderKey k;
    k.desc = o.desc;
    k.nulls_first = o.has_nulls ? o.nulls_first : o.desc;
    if (o.is_ordinal) {
      if (o.ordinal < 1 ||
          static_cast<std::size_t>(o.ordinal) > r.columns.size())
        throw SqlError("ORDER BY-Position ausserhalb der Projektion");
      k.kind = GroupOrderKey::Kind::Result;
      k.pos = static_cast<std::size_t>(o.ordinal - 1);
    } else if (o.is_agg) {
      int rpos = -1;
      for (std::size_t i = 0; i < r.columns.size(); ++i)
        if (foldIdent(r.columns[i]) == foldIdent(o.agg.display)) {
          rpos = static_cast<int>(i);
          break;
        }
      // Alias-Treffer: ORDER BY <alias> waere als Name gekommen; hier nur
      // Display-Match, sonst pro Gruppe frisch berechnen.
      if (rpos >= 0) {
        k.kind = GroupOrderKey::Kind::Result;
        k.pos = static_cast<std::size_t>(rpos);
      } else {
        k.kind = GroupOrderKey::Kind::Computed;
        k.agg = o.agg;
      }
    } else {
      int rpos = -1;
      for (std::size_t i = 0; i < r.columns.size(); ++i)
        if (foldIdent(r.columns[i]) == foldIdent(o.column)) {
          rpos = static_cast<int>(i);
          break;
        }
      if (rpos >= 0) {
        k.kind = GroupOrderKey::Kind::Result;
        k.pos = static_cast<std::size_t>(rpos);
      } else {
        int gpos = -1;
        for (std::size_t i = 0; i < s.group_by.size(); ++i)
          if (foldIdent(s.group_by[i]) == foldIdent(o.column)) {
            gpos = static_cast<int>(i);
            break;
          }
        if (gpos < 0)
          throw SqlError("Unbekannte Spalte in ORDER BY: " + o.column);
        k.kind = GroupOrderKey::Kind::GroupKey;
        k.pos = static_cast<std::size_t>(gpos);
      }
    }
    keys.push_back(std::move(k));
  }
  std::size_t n = r.rows.size();
  std::vector<std::vector<Value>> kvals(n);
  for (std::size_t i = 0; i < n; ++i) {
    kvals[i].reserve(keys.size());
    for (auto& k : keys) {
      if (k.kind == GroupOrderKey::Kind::Result)
        kvals[i].push_back(r.rows[i][k.pos]);
      else if (k.kind == GroupOrderKey::Kind::GroupKey)
        kvals[i].push_back(groups[i].key[k.pos]);
      else
        kvals[i].push_back(computeAggregate(t, groups[i].rows, k.agg));
    }
  }
  std::vector<std::size_t> idx(n);
  for (std::size_t i = 0; i < n; ++i) idx[i] = i;
  std::stable_sort(idx.begin(), idx.end(),
                   [&](std::size_t a, std::size_t b) {
                     for (std::size_t j = 0; j < keys.size(); ++j) {
                       int c = compareOrdered(kvals[a][j], kvals[b][j],
                                              keys[j].desc,
                                              keys[j].nulls_first);
                       if (c != 0) return c < 0;
                     }
                     return false;
                   });
  std::vector<std::vector<Value>> sorted;
  sorted.reserve(n);
  for (auto i : idx) sorted.push_back(std::move(r.rows[i]));
  r.rows = std::move(sorted);
}

// Hash-Aggregation (s43, Q1-Kern): eine Zeile pro Gruppe in
// First-Seen-Reihenfolge (ohne ORDER BY), Spalten in SELECT-Reihenfolge
// (Gruppen-Spalten + Aggregate gemischt). Leere Eingabe -> 0 Gruppen
// (keine Zeile).
Result execGroupedAggregates(const Table& t,
                             const std::vector<std::vector<Value>>& kept,
                             const SelectStmt& s) {
  if (s.select_all || s.count_star)
    throw SqlError("SELECT * / COUNT(*) mit GROUP BY wird nicht unterstuetzt");
  // GROUP-Spalten aufloesen (unbekannt -> SqlError)
  std::vector<int> gidx;
  gidx.reserve(s.group_by.size());
  for (auto& g : s.group_by) {
    int idx = t.colIndex(g);
    if (idx < 0) throw SqlError("Unbekannte Spalte in GROUP BY: " + g);
    gidx.push_back(idx);
  }
  // Plain-Spalten muessen gruppiert sein (PG-Semantik)
  for (auto& c : s.columns) {
    bool ok = false;
    for (auto& g : s.group_by)
      if (foldIdent(g) == foldIdent(c)) {
        ok = true;
        break;
      }
    if (!ok)
      throw SqlError("Spalte '" + c + "' muss in GROUP BY erscheinen");
  }
  // Hash-Partitionierung (Key = Wert-Tupel), Gruppen in First-Seen-Ordnung
  std::vector<Group> groups;
  std::unordered_map<std::string, std::size_t> pos;
  for (auto& row : kept) {
    std::string k;
    std::vector<Value> kv;
    kv.reserve(gidx.size());
    for (int gi : gidx) {
      const Value& v = row[static_cast<std::size_t>(gi)];
      kv.push_back(v);
      k += groupKeyField(v);
    }
    auto it = pos.find(k);
    if (it == pos.end()) {
      std::size_t id = groups.size();
      pos.emplace(k, id);
      Group g;
      g.key = std::move(kv);
      g.rows.push_back(&row);
      groups.push_back(std::move(g));
    } else {
      groups[it->second].rows.push_back(&row);
    }
  }
  // Plain-Spalte -> Position im GROUP BY (fuer Key-Lookup; oben validiert)
  std::vector<std::size_t> colPos;
  colPos.reserve(s.columns.size());
  for (auto& c : s.columns) {
    std::size_t p = 0;
    for (; p < s.group_by.size(); ++p)
      if (foldIdent(s.group_by[p]) == foldIdent(c)) break;
    colPos.push_back(p);
  }
  // Projektion in SELECT-Reihenfolge (items leer = vor-s43-AST: erst Spalten)
  std::vector<SelectItem> items = s.items;
  if (items.empty()) {
    for (std::size_t i = 0; i < s.columns.size(); ++i)
      items.push_back(SelectItem{false, i});
    for (std::size_t i = 0; i < s.aggregates.size(); ++i)
      items.push_back(SelectItem{true, i});
  }
  Result r;
  r.columns.reserve(items.size());
  for (auto& it : items) {
    if (it.is_agg) {
      if (it.index >= s.aggregates.size())
        throw SqlError("Ungueltige Projektion (Aggregat-Index)");
      const Aggregate& a = s.aggregates[it.index];
      r.columns.push_back(a.alias.empty() ? a.display : a.alias);
    } else {
      if (it.index >= s.columns.size())
        throw SqlError("Ungueltige Projektion (Spalten-Index)");
      int idx = t.colIndex(s.columns[it.index]);
      if (idx < 0) throw SqlError("Unbekannte Spalte: " + s.columns[it.index]);
      std::string alias;
      if (it.index < s.column_aliases.size()) alias = s.column_aliases[it.index];
      r.columns.push_back(
          alias.empty() ? t.columns[static_cast<std::size_t>(idx)].name : alias);
    }
  }
  for (auto& g : groups) {
    std::vector<Value> out;
    out.reserve(items.size());
    for (auto& it : items) {
      if (it.is_agg) {
        out.push_back(computeAggregate(t, g.rows, s.aggregates[it.index]));
      } else {
        out.push_back(g.key[colPos[it.index]]);
      }
    }
    r.rows.push_back(std::move(out));
  }
  // ORDER BY nach Aggregation (Default: First-Seen), dann LIMIT/OFFSET.
  if (!s.order_by.empty()) sortGroupedRows(t, groups, s, r);
  applyLimitOffset(r, s);
  return r;
}

// ---------- INNER JOIN ----------
// (JoinCtx, resolveJoinCol, evalJoin*, execJoinRows, sortJoin*,
// execJoinScalarAggregates, execJoinGroupedAggregates, execJoinSelect;
// Details siehe Fortsetzung unten.)

// Kombinierter Auswertungskontext: Combined-Rows sind konkateniert
// [links..., rechts...]; WHERE/GROUP/ORDER/Aggregate arbeiten darauf wie
// bisher auf Single-Table-Rows (WHERE/GROUP/ORDER-Semantik unveraendert).
struct JoinCtx {
  const Table* left = nullptr;
  const Table* right = nullptr;
  std::string lTable;  // Originalnamen (Fehlermeldungen)
  std::string rTable;
  std::string lEff;  // effektiver Qualifizierer (Alias oder Tabelle)
  std::string rEff;
  std::size_t nL = 0;  // Spaltenzahl links
};

void splitColRef(const std::string& ref, std::string& prefix,
                 std::string& col) {
  std::string::size_type p = ref.find('.');
  if (p == std::string::npos) {
    prefix.clear();
    col = ref;
  } else {
    prefix = ref.substr(0, p);
    col = ref.substr(p + 1);
  }
}

// Spaltenreferenz -> Combined-Index. Qualifizierer darf Tabellenname oder
// Alias je Seite sein; unqualifiziert + beidseitig vorhanden -> ambiguous.
int resolveJoinCol(const JoinCtx& j, const std::string& ref) {
  std::string pre, col;
  splitColRef(ref, pre, col);
  if (!pre.empty()) {
    bool lm = (foldIdent(pre) == foldIdent(j.lEff)) ||
              (foldIdent(pre) == foldIdent(j.lTable));
    bool rm = (foldIdent(pre) == foldIdent(j.rEff)) ||
              (foldIdent(pre) == foldIdent(j.rTable));
    if (lm && rm)
      throw SqlError("Mehrdeutiger Tabellen-Prefix: " + pre +
                     " (ambiguous, Alias verwenden)");
    if (lm) {
      int idx = j.left->colIndex(col);
      if (idx < 0) throw SqlError("Unbekannte Spalte: " + ref);
      return idx;
    }
    if (rm) {
      int idx = j.right->colIndex(col);
      if (idx < 0) throw SqlError("Unbekannte Spalte: " + ref);
      return static_cast<int>(j.nL + static_cast<std::size_t>(idx));
    }
    throw SqlError("Unbekannter Tabellen-Prefix: " + pre);
  }
  int li = j.left->colIndex(col);
  int ri = j.right->colIndex(col);
  if (li >= 0 && ri >= 0)
    throw SqlError("Mehrdeutige Spalte: " + col +
                   " (ambiguous, Tabellen-Prefix t.c angeben)");
  if (li >= 0) return li;
  if (ri >= 0)
    return static_cast<int>(j.nL + static_cast<std::size_t>(ri));
  throw SqlError("Unbekannte Spalte: " + ref);
}

// Original-Spaltenname am Combined-Index (Ausgabe ohne Qualifizierer, PG).
const std::string& joinColName(const JoinCtx& j, int idx) {
  std::size_t i = static_cast<std::size_t>(idx);
  if (i < j.nL) return j.left->columns[i].name;
  return j.right->columns[i - j.nL].name;
}

// ON-Bedingung auf einer Combined-Row (AND-Kette ausserhalb). NULL auf
// einer Seite -> UNKNOWN -> false (Inner-Join droppt, wie WHERE).
bool evalJoinOnCond(const JoinCtx& j, const std::vector<Value>& crow,
                    const JoinCond& c) {
  int li = resolveJoinCol(j, c.left);
  const Value& lv = crow[static_cast<std::size_t>(li)];
  Value rv;
  if (c.right_is_col) {
    int ri = resolveJoinCol(j, c.right);
    rv = crow[static_cast<std::size_t>(ri)];
  } else {
    rv = c.literal;
  }
  if (valueIsNull(lv) || valueIsNull(rv)) return false;
  int cmp = compareValues(lv, rv);
  if (cmp == -2) return false;
  if (c.op == "=") return cmp == 0;
  if (c.op == "<>") return cmp != 0;
  if (c.op == "<") return cmp < 0;
  if (c.op == "<=") return cmp <= 0;
  if (c.op == ">") return cmp > 0;
  if (c.op == ">=") return cmp >= 0;
  throw SqlError("Unbekannter Operator in JOIN ... ON: " + c.op);
}

// WHERE-Condition auf Combined-Row (Koerper wie evalCondition, nur
// Aufloesung join-bewusst).
bool evalJoinCondition(const JoinCtx& j, const std::vector<Value>& crow,
                       const Condition& c) {
  if (c.subquery)
    throw SqlError("Subquery ohne Ausfuehrungskontext (interner Fehler)");
  int idx = resolveJoinCol(j, c.column);
  const Value& v = crow[static_cast<std::size_t>(idx)];
  if (c.op == "IS NULL") return valueIsNull(v);
  if (c.op == "IS NOT NULL") return !valueIsNull(v);
  if (c.op == "IN" || c.op == "NOT IN") {
    if (valueIsNull(v)) return false;
    bool has_null = false;
    for (auto& e : c.list) {
      if (valueIsNull(e)) {
        has_null = true;
        continue;
      }
      if (compareValues(v, e) == 0) return (c.op == "IN");
    }
    if (c.op == "IN") return false;
    return !has_null;
  }
  if (valueIsNull(v) || valueIsNull(c.value)) return false;  // NULL -> false
  if (c.op == "BETWEEN" || c.op == "NOT BETWEEN") {
    if (valueIsNull(c.second)) return false;
    int lo = compareValues(v, c.value);
    int hi = compareValues(v, c.second);
    if (lo == -2 || hi == -2) return false;
    bool in = (lo >= 0 && hi <= 0);
    return (c.op == "BETWEEN") ? in : !in;
  }
  if (c.op == "LIKE" || c.op == "ILIKE" || c.op == "NOT LIKE" ||
      c.op == "NOT ILIKE") {
    auto* vs = std::get_if<std::string>(&v);
    auto* ps = std::get_if<std::string>(&c.value);
    if (!vs || !ps)
      throw SqlError(c.op + " braucht TEXT-Operanden");
    bool m = likeMatch(*vs, *ps, c.op == "ILIKE" || c.op == "NOT ILIKE");
    return (c.op == "LIKE" || c.op == "ILIKE") ? m : !m;
  }
  int cmp = compareValues(v, c.value);
  if (cmp == -2) return false;
  if (c.op == "=") return cmp == 0;
  if (c.op == "<>") return cmp != 0;
  if (c.op == "<") return cmp < 0;
  if (c.op == "<=") return cmp <= 0;
  if (c.op == ">") return cmp > 0;
  if (c.op == ">=") return cmp >= 0;
  throw SqlError("Unbekannter Operator: " + c.op);
}

bool evalJoinWhere(const JoinCtx& j, const std::vector<Value>& crow,
                   const SelectStmt& s) {
  if (!s.where_groups.empty()) {
    for (auto& conj : s.where_groups) {
      bool ok = true;
      for (auto& c : conj) {
        if (!evalJoinCondition(j, crow, c)) {
          ok = false;
          break;
        }
      }
      if (ok) return true;
    }
    return false;
  }
  for (auto& c : s.where)
    if (!evalJoinCondition(j, crow, c)) return false;
  return true;
}

// Aggregat-Argument auf Combined-Row (Koerper wie evalAggExprNode).
Value evalJoinAggNode(const JoinCtx& j, const std::vector<Value>& crow,
                      const AggExpr& e) {
  switch (e.kind) {
    case AggExpr::Kind::Column: {
      int idx = resolveJoinCol(j, e.column);
      return crow[static_cast<std::size_t>(idx)];
    }
    case AggExpr::Kind::Literal:
      return e.literal;
    case AggExpr::Kind::Binary: {
      Value lv = evalJoinAggNode(j, crow, *e.left);
      Value rv = evalJoinAggNode(j, crow, *e.right);
      if (valueIsNull(lv) || valueIsNull(rv))
        return Value{std::monostate{}};
      if (std::holds_alternative<std::string>(lv) ||
          std::holds_alternative<std::string>(rv) ||
          std::holds_alternative<bool>(lv) ||
          std::holds_alternative<bool>(rv))
        throw SqlError("Aggregat-Ausdruck braucht numerische Operanden");
      double a = aggToDouble(lv);
      double b = aggToDouble(rv);
      switch (e.op) {
        case '+':
          return Value{a + b};
        case '-':
          return Value{a - b};
        case '*':
          return Value{a * b};
        case '/':
          if (b == 0.0) return Value{std::monostate{}};
          return Value{a / b};
        default:
          break;
      }
      throw SqlError("Unbekannter Operator in Aggregat");
    }
  }
  throw SqlError("Ungueltiger Aggregat-Ausdruck");
}

// Ein Aggregat ueber Combined-Rows (Semantik wie computeAggregate).
Value computeJoinAggregate(
    const JoinCtx& j, const std::vector<const std::vector<Value>*>& rows,
    const Aggregate& a) {
  if (a.star) {  // COUNT(*)
    return Value{static_cast<int64_t>(rows.size())};
  }
  if (!a.arg) throw SqlError("Aggregat ohne Argument: " + a.func);
  if (a.func == "COUNT") {
    int64_t c = 0;
    for (auto rp : rows) {
      Value v = evalJoinAggNode(j, *rp, *a.arg);
      if (!valueIsNull(v)) ++c;
    }
    return Value{c};
  }
  if (a.func == "SUM") {
    bool any = false;
    double sum = 0.0;
    for (auto rp : rows) {
      Value v = evalJoinAggNode(j, *rp, *a.arg);
      if (valueIsNull(v)) continue;
      requireNumericForSumAvg(v, "SUM");
      sum += aggToDouble(v);
      any = true;
    }
    return any ? Value{sum} : Value{std::monostate{}};
  }
  if (a.func == "AVG") {
    double sum = 0.0;
    int64_t n = 0;
    for (auto rp : rows) {
      Value v = evalJoinAggNode(j, *rp, *a.arg);
      if (valueIsNull(v)) continue;
      requireNumericForSumAvg(v, "AVG");
      sum += aggToDouble(v);
      ++n;
    }
    return n > 0 ? Value{sum / static_cast<double>(n)}
                 : Value{std::monostate{}};
  }
  if (a.func == "MIN" || a.func == "MAX") {
    bool any = false;
    Value best{std::monostate{}};
    for (auto rp : rows) {
      Value v = evalJoinAggNode(j, *rp, *a.arg);
      if (valueIsNull(v)) continue;
      if (!any) {
        best = v;
        any = true;
      } else {
        int cmp = compareValues(v, best);
        if (a.func == "MIN" ? (cmp < 0) : (cmp > 0)) best = v;
      }
    }
    return any ? best : Value{std::monostate{}};
  }
  throw SqlError("Unbekannte Aggregatfunktion: " + a.func);
}

// ---------- Unkorrelierte Subqueries (IN + Skalar) ----------
// Design: WHERE-Conditions tragen optional ein shared_ptr<SelectStmt>.
// Ausfuehrung in Database::execSelect/Update/Delete: Subquery EINMAL
// ausfuehren (IN -> Wertemenge, Skalar -> Einzelwert), dann Membership bzw.
// Vergleich pro Zeile. Korrelierte Refs -> expliziter SqlError, Tiefe max 8.
// Scope-Tracking via thread-lokalen Stack (keine Signaturaenderung der
// bestehenden Eval-Funktionen).

struct ScopeTable {
  std::unordered_set<std::string> quals;  // lower: Tabellenname + Alias
  const Table* table = nullptr;           // null wenn Tabelle (noch) unbekannt
};
struct Scope {
  std::vector<ScopeTable> tables;  // 1 Eintrag single, 2 bei JOIN
};
thread_local std::vector<Scope> g_scopes;

struct SubRes {
  bool is_in = false;  // true = IN/NOT IN-Menge, false = Skalarwert
  std::vector<Value> set;
  Value scalar{std::monostate{}};
};
using SubMap = std::map<const SelectStmt*, SubRes>;

struct ScopeGuard {
  ~ScopeGuard() {
    if (!g_scopes.empty()) g_scopes.pop_back();
  }
};

void collectAggRefs(const std::shared_ptr<AggExpr>& e,
                    std::vector<std::string>& out) {
  if (!e) return;
  if (e->kind == AggExpr::Kind::Column) {
    out.push_back(e->column);
    return;
  }
  if (e->kind == AggExpr::Kind::Binary) {
    collectAggRefs(e->left, out);
    collectAggRefs(e->right, out);
  }
}

// Direkte Spaltenrefs eines SELECTs (ohne den Inhalt genesteter Subqueries;
// deren Refs werden bei deren eigener Ausfuehrung mit erweitertem Stack
// geprueft).
void collectDirectRefs(const SelectStmt& s, std::vector<std::string>& out) {
  for (auto& c : s.columns) out.push_back(c);
  for (auto& g : s.group_by) out.push_back(g);
  for (auto& a : s.aggregates) collectAggRefs(a.arg, out);
  for (auto& c : s.where) out.push_back(c.column);
  for (auto& gr : s.where_groups)
    for (auto& c : gr) out.push_back(c.column);
  for (auto& jc : s.join_on) {
    out.push_back(jc.left);
    if (jc.right_is_col) out.push_back(jc.right);
  }
  for (auto& o : s.order_by) {
    if (o.is_agg)
      collectAggRefs(o.agg.arg, out);
    else if (!o.is_ordinal)
      out.push_back(o.column);
  }
}

bool resolvesInner(const Scope& inner, const std::string& ref) {
  auto pos = ref.find('.');
  if (pos != std::string::npos) {
    std::string pre = foldIdent(ref.substr(0, pos));
    for (auto& t : inner.tables)
      if (t.quals.count(pre)) return true;
    return false;
  }
  for (auto& t : inner.tables) {
    if (t.table && t.table->colIndex(ref) >= 0) return true;
  }
  return false;
}

bool matchesOuter(const Scope& outer, const std::string& ref) {
  auto pos = ref.find('.');
  if (pos != std::string::npos) {
    std::string pre = foldIdent(ref.substr(0, pos));
    for (auto& t : outer.tables)
      if (t.quals.count(pre)) return true;
    return false;
  }
  for (auto& t : outer.tables) {
    if (t.table && t.table->colIndex(ref) >= 0) return true;
  }
  return false;
}

// Eigene Refs gegen aeussere Scopes pruefen (korreliert -> expliziter Fehler).
// Aufruf im Kind-Kontext: g_scopes enthaelt bereits alle aeusseren Queries.
void validateNotCorrelated(const SelectStmt& sub, const Scope& inner) {
  std::vector<std::string> refs;
  collectDirectRefs(sub, refs);
  for (auto& r : refs) {
    if (resolvesInner(inner, r)) continue;
    for (auto it = g_scopes.rbegin(); it != g_scopes.rend(); ++it) {
      if (matchesOuter(*it, r))
        throw SqlError(
            "Korrelierte Subquery wird nicht unterstuetzt (Referenz: " + r +
            ")");
    }
  }
}

Scope buildScopeFor(const Database& db, const SelectStmt& s) {
  Scope sc;
  auto lookup = [&](const std::string& t) -> const Table* {
    if (!db.hasTable(t)) return nullptr;
    try {
      return &db.getTable(t);
    } catch (...) {
      return nullptr;
    }
  };
  if (s.has_join) {
    ScopeTable l, r;
    l.quals.insert(foldIdent(s.table));
    if (!s.table_alias.empty()) l.quals.insert(foldIdent(s.table_alias));
    l.table = lookup(s.table);
    r.quals.insert(foldIdent(s.join_table));
    if (!s.join_alias.empty()) r.quals.insert(foldIdent(s.join_alias));
    r.table = lookup(s.join_table);
    sc.tables.push_back(std::move(l));
    sc.tables.push_back(std::move(r));
  } else {
    ScopeTable e;
    e.quals.insert(foldIdent(s.table));
    if (!s.table_alias.empty()) e.quals.insert(foldIdent(s.table_alias));
    e.table = lookup(s.table);
    sc.tables.push_back(std::move(e));
  }
  return sc;
}

bool evalScalarCmp(const Value& v, const std::string& op, const Value& sv) {
  if (valueIsNull(v) || valueIsNull(sv)) return false;
  int cmp = compareValues(v, sv);
  if (cmp == -2) return false;
  if (op == "=") return cmp == 0;
  if (op == "<>") return cmp != 0;
  if (op == "<") return cmp < 0;
  if (op == "<=") return cmp <= 0;
  if (op == ">") return cmp > 0;
  if (op == ">=") return cmp >= 0;
  throw SqlError("Unbekannter Operator vor Subquery: " + op);
}

// Single-Table Condition mit Subquery-Kontext (ohne Subquery -> Altpfad).
bool evalConditionSub(const Table& t, const std::vector<Value>& row,
                      const Condition& c, const SubMap& m) {
  if (!c.subquery) return evalCondition(t, row, c);
  auto it = m.find(c.subquery.get());
  if (it == m.end()) throw SqlError("Subquery nicht aufgeloest");
  const SubRes& sr = it->second;
  int idx = t.colIndex(c.column);
  if (idx < 0) throw SqlError("Unbekannte Spalte in WHERE: " + c.column);
  const Value& v = row[static_cast<std::size_t>(idx)];
  if (sr.is_in) {
    bool is_not = (c.op == "NOT IN");
    if (valueIsNull(v)) return false;
    bool has_null = false;
    for (auto& e : sr.set) {
      if (valueIsNull(e)) {
        has_null = true;
        continue;
      }
      if (compareValues(v, e) == 0) return !is_not;
    }
    if (!is_not) return false;
    return !has_null;
  }
  return evalScalarCmp(v, c.op, sr.scalar);
}

bool evalWhereSub(const Table& t, const std::vector<Value>& row,
                  const std::vector<Condition>& where,
                  const std::vector<std::vector<Condition>>& groups,
                  const SubMap& m) {
  if (!groups.empty()) {
    for (auto& conj : groups) {
      bool ok = true;
      for (auto& c : conj) {
        if (!evalConditionSub(t, row, c, m)) {
          ok = false;
          break;
        }
      }
      if (ok) return true;
    }
    return false;
  }
  for (auto& c : where)
    if (!evalConditionSub(t, row, c, m)) return false;
  return true;
}

bool evalJoinConditionSub(const JoinCtx& j, const std::vector<Value>& crow,
                          const Condition& c, const SubMap& m) {
  if (!c.subquery) return evalJoinCondition(j, crow, c);
  auto it = m.find(c.subquery.get());
  if (it == m.end()) throw SqlError("Subquery nicht aufgeloest");
  const SubRes& sr = it->second;
  int idx = resolveJoinCol(j, c.column);
  const Value& v = crow[static_cast<std::size_t>(idx)];
  if (sr.is_in) {
    bool is_not = (c.op == "NOT IN");
    if (valueIsNull(v)) return false;
    bool has_null = false;
    for (auto& e : sr.set) {
      if (valueIsNull(e)) {
        has_null = true;
        continue;
      }
      if (compareValues(v, e) == 0) return !is_not;
    }
    if (!is_not) return false;
    return !has_null;
  }
  return evalScalarCmp(v, c.op, sr.scalar);
}

bool evalJoinWhereSub(const JoinCtx& j, const std::vector<Value>& crow,
                      const SelectStmt& s, const SubMap& m) {
  if (!s.where_groups.empty()) {
    for (auto& conj : s.where_groups) {
      bool ok = true;
      for (auto& c : conj) {
        if (!evalJoinConditionSub(j, crow, c, m)) {
          ok = false;
          break;
        }
      }
      if (ok) return true;
    }
    return false;
  }
  for (auto& c : s.where)
    if (!evalJoinConditionSub(j, crow, c, m)) return false;
  return true;
}

// Hashbar? Nur reine Equi-Kette ("=" Spalte-zu-Spalte, je eine Seite).
// Loest dabei alle Refs auf (unbekannt/ambig -> SqlError) und liefert die
// seiten-lokalen Key-Indizes.
bool joinIsHashable(const JoinCtx& j, const SelectStmt& s,
                    std::vector<int>& lKeys, std::vector<int>& rKeys) {
  lKeys.clear();
  rKeys.clear();
  if (s.join_on.empty()) return false;
  for (auto& c : s.join_on) {
    if (c.op != "=" || !c.right_is_col) return false;
    int li = resolveJoinCol(j, c.left);
    int ri = resolveJoinCol(j, c.right);
    bool lInL = static_cast<std::size_t>(li) < j.nL;
    bool rInL = static_cast<std::size_t>(ri) < j.nL;
    if (lInL == rInL) return false;  // gleiche Seite -> kein Join-Key
    int lLocal = lInL ? li : ri;
    int rLocal = (lInL ? ri : li) - static_cast<int>(j.nL);
    lKeys.push_back(lLocal);
    rKeys.push_back(rLocal);
  }
  return true;
}

// Combined-Rows ([links..., rechts...]) in Nested-Loop-Ordnung (links-major):
// Hash-Join bei reiner Equi-ON-Kette (kleinere Seite builden, NULL-Keys
// matchen nie), sonst Nested-Loop mit voller AND-Auswertung.
std::vector<std::vector<Value>> execJoinRows(const JoinCtx& j,
                                             const SelectStmt& s) {
  const auto& L = j.left->rows;
  const auto& R = j.right->rows;
  const std::size_t nR = j.right->columns.size();
  std::vector<std::vector<Value>> out;
  std::vector<int> lKeys, rKeys;
  if (joinIsHashable(j, s, lKeys, rKeys)) {
    auto keyOf = [&](const std::vector<Value>& row, std::size_t off,
                     const std::vector<int>& keys, std::string& k) -> bool {
      k.clear();
      for (int ki : keys) {
        const Value& v = row[off + static_cast<std::size_t>(ki)];
        if (valueIsNull(v)) return false;  // NULL-Key matcht nie
        k += groupKeyField(v);
      }
      return true;
    };
    struct Pair {
      std::size_t li = 0;
      std::size_t ri = 0;
    };
    std::vector<Pair> pairs;
    if (L.size() <= R.size()) {
      std::unordered_map<std::string, std::vector<std::size_t>> ht;
      for (std::size_t i = 0; i < L.size(); ++i) {
        std::string k;
        if (!keyOf(L[i], 0, lKeys, k)) continue;
        ht[k].push_back(i);
      }
      for (std::size_t ri = 0; ri < R.size(); ++ri) {
        std::string k;
        if (!keyOf(R[ri], 0, rKeys, k)) continue;
        auto it = ht.find(k);
        if (it == ht.end()) continue;
        for (auto li : it->second) pairs.push_back(Pair{li, ri});
      }
      // Build-Seite war links (Probe rechts-major) -> links-major
      // wiederherstellen.
      std::stable_sort(pairs.begin(), pairs.end(),
                       [](const Pair& a, const Pair& b) {
                         if (a.li != b.li) return a.li < b.li;
                         return a.ri < b.ri;
                       });
    } else {
      std::unordered_map<std::string, std::vector<std::size_t>> ht;
      for (std::size_t i = 0; i < R.size(); ++i) {
        std::string k;
        if (!keyOf(R[i], 0, rKeys, k)) continue;
        ht[k].push_back(i);
      }
      for (std::size_t li = 0; li < L.size(); ++li) {
        std::string k;
        if (!keyOf(L[li], 0, lKeys, k)) continue;
        auto it = ht.find(k);
        if (it == ht.end()) continue;
        for (auto ri : it->second) pairs.push_back(Pair{li, ri});
      }
      // Probe war links -> bereits links-major.
    }
    out.reserve(pairs.size());
    for (auto& p : pairs) {
      std::vector<Value> c;
      c.reserve(j.nL + nR);
      c.insert(c.end(), L[p.li].begin(), L[p.li].end());
      c.insert(c.end(), R[p.ri].begin(), R[p.ri].end());
      out.push_back(std::move(c));
    }
    return out;
  }
  for (std::size_t li = 0; li < L.size(); ++li) {
    for (std::size_t ri = 0; ri < R.size(); ++ri) {
      std::vector<Value> c;
      c.reserve(j.nL + nR);
      c.insert(c.end(), L[li].begin(), L[li].end());
      c.insert(c.end(), R[ri].begin(), R[ri].end());
      bool ok = true;
      for (auto& cd : s.join_on)
        if (!evalJoinOnCond(j, c, cd)) {
          ok = false;
          break;
        }
      if (ok) out.push_back(std::move(c));
    }
  }
  return out;
}

// ORDER BY auf ungefiltert-projizierten Plain-Combined-Rows (vor Projektion):
// Aufloesung je Item wie sortPlainRows (Ausgabe-Alias/Spalte zuerst, dann
// Combined-Spalte; Dubletten unqualifiziert -> ambiguous). Stabil.
void sortJoinPlainRows(const JoinCtx& j,
                       std::vector<std::vector<Value>>& kept,
                       const SelectStmt& s, const std::vector<int>& projIdx,
                       const std::vector<std::string>& outNames) {
  struct Key {
    int col = -1;
    bool desc = false;
    bool nulls_first = false;
  };
  auto resolveName = [&](const std::string& name) -> int {
    int found = -1;
    bool amb = false;
    for (std::size_t i = 0; i < outNames.size(); ++i) {
      if (foldIdent(outNames[i]) == foldIdent(name)) {
        int ci = projIdx[i];
        if (found < 0)
          found = ci;
        else if (found != ci) {
          amb = true;
          break;
        }
      }
    }
    if (amb)
      throw SqlError("Mehrdeutige Spalte in ORDER BY: " + name +
                     " (ambiguous)");
    if (found >= 0) return found;
    return resolveJoinCol(j, name);
  };
  auto resolveOrdinal = [&](int64_t ord) -> int {
    if (ord < 1 || static_cast<std::size_t>(ord) > projIdx.size())
      throw SqlError("ORDER BY-Position ausserhalb der Projektion");
    return projIdx[static_cast<std::size_t>(ord - 1)];
  };
  std::vector<Key> keys;
  keys.reserve(s.order_by.size());
  for (auto& o : s.order_by) {
    Key k;
    k.desc = o.desc;
    k.nulls_first = o.has_nulls ? o.nulls_first : o.desc;
    if (o.is_agg)
      throw SqlError(
          "ORDER BY mit Aggregat nur mit GROUP BY oder Aggregat-Projektion");
    else if (o.is_ordinal)
      k.col = resolveOrdinal(o.ordinal);
    else
      k.col = resolveName(o.column);
    keys.push_back(k);
  }
  std::stable_sort(kept.begin(), kept.end(),
                   [&](const std::vector<Value>& a,
                       const std::vector<Value>& b) {
                     for (auto& k : keys) {
                       int c = compareOrdered(
                           a[static_cast<std::size_t>(k.col)],
                           b[static_cast<std::size_t>(k.col)], k.desc,
                           k.nulls_first);
                       if (c != 0) return c < 0;
                     }
                     return false;
                   });
}

// ORDER BY auf gruppierten Join-Results (nach Aggregation): Aufloesung wie
// sortGroupedRows (Ausgabe, GROUP BY-Key per Combined-Index, berechnetes
// Aggregat). Unsortiert bleibt First-Seen-Reihenfolge. Stabil.
void sortJoinGroupedRows(const JoinCtx& j, const std::vector<Group>& groups,
                         const std::vector<int>& gidx, const SelectStmt& s,
                         Result& r) {
  std::vector<GroupOrderKey> keys;
  keys.reserve(s.order_by.size());
  for (auto& o : s.order_by) {
    GroupOrderKey k;
    k.desc = o.desc;
    k.nulls_first = o.has_nulls ? o.nulls_first : o.desc;
    if (o.is_ordinal) {
      if (o.ordinal < 1 ||
          static_cast<std::size_t>(o.ordinal) > r.columns.size())
        throw SqlError("ORDER BY-Position ausserhalb der Projektion");
      k.kind = GroupOrderKey::Kind::Result;
      k.pos = static_cast<std::size_t>(o.ordinal - 1);
    } else if (o.is_agg) {
      int rpos = -1;
      for (std::size_t i = 0; i < r.columns.size(); ++i)
        if (foldIdent(r.columns[i]) == foldIdent(o.agg.display)) {
          rpos = static_cast<int>(i);
          break;
        }
      if (rpos >= 0) {
        k.kind = GroupOrderKey::Kind::Result;
        k.pos = static_cast<std::size_t>(rpos);
      } else {
        k.kind = GroupOrderKey::Kind::Computed;
        k.agg = o.agg;
      }
    } else {
      int rpos = -1;
      for (std::size_t i = 0; i < r.columns.size(); ++i)
        if (foldIdent(r.columns[i]) == foldIdent(o.column)) {
          rpos = static_cast<int>(i);
          break;
        }
      if (rpos >= 0) {
        k.kind = GroupOrderKey::Kind::Result;
        k.pos = static_cast<std::size_t>(rpos);
      } else {
        int ci = resolveJoinCol(j, o.column);
        int gpos = -1;
        for (std::size_t i = 0; i < gidx.size(); ++i)
          if (gidx[i] == ci) {
            gpos = static_cast<int>(i);
            break;
          }
        if (gpos < 0)
          throw SqlError("Unbekannte Spalte in ORDER BY: " + o.column);
        k.kind = GroupOrderKey::Kind::GroupKey;
        k.pos = static_cast<std::size_t>(gpos);
      }
    }
    keys.push_back(std::move(k));
  }
  std::size_t n = r.rows.size();
  std::vector<std::vector<Value>> kvals(n);
  for (std::size_t i = 0; i < n; ++i) {
    kvals[i].reserve(keys.size());
    for (auto& k : keys) {
      if (k.kind == GroupOrderKey::Kind::Result)
        kvals[i].push_back(r.rows[i][k.pos]);
      else if (k.kind == GroupOrderKey::Kind::GroupKey)
        kvals[i].push_back(groups[i].key[k.pos]);
      else
        kvals[i].push_back(computeJoinAggregate(j, groups[i].rows, k.agg));
    }
  }
  std::vector<std::size_t> idx(n);
  for (std::size_t i = 0; i < n; ++i) idx[i] = i;
  std::stable_sort(idx.begin(), idx.end(),
                   [&](std::size_t a, std::size_t b) {
                     for (std::size_t m = 0; m < keys.size(); ++m) {
                       int c = compareOrdered(kvals[a][m], kvals[b][m],
                                              keys[m].desc,
                                              keys[m].nulls_first);
                       if (c != 0) return c < 0;
                     }
                     return false;
                   });
  std::vector<std::vector<Value>> sorted;
  sorted.reserve(n);
  for (auto i : idx) sorted.push_back(std::move(r.rows[i]));
  r.rows = std::move(sorted);
}

// ORDER BY-Validierung auf 1-zeiligen Join-Aggregat-Results (No-Op wie
// validateOrderScalar, join-bewusst).
void validateOrderJoinScalar(
    const JoinCtx& j, const std::vector<std::vector<Value>>& kept,
    const SelectStmt& s, const Result& r) {
  for (auto& o : s.order_by) {
    if (o.is_ordinal) {
      if (o.ordinal < 1 ||
          static_cast<std::size_t>(o.ordinal) > r.columns.size())
        throw SqlError("ORDER BY-Position ausserhalb der Projektion");
    } else if (o.is_agg) {
      bool known = false;
      for (auto& a : s.aggregates)
        if (a.display == o.agg.display && a.func == o.agg.func) {
          known = true;
          break;
        }
      if (!known) {
        std::vector<const std::vector<Value>*> refs;
        refs.reserve(kept.size());
        for (auto& row : kept) refs.push_back(&row);
        (void)computeJoinAggregate(j, refs, o.agg);
      }
    } else {
      bool ok = false;
      for (auto& c : r.columns)
        if (foldIdent(c) == foldIdent(o.column)) {
          ok = true;
          break;
        }
      if (!ok) throw SqlError("Unbekannte Spalte in ORDER BY: " + o.column);
    }
  }
}

// Ein-Zeilen-Result ueber Join (Semantik wie execScalarAggregates).
Result execJoinScalarAggregates(
    const JoinCtx& j, const std::vector<std::vector<Value>>& kept,
    const SelectStmt& s) {
  Result r;
  r.columns.reserve(s.aggregates.size());
  for (auto& a : s.aggregates)
    r.columns.push_back(a.alias.empty() ? a.display : a.alias);
  std::vector<const std::vector<Value>*> refs;
  refs.reserve(kept.size());
  for (auto& row : kept) refs.push_back(&row);
  std::vector<Value> out;
  out.reserve(s.aggregates.size());
  for (auto& a : s.aggregates)
    out.push_back(computeJoinAggregate(j, refs, a));
  r.rows.push_back(std::move(out));
  validateOrderJoinScalar(j, kept, s, r);
  applyLimitOffset(r, s);
  return r;
}

// Hash-Aggregation ueber Join (Semantik wie execGroupedAggregates:
// First-Seen, leere Eingabe -> 0 Zeilen, Plain-Spalten muessen gruppiert
// sein; Vergleich per Combined-Index, daher ist "id" GROUP BY "a.id"
// dieselbe Spalte).
Result execJoinGroupedAggregates(
    const JoinCtx& j, const std::vector<std::vector<Value>>& kept,
    const SelectStmt& s) {
  if (s.select_all || s.count_star)
    throw SqlError("SELECT * / COUNT(*) mit GROUP BY wird nicht unterstuetzt");
  std::vector<int> gidx;
  gidx.reserve(s.group_by.size());
  for (auto& g : s.group_by) gidx.push_back(resolveJoinCol(j, g));
  for (auto& c : s.columns) {
    int ci = resolveJoinCol(j, c);
    bool ok = false;
    for (int gi : gidx)
      if (gi == ci) {
        ok = true;
        break;
      }
    if (!ok)
      throw SqlError("Spalte '" + c + "' muss in GROUP BY erscheinen");
  }
  std::vector<Group> groups;
  std::unordered_map<std::string, std::size_t> pos;
  for (auto& row : kept) {
    std::string k;
    std::vector<Value> kv;
    kv.reserve(gidx.size());
    for (int gi : gidx) {
      const Value& v = row[static_cast<std::size_t>(gi)];
      kv.push_back(v);
      k += groupKeyField(v);
    }
    auto it = pos.find(k);
    if (it == pos.end()) {
      std::size_t id = groups.size();
      pos.emplace(k, id);
      Group g;
      g.key = std::move(kv);
      g.rows.push_back(&row);
      groups.push_back(std::move(g));
    } else {
      groups[it->second].rows.push_back(&row);
    }
  }
  std::vector<std::size_t> colPos;
  colPos.reserve(s.columns.size());
  for (auto& c : s.columns) {
    int ci = resolveJoinCol(j, c);
    std::size_t p = 0;
    for (; p < gidx.size(); ++p)
      if (gidx[p] == ci) break;
    colPos.push_back(p);
  }
  std::vector<SelectItem> items = s.items;
  if (items.empty()) {
    for (std::size_t i = 0; i < s.columns.size(); ++i)
      items.push_back(SelectItem{false, i});
    for (std::size_t i = 0; i < s.aggregates.size(); ++i)
      items.push_back(SelectItem{true, i});
  }
  Result r;
  r.columns.reserve(items.size());
  for (auto& it : items) {
    if (it.is_agg) {
      if (it.index >= s.aggregates.size())
        throw SqlError("Ungueltige Projektion (Aggregat-Index)");
      const Aggregate& a = s.aggregates[it.index];
      r.columns.push_back(a.alias.empty() ? a.display : a.alias);
    } else {
      if (it.index >= s.columns.size())
        throw SqlError("Ungueltige Projektion (Spalten-Index)");
      int idx = resolveJoinCol(j, s.columns[it.index]);
      std::string alias;
      if (it.index < s.column_aliases.size()) alias = s.column_aliases[it.index];
      r.columns.push_back(alias.empty() ? joinColName(j, idx) : alias);
    }
  }
  for (auto& g : groups) {
    std::vector<Value> out;
    out.reserve(items.size());
    for (auto& it : items) {
      if (it.is_agg) {
        out.push_back(computeJoinAggregate(j, g.rows, s.aggregates[it.index]));
      } else {
        out.push_back(g.key[colPos[it.index]]);
      }
    }
    r.rows.push_back(std::move(out));
  }
  if (!s.order_by.empty()) sortJoinGroupedRows(j, groups, gidx, s, r);
  applyLimitOffset(r, s);
  return r;
}

// Join-Treiber: Join -> WHERE -> Gruppe/Aggregat/Plain -> ORDER/LIMIT.
// subs enthaelt einmalig aufgeloeste WHERE-Subqueries (leer = Altverhalten).
Result execJoinSelect(const JoinCtx& j, const SelectStmt& s,
                      const SubMap& subs = SubMap{}) {
  std::vector<std::vector<Value>> joined = execJoinRows(j, s);
  std::vector<std::vector<Value>> kept;
  kept.reserve(joined.size());
  for (auto& row : joined)
    if (subs.empty() ? evalJoinWhere(j, row, s)
                     : evalJoinWhereSub(j, row, s, subs))
      kept.push_back(row);
  if (!s.group_by.empty()) {
    return execJoinGroupedAggregates(j, kept, s);
  }
  if (s.count_star) {
    Result r{{"count"}, {{Value{static_cast<int64_t>(kept.size())}}},
             "SELECT 1", std::size_t{1}};
    validateOrderJoinScalar(j, kept, s, r);
    applyLimitOffset(r, s);
    return r;
  }
  if (!s.aggregates.empty()) {
    if (!s.columns.empty() || s.select_all)
      throw SqlError(
          "Ohne GROUP BY: Aggregate (SUM/AVG/MIN/MAX/COUNT) und Spalten nicht "
          "mischbar");
    return execJoinScalarAggregates(j, kept, s);
  }
  std::vector<int> idxs;
  std::vector<std::string> cols;
  if (s.select_all) {
    for (std::size_t i = 0; i < j.nL + j.right->columns.size(); ++i) {
      idxs.push_back(static_cast<int>(i));
      if (i < j.nL)
        cols.push_back(j.left->columns[i].name);
      else
        cols.push_back(j.right->columns[i - j.nL].name);
    }
  } else {
    for (std::size_t i = 0; i < s.columns.size(); ++i) {
      auto& c = s.columns[i];
      int idx = resolveJoinCol(j, c);
      idxs.push_back(idx);
      std::string alias;
      if (i < s.column_aliases.size()) alias = s.column_aliases[i];
      cols.push_back(alias.empty() ? joinColName(j, idx) : alias);
    }
  }
  Result r;
  r.columns = cols;
  if (!s.order_by.empty()) sortJoinPlainRows(j, kept, s, idxs, cols);
  for (auto& row : kept) {
    std::vector<Value> o;
    o.reserve(idxs.size());
    for (int i : idxs) o.push_back(row[static_cast<std::size_t>(i)]);
    r.rows.push_back(std::move(o));
  }
  applyLimitOffset(r, s);
  return r;
}

}  // namespace

// ---------- Oeffentliche API ----------

std::string colTypeToString(ColType t) {
  switch (t) {
    case ColType::Int: return "INT";
    case ColType::Text: return "TEXT";
    case ColType::Double: return "DOUBLE";
    case ColType::Bool: return "BOOL";
    case ColType::Jsonb: return "JSONB";
  }
  return "TEXT";
}

ColType colTypeFromString(const std::string& s) {
  std::string u = toUpper(s);
  if (u == "INT" || u == "INTEGER" || u == "BIGINT" || u == "SMALLINT" ||
      u == "SERIAL" || u == "BIGSERIAL")
    return ColType::Int;
  if (u == "TEXT" || u == "VARCHAR" || u == "CHAR" || u == "CHARACTER" ||
      u == "NAME" || u == "TIMESTAMP" || u == "DATE" || u == "TIME")
    return ColType::Text;
  if (u == "DOUBLE" || u == "FLOAT" || u == "REAL" || u == "NUMERIC" ||
      u == "DECIMAL")
    return ColType::Double;
  if (u == "BOOL" || u == "BOOLEAN") return ColType::Bool;
  if (u == "JSONB" || u == "JSON") return ColType::Jsonb;
  throw SqlError("Unbekannter Spaltentyp: " + s);
}

bool valueIsNull(const Value& v) {
  return std::holds_alternative<std::monostate>(v);
}

std::string valueToString(const Value& v) {
  if (std::holds_alternative<std::monostate>(v)) return "NULL";
  if (auto* i = std::get_if<int64_t>(&v)) return std::to_string(*i);
  if (auto* d = std::get_if<double>(&v)) {
    std::ostringstream o;
    o << *d;
    return o.str();
  }
  if (auto* s = std::get_if<std::string>(&v)) return *s;
  if (auto* b = std::get_if<bool>(&v)) return *b ? "TRUE" : "FALSE";
  return "?";
}

bool valueEquals(const Value& a, const Value& b) {
  if (valueIsNull(a) || valueIsNull(b))
    return valueIsNull(a) && valueIsNull(b);
  return compareValues(a, b) == 0;
}

int Table::colIndex(const std::string& name) const {
  std::string f = foldIdent(name);
  for (std::size_t i = 0; i < columns.size(); ++i)
    if (foldIdent(columns[i].name) == f) return (int)i;
  return -1;
}

Statement parseStatement(const std::string& sql) {
  Tokenizer tz(sql);
  auto toks = tz.run();
  Parser p(std::move(toks));
  return p.run();
}

std::string statementKind(const Statement& s) {
  if (std::holds_alternative<CreateTableStmt>(s)) return "CREATE";
  if (std::holds_alternative<InsertStmt>(s)) return "INSERT";
  if (std::holds_alternative<UpdateStmt>(s)) return "UPDATE";
  if (std::holds_alternative<DeleteStmt>(s)) return "DELETE";
  if (std::holds_alternative<DropTableStmt>(s)) return "DROP";
  if (std::holds_alternative<GrantStmt>(s)) return "GRANT";
  if (std::holds_alternative<RevokeStmt>(s)) return "REVOKE";
  if (std::holds_alternative<SetRoleStmt>(s)) return "SET";
  if (std::holds_alternative<CreatePolicyStmt>(s)) return "CREATE POLICY";
  if (std::holds_alternative<AlterTableRlsStmt>(s)) return "ALTER TABLE";
  return "SELECT";
}

bool Database::hasTable(const std::string& name) const {
  return tables_.count(foldIdent(name)) > 0;
}

const Table& Database::getTable(const std::string& name) const {
  auto it = tables_.find(foldIdent(name));
  if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + name);
  return it->second;
}

// ---------- RBAC (minimal) ----------

void Database::setRole(const std::string& role) { role_ = foldIdent(role); }

bool Database::hasPriv(const std::string& table,
                       const std::string& priv) const {
  if (role_.empty()) return true;  // Admin: alles erlaubt (Default wie bisher)
  auto tit = grants_.find(foldIdent(table));
  if (tit == grants_.end()) return false;
  auto rit = tit->second.find(role_);
  if (rit == tit->second.end()) return false;
  return rit->second.count(priv) > 0;
}

void Database::requirePriv(const std::string& table,
                           const std::string& priv) const {
  if (!hasPriv(table, priv))
    throw SqlError("permission denied for table " + table +
                   " (SQLSTATE 42501)");
}

void Database::requireAdmin(const std::string& what) const {
  if (!role_.empty())
    throw SqlError("permission denied (" + what +
                   " requires admin role, SQLSTATE 42501)");
}

Result Database::execGrant(const GrantStmt& s) {
  requireAdmin("GRANT");
  auto it = tables_.find(foldIdent(s.table));
  if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + s.table);
  auto& set = grants_[foldIdent(s.table)][s.role];
  for (auto& p : s.privs) set.insert(p);
  return {{}, {}, "GRANT", 0};
}

Result Database::execRevoke(const RevokeStmt& s) {
  requireAdmin("REVOKE");
  auto it = tables_.find(foldIdent(s.table));
  if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + s.table);
  auto tit = grants_.find(foldIdent(s.table));
  if (tit != grants_.end()) {
    auto rit = tit->second.find(s.role);
    if (rit != tit->second.end()) {
      for (auto& p : s.privs) rit->second.erase(p);
      if (rit->second.empty()) tit->second.erase(rit);
    }
    if (tit->second.empty()) grants_.erase(tit);
  }
  return {{}, {}, "REVOKE", 0};
}

Result Database::execSetRole(const SetRoleStmt& s) {
  // s.role kommt aus parseRole (unquoted bereits gefoldet, quoted exakt).
  role_ = s.reset ? "" : s.role;
  return {{}, {}, s.reset ? "RESET ROLE" : "SET ROLE", 0};
}

// ---------- RLS (Row-Level Security) ----------

bool Database::rlsEnabled(const std::string& normTable) const {
  return rls_on_.count(normTable) > 0;
}

bool Database::evalPolicyCond(const Table& t, const std::vector<Value>& row,
                              const Condition& c) const {
  if (c.subquery)
    throw SqlError("Subquery in POLICY USING wird nicht unterstuetzt");
  const Value roleVal{role_};
  // Spaltenlose Konstante: "current_user OP literal/Liste".
  if (c.lhs_is_current) {
    if (c.op == "IS NULL") return false;
    if (c.op == "IS NOT NULL") return true;
    if (c.op == "IN" || c.op == "NOT IN") {
      bool is_not = (c.op == "NOT IN");
      bool has_null = false;
      for (auto& e : c.list) {
        if (valueIsNull(e)) {
          has_null = true;
          continue;
        }
        if (compareValues(roleVal, e) == 0) return !is_not;
      }
      if (!is_not) return false;
      return !has_null;
    }
    if (c.op == "LIKE" || c.op == "ILIKE" || c.op == "NOT LIKE" ||
        c.op == "NOT ILIKE") {
      if (valueIsNull(c.value)) return false;
      auto* ps = std::get_if<std::string>(&c.value);
      if (!ps) throw SqlError(c.op + " braucht TEXT-Operanden");
      bool m = likeMatch(role_, *ps, c.op == "ILIKE" || c.op == "NOT ILIKE");
      return (c.op == "LIKE" || c.op == "ILIKE") ? m : !m;
    }
    if (valueIsNull(c.value)) return false;
    if (c.op == "BETWEEN" || c.op == "NOT BETWEEN") return false;
    int cmp = compareValues(roleVal, c.value);
    if (cmp == -2) return false;
    if (c.op == "=") return cmp == 0;
    if (c.op == "<>") return cmp != 0;
    if (c.op == "<") return cmp < 0;
    if (c.op == "<=") return cmp <= 0;
    if (c.op == ">") return cmp > 0;
    if (c.op == ">=") return cmp >= 0;
    throw SqlError("Unbekannter Operator: " + c.op);
  }
  int idx = t.colIndex(c.column);
  if (idx < 0) throw SqlError("Unbekannte Spalte in POLICY USING: " + c.column);
  const Value& v = row[static_cast<std::size_t>(idx)];
  if (c.op == "IS NULL") return valueIsNull(v);
  if (c.op == "IS NOT NULL") return !valueIsNull(v);
  if (c.op == "IN" || c.op == "NOT IN") {
    if (valueIsNull(v)) return false;
    bool has_null = false;
    for (std::size_t i = 0; i < c.list.size(); ++i) {
      Value e = (i < c.list_is_current.size() && c.list_is_current[i])
                    ? roleVal
                    : c.list[i];
      if (valueIsNull(e)) {
        has_null = true;
        continue;
      }
      if (compareValues(v, e) == 0) return (c.op == "IN");
    }
    if (c.op == "IN") return false;
    return !has_null;
  }
  Value rhs = c.value_is_current ? roleVal : c.value;
  Value rhs2 = c.second_is_current ? roleVal : c.second;
  if (valueIsNull(v) || valueIsNull(rhs)) return false;
  if (c.op == "BETWEEN" || c.op == "NOT BETWEEN") {
    if (valueIsNull(rhs2)) return false;
    int lo = compareValues(v, rhs);
    int hi = compareValues(v, rhs2);
    if (lo == -2 || hi == -2) return false;
    bool in = (lo >= 0 && hi <= 0);
    return (c.op == "BETWEEN") ? in : !in;
  }
  if (c.op == "LIKE" || c.op == "ILIKE" || c.op == "NOT LIKE" ||
      c.op == "NOT ILIKE") {
    auto* vs = std::get_if<std::string>(&v);
    auto* ps = std::get_if<std::string>(&rhs);
    if (!vs || !ps) throw SqlError(c.op + " braucht TEXT-Operanden");
    bool m = likeMatch(*vs, *ps, c.op == "ILIKE" || c.op == "NOT ILIKE");
    return (c.op == "LIKE" || c.op == "ILIKE") ? m : !m;
  }
  int cmp = compareValues(v, rhs);
  if (cmp == -2) return false;
  if (c.op == "=") return cmp == 0;
  if (c.op == "<>") return cmp != 0;
  if (c.op == "<") return cmp < 0;
  if (c.op == "<=") return cmp <= 0;
  if (c.op == ">") return cmp > 0;
  if (c.op == ">=") return cmp >= 0;
  throw SqlError("Unbekannter Operator: " + c.op);
}

bool Database::rowPassesRls(const Table& t, const std::vector<Value>& row,
                             const std::string& normTable,
                             const std::string& op) const {
  if (role_.empty()) return true;  // Owner/Admin bypassed immer
  if (!rlsEnabled(normTable)) return true;
  auto it = policies_.find(normTable);
  if (it == policies_.end()) return false;  // enabled, keine Policy -> deny
  bool anyApplicable = false;
  for (auto& p : it->second) {
    if (p.command != "ALL" && p.command != op) continue;
    if (p.role != "*" && p.role != role_) continue;
    anyApplicable = true;
    bool pass;
    if (!p.where_groups.empty()) {
      pass = false;
      for (auto& conj : p.where_groups) {
        bool ok = true;
        for (auto& cc : conj) {
          if (!evalPolicyCond(t, row, cc)) {
            ok = false;
            break;
          }
        }
        if (ok) {
          pass = true;
          break;
        }
      }
    } else {
      pass = true;
      for (auto& cc : p.where) {
        if (!evalPolicyCond(t, row, cc)) {
          pass = false;
          break;
        }
      }
    }
    if (pass) return true;  // Policies per OR
  }
  (void)anyApplicable;
  if (!anyApplicable) return false;  // keine Policy fuer Rolle+Op -> deny
  return false;
}

Result Database::execCreatePolicy(const CreatePolicyStmt& s) {
  requireAdmin("CREATE POLICY");
  auto it = tables_.find(foldIdent(s.table));
  if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + s.table);
  const std::string norm = foldIdent(s.table);
  auto& vec = policies_[norm];
  for (auto& p : vec)
    if (foldIdent(p.policy) == foldIdent(s.policy))
      throw SqlError("Policy existiert bereits: " + s.policy);
  // USING-Spalten frueh validieren (unbekannt -> SqlError, keine Halb-Registry).
  const Table& t = it->second;
  auto check = [&](const Condition& c) {
    if (c.subquery)
      throw SqlError("Subquery in POLICY USING wird nicht unterstuetzt");
    if (!c.lhs_is_current) {
      if (t.colIndex(c.column) < 0)
        throw SqlError("Unbekannte Spalte in POLICY USING: " + c.column);
    }
  };
  for (auto& c : s.where) check(c);
  for (auto& gr : s.where_groups)
    for (auto& c : gr) check(c);
  vec.push_back(s);
  return {{}, {}, "CREATE POLICY", 0};
}

Result Database::execAlterRls(const AlterTableRlsStmt& s) {
  requireAdmin("ALTER TABLE ... ROW LEVEL SECURITY");
  auto it = tables_.find(foldIdent(s.table));
  if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + s.table);
  const std::string norm = foldIdent(s.table);
  if (s.enable)
    rls_on_.insert(norm);
  else
    rls_on_.erase(norm);
  return {{}, {}, s.enable ? "ENABLE ROW LEVEL SECURITY" : "DISABLE ROW LEVEL SECURITY", 0};
}

Result Database::execCreate(const CreateTableStmt& s) {
  requireAdmin("CREATE TABLE");
  std::string key = foldIdent(s.table);
  auto it = tables_.find(key);
  if (it != tables_.end()) {
    if (s.if_not_exists) return { {}, {}, "TABLE EXISTS " + s.table, 0 };
    throw SqlError("Tabelle existiert bereits: " + s.table);
  }
  Table t;
  t.columns = s.columns;
  tables_[key] = std::move(t);
  return { {}, {}, "CREATE TABLE", 0 };
}

Result Database::execInsert(const InsertStmt& s) {
  auto it = tables_.find(foldIdent(s.table));
  if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + s.table);
  requirePriv(s.table, "INSERT");
  Table& t = it->second;
  // Zielspalten aufloesen
  std::vector<int> colMap;  // pro Eingabeposition -> Tabellenindex
  if (s.columns.empty()) {
    for (std::size_t i = 0; i < t.columns.size(); ++i) colMap.push_back((int)i);
  } else {
    for (auto& c : s.columns) {
      int idx = t.colIndex(c);
      if (idx < 0) throw SqlError("Unbekannte Spalte: " + c);
      colMap.push_back(idx);
    }
  }
  std::size_t n = 0;
  std::vector<std::vector<Value>> pending;
  pending.reserve(s.rows.size());
  for (auto& r : s.rows) {
    if (r.size() != colMap.size())
      throw SqlError("INSERT: Spaltenzahl passt nicht (" +
                     std::to_string(r.size()) + " vs " +
                     std::to_string(colMap.size()) + ")");
    std::vector<Value> full(t.columns.size(), Value{std::monostate{}});
    for (std::size_t i = 0; i < r.size(); ++i)
      full[(std::size_t)colMap[i]] =
          coerceTo(r[i], t.columns[(std::size_t)colMap[i]].type,
                   t.columns[(std::size_t)colMap[i]].name);
    // RLS WITH CHECK: neue Rows muessen USING erfuellen (sonst 42501).
    // Pruefung vor jeder Mutation (kein Halb-Insert bei Fehler).
    if (!rowPassesRls(t, full, foldIdent(s.table), "INSERT"))
      throw SqlError("permission denied for table " + s.table +
                     " (SQLSTATE 42501)");
    pending.push_back(std::move(full));
  }
  for (auto& full : pending) {
    t.rows.push_back(std::move(full));
    ++n;
  }
  return { {}, {}, "INSERT 0 " + std::to_string(n), n };
}

Result Database::execUpdate(const UpdateStmt& s) {
  auto it = tables_.find(foldIdent(s.table));
  if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + s.table);
  requirePriv(s.table, "UPDATE");
  Table& t = it->second;
  // SET-Spalten aufloesen (unbekannt -> SqlError, vor jeder Mutation).
  std::vector<int> setIdx;
  setIdx.reserve(s.sets.size());
  for (const auto& [col, val] : s.sets) {
    (void)val;
    int idx = t.colIndex(col);
    if (idx < 0) throw SqlError("Unbekannte Spalte: " + col);
    setIdx.push_back(idx);
  }
  if (g_scopes.size() >= static_cast<std::size_t>(kMaxSubqueryDepth))
    throw SqlError("Subquery-Tiefe ueberschritten (max 8)");
  Scope outer;
  ScopeTable e;
  e.quals.insert(foldIdent(s.table));
  e.table = &t;
  outer.tables.push_back(std::move(e));
  g_scopes.push_back(std::move(outer));
  ScopeGuard guard;
  SubMap subs;
  auto resolve = [&](const Condition& c) {
    if (!c.subquery) return;
    if (subs.count(c.subquery.get())) return;
    Result r = execSelect(*c.subquery);
    if (r.columns.size() != 1)
      throw SqlError("Subquery muss genau eine Spalte liefern");
    SubRes sr;
    if (c.op == "IN" || c.op == "NOT IN") {
      sr.is_in = true;
      for (auto& row : r.rows) sr.set.push_back(row[0]);
    } else {
      sr.is_in = false;
      if (r.rows.empty())
        sr.scalar = Value{std::monostate{}};
      else if (r.rows.size() > 1)
        throw SqlError("Skalar-Subquery liefert mehr als eine Zeile");
      else
        sr.scalar = r.rows[0][0];
    }
    subs[c.subquery.get()] = std::move(sr);
  };
  for (auto& c : s.where) resolve(c);
  for (auto& gr : s.where_groups)
    for (auto& c : gr) resolve(c);
  SelectStmt f;
  f.where = s.where;
  f.where_groups = s.where_groups;
  const std::string normU = foldIdent(s.table);
  // Phase 1: sichtbare Treffer sammeln (WHERE + RLS-Visibility), neue Zeilen
  // berechnen + WITH CHECK pruefen (keine Mutation vor allen Checks).
  std::vector<std::size_t> hitIdx;
  std::vector<std::vector<Value>> newRows;
  for (std::size_t ri = 0; ri < t.rows.size(); ++ri) {
    auto& row = t.rows[ri];
    bool ok = subs.empty()
                  ? evalWhere(t, row, f)
                  : evalWhereSub(t, row, s.where, s.where_groups, subs);
    if (!ok) continue;
    if (!rowPassesRls(t, row, normU, "UPDATE")) continue;  // unsichtbar
    std::vector<Value> neu = row;
    for (std::size_t i = 0; i < s.sets.size(); ++i) {
      const std::size_t ti = static_cast<std::size_t>(setIdx[i]);
      neu[ti] = coerceTo(s.sets[i].second, t.columns[ti].type,
                         t.columns[ti].name);
    }
    if (!rowPassesRls(t, neu, normU, "UPDATE"))
      throw SqlError("permission denied for table " + s.table +
                     " (SQLSTATE 42501)");
    hitIdx.push_back(ri);
    newRows.push_back(std::move(neu));
  }
  for (std::size_t i = 0; i < hitIdx.size(); ++i)
    t.rows[hitIdx[i]] = std::move(newRows[i]);
  return { {}, {}, "UPDATE " + std::to_string(hitIdx.size()), hitIdx.size() };
}

Result Database::execDelete(const DeleteStmt& s) {
  auto it = tables_.find(foldIdent(s.table));
  if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + s.table);
  requirePriv(s.table, "DELETE");
  Table& t = it->second;
  if (g_scopes.size() >= static_cast<std::size_t>(kMaxSubqueryDepth))
    throw SqlError("Subquery-Tiefe ueberschritten (max 8)");
  Scope outer;
  ScopeTable e;
  e.quals.insert(foldIdent(s.table));
  e.table = &t;
  outer.tables.push_back(std::move(e));
  g_scopes.push_back(std::move(outer));
  ScopeGuard guard;
  SubMap subs;
  auto resolve = [&](const Condition& c) {
    if (!c.subquery) return;
    if (subs.count(c.subquery.get())) return;
    Result r = execSelect(*c.subquery);
    if (r.columns.size() != 1)
      throw SqlError("Subquery muss genau eine Spalte liefern");
    SubRes sr;
    if (c.op == "IN" || c.op == "NOT IN") {
      sr.is_in = true;
      for (auto& row : r.rows) sr.set.push_back(row[0]);
    } else {
      sr.is_in = false;
      if (r.rows.empty())
        sr.scalar = Value{std::monostate{}};
      else if (r.rows.size() > 1)
        throw SqlError("Skalar-Subquery liefert mehr als eine Zeile");
      else
        sr.scalar = r.rows[0][0];
    }
    subs[c.subquery.get()] = std::move(sr);
  };
  for (auto& c : s.where) resolve(c);
  for (auto& gr : s.where_groups)
    for (auto& c : gr) resolve(c);
  SelectStmt f;
  f.where = s.where;
  f.where_groups = s.where_groups;
  const std::string normD = foldIdent(s.table);
  std::vector<std::vector<Value>> kept;
  kept.reserve(t.rows.size());
  for (auto& row : t.rows) {
    bool ok = subs.empty()
                  ? evalWhere(t, row, f)
                  : evalWhereSub(t, row, s.where, s.where_groups, subs);
    if (!ok) {
      kept.push_back(row);
      continue;
    }
    // RLS-Visibility: Rows ohne USING-Treffer sind unsichtbar (kein DELETE).
    if (!rowPassesRls(t, row, normD, "DELETE")) {
      kept.push_back(row);
      continue;
    }
  }
  std::size_t n = t.rows.size() - kept.size();
  t.rows = std::move(kept);
  return { {}, {}, "DELETE " + std::to_string(n), n };
}

Result Database::execDrop(const DropTableStmt& s) {
  requireAdmin("DROP TABLE");
  auto it = tables_.find(foldIdent(s.table));
  if (it == tables_.end()) {
    if (s.if_exists) return { {}, {}, "DROP TABLE", 0 };
    throw SqlError("Tabelle unbekannt: " + s.table);
  }
  tables_.erase(it);
  grants_.erase(foldIdent(s.table));  // Rechte fallen mit der Tabelle (PG)
  policies_.erase(foldIdent(s.table));  // RLS-Policies fallen mit (PG)
  rls_on_.erase(foldIdent(s.table));
  return { {}, {}, "DROP TABLE", 0 };
}

Result Database::execSelect(const SelectStmt& s) {
  if (g_scopes.size() >= static_cast<std::size_t>(kMaxSubqueryDepth))
    throw SqlError("Subquery-Tiefe ueberschritten (max 8)");
  Scope my = buildScopeFor(*this, s);
  validateNotCorrelated(s, my);
  g_scopes.push_back(std::move(my));
  ScopeGuard guard;
  // WHERE-Subqueries je einmal ausfuehren (IN -> Menge, Skalar -> Wert).
  SubMap subs;
  auto resolve = [&](const Condition& c) {
    if (!c.subquery) return;
    if (subs.count(c.subquery.get())) return;
    Result r = execSelect(*c.subquery);
    if (r.columns.size() != 1)
      throw SqlError("Subquery muss genau eine Spalte liefern");
    SubRes sr;
    if (c.op == "IN" || c.op == "NOT IN") {
      sr.is_in = true;
      for (auto& row : r.rows) sr.set.push_back(row[0]);
    } else {
      sr.is_in = false;
      if (r.rows.empty())
        sr.scalar = Value{std::monostate{}};
      else if (r.rows.size() > 1)
        throw SqlError("Skalar-Subquery liefert mehr als eine Zeile");
      else
        sr.scalar = r.rows[0][0];
    }
    subs[c.subquery.get()] = std::move(sr);
  };
  for (auto& c : s.where) resolve(c);
  for (auto& gr : s.where_groups)
    for (auto& c : gr) resolve(c);
  if (s.has_join) {
    auto lit = tables_.find(foldIdent(s.table));
    if (lit == tables_.end()) throw SqlError("Tabelle unbekannt: " + s.table);
    auto rit = tables_.find(foldIdent(s.join_table));
    if (rit == tables_.end())
      throw SqlError("Tabelle unbekannt: " + s.join_table);
    requirePriv(s.table, "SELECT");
    requirePriv(s.join_table, "SELECT");
    // RLS-Visibility je Seite (Pre-Filter wie Single-Table, vor JOIN/AGGR).
    Table lf = lit->second;
    Table rf = rit->second;
    const Table* lp = &lit->second;
    const Table* rp = &rit->second;
    const std::string normL = foldIdent(s.table);
    const std::string normR = foldIdent(s.join_table);
    if (!role_.empty() &&
        (rlsEnabled(normL) || rlsEnabled(normR))) {
      lf.rows.clear();
      for (auto& row : lit->second.rows)
        if (rowPassesRls(lit->second, row, normL, "SELECT")) lf.rows.push_back(row);
      rf.rows.clear();
      // Self-Join: rechte Seite aus derselben (gefilterten) Zeilenmenge.
      const Table& rsrc =
          (normL == normR) ? lit->second : rit->second;
      const std::string& rnorm = (normL == normR) ? normL : normR;
      for (auto& row : rsrc.rows)
        if (rowPassesRls(rsrc, row, rnorm, "SELECT")) rf.rows.push_back(row);
      lp = &lf;
      rp = &rf;
    }
    JoinCtx j;
    j.left = lp;
    j.right = rp;
    j.lTable = s.table;
    j.rTable = s.join_table;
    j.lEff = s.table_alias.empty() ? s.table : s.table_alias;
    j.rEff = s.join_alias.empty() ? s.join_table : s.join_alias;
    j.nL = j.left->columns.size();
    // Defensive Huerde fuer handgebaute Statements (Parser prueft schaerfer).
    if (foldIdent(j.lEff) == foldIdent(j.rEff))
      throw SqlError(
          "JOIN braucht disjunkte Tabellen-Aliase (Self-Join: FROM t AS x "
          "JOIN t AS y ...)");
    if (s.join_on.empty())
      throw SqlError("JOIN ohne ON wird nicht unterstuetzt");
    return execJoinSelect(j, s, subs);
  }
  auto it = tables_.find(foldIdent(s.table));
  if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + s.table);
  requirePriv(s.table, "SELECT");
  const Table& t = it->second;
  const std::string normS = foldIdent(s.table);
  // Filter (AND bzw. DNF bei OR; ohne Subqueries exakt der Altpfad) + RLS.
  std::vector<std::vector<Value>> kept;
  for (auto& row : t.rows) {
    bool ok = subs.empty()
                  ? evalWhere(t, row, s)
                  : evalWhereSub(t, row, s.where, s.where_groups, subs);
    if (!ok) continue;
    if (!rowPassesRls(t, row, normS, "SELECT")) continue;  // unsichtbar
    kept.push_back(row);
  }
  if (!s.group_by.empty()) {
    return execGroupedAggregates(t, kept, s);
  }
  if (s.count_star) {
    Result r{ {"count"}, { {Value{(int64_t)kept.size()}} },
              "SELECT 1", std::size_t{1} };
    validateOrderScalar(t, kept, s, r);
    applyLimitOffset(r, s);
    return r;
  }
  if (!s.aggregates.empty()) {
    if (!s.columns.empty() || s.select_all)
      throw SqlError(
          "Ohne GROUP BY: Aggregate (SUM/AVG/MIN/MAX/COUNT) und Spalten nicht "
          "mischbar");
    return execScalarAggregates(t, kept, s);
  }
  // Projektion
  std::vector<int> idxs;
  std::vector<std::string> cols;
  if (s.select_all) {
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
      idxs.push_back((int)i);
      cols.push_back(t.columns[i].name);
    }
  } else {
    for (std::size_t i = 0; i < s.columns.size(); ++i) {
      auto& c = s.columns[i];
      int idx = t.colIndex(c);
      if (idx < 0) throw SqlError("Unbekannte Spalte: " + c);
      idxs.push_back(idx);
      std::string alias;
      if (i < s.column_aliases.size()) alias = s.column_aliases[i];
      cols.push_back(alias.empty() ? t.columns[(std::size_t)idx].name : alias);
    }
  }
  Result r;
  r.columns = cols;
  // ORDER BY vor der Projektion auf den vollen Zeilen (nicht-projizierte
  // Spalten bleiben sortierbar), LIMIT/OFFSET danach auf dem Result.
  if (!s.order_by.empty()) sortPlainRows(t, kept, s);
  for (auto& row : kept) {
    std::vector<Value> o;
    for (int i : idxs) o.push_back(row[(std::size_t)i]);
    r.rows.push_back(std::move(o));
  }
  applyLimitOffset(r, s);
  return r;
}

// Snapshot-Ausfuehrung (s. parser.h): Snapshot-Tabellen temporär als eigene
// einsetzen, exakt denselben execSelect-Pfad nutzen, danach wieder
// herstellen (auch bei Exception, damit der Database-Zustand stabil bleibt).
// Subqueries loesen sich rekursiv gegen dieselben Snapshot-Tabellen auf.
Result Database::execSelectSnapshot(const SelectStmt& s,
                                    std::map<std::string, Table> snapshot) {
  std::map<std::string, Table> saved = std::move(tables_);
  tables_ = std::move(snapshot);
  try {
    Result r = execSelect(s);
    tables_ = std::move(saved);
    return r;
  } catch (...) {
    tables_ = std::move(saved);
    throw;
  }
}

Result Database::execute(const std::string& sql) {
  // Ein Statement pro execute() (V1). Mehrere durch ';' getrennte werden
  // nur akzeptiert, wenn der Rest leer/Whitespace ist.
  Statement st = parseStatement(sql);
  if (std::holds_alternative<CreateTableStmt>(st))
    return execCreate(std::get<CreateTableStmt>(st));
  if (std::holds_alternative<InsertStmt>(st))
    return execInsert(std::get<InsertStmt>(st));
  if (std::holds_alternative<UpdateStmt>(st))
    return execUpdate(std::get<UpdateStmt>(st));
  if (std::holds_alternative<DeleteStmt>(st))
    return execDelete(std::get<DeleteStmt>(st));
  if (std::holds_alternative<DropTableStmt>(st))
    return execDrop(std::get<DropTableStmt>(st));
  if (std::holds_alternative<GrantStmt>(st))
    return execGrant(std::get<GrantStmt>(st));
  if (std::holds_alternative<RevokeStmt>(st))
    return execRevoke(std::get<RevokeStmt>(st));
  if (std::holds_alternative<SetRoleStmt>(st))
    return execSetRole(std::get<SetRoleStmt>(st));
  if (std::holds_alternative<CreatePolicyStmt>(st))
    return execCreatePolicy(std::get<CreatePolicyStmt>(st));
  if (std::holds_alternative<AlterTableRlsStmt>(st))
    return execAlterRls(std::get<AlterTableRlsStmt>(st));
  return execSelect(std::get<SelectStmt>(st));
}

// Single-Table WHERE-Match fuer UPDATE/DELETE (exportiert, auch vom
// KV/MVCC-Executor nutzbar). Semantik exakt wie SELECT: Ein-Zeilen-Filter
// ueber einer Shadow-Tabelle, daher keine eigene Condition-Duplikation.
bool rowMatchesWhere(const Table& t, const std::vector<Value>& row,
                     const std::vector<Condition>& where,
                     const std::vector<std::vector<Condition>>& where_groups) {
  Database tmp;
  CreateTableStmt c;
  c.table = "rowmatcheswhere_shadow";
  c.columns = t.columns;
  tmp.execCreate(c);
  InsertStmt ins;
  ins.table = c.table;
  ins.rows.push_back(row);
  tmp.execInsert(ins);
  SelectStmt f;
  f.table = c.table;
  f.select_all = true;
  f.where = where;
  f.where_groups = where_groups;
  return !tmp.execSelect(f).rows.empty();
}

}  // namespace dbengine::sql
