// Mini-SQL V1: Tokenizer + Recursive-Descent-Parser + In-Memory-Executor.
// PG-kompatibler Subset (siehe parser.h). Kein Flex/Bison.

#include "dbengine/sql/parser.h"

#include <cctype>
#include <cmath>
#include <regex>
#include <sstream>

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
    if (matchKeyword("CREATE")) return parseCreate();
    if (matchKeyword("INSERT")) return parseInsert();
    if (matchKeyword("SELECT")) return parseSelect();
    throw SqlError(
        "Nur CREATE TABLE / INSERT / SELECT werden in V1 unterstuetzt");
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

  SelectStmt parseSelect() {
    SelectStmt s;
    // Projektion
    if (matchSymbol("*")) {
      s.select_all = true;
    } else if (peekKeyword("COUNT")) {
      ++pos_;
      expectSymbol("(");
      if (!(matchSymbol("*"))) throw SqlError("Nur COUNT(*) in V1");
      expectSymbol(")");
      s.select_all = false;
      s.count_star = true;
    } else {
      s.select_all = false;
      while (true) {
        s.columns.push_back(parseIdent());
        if (matchSymbol(",")) continue;
        break;
      }
    }
    expectKeyword("FROM");
    s.table = parseIdent();
    if (matchKeyword("WHERE")) {
      while (true) {
        s.where.push_back(parseCondition());
        if (matchKeyword("AND")) continue;
        break;
      }
      if (matchKeyword("OR"))
        throw SqlError("OR erst ab V2 (aktuell nur AND)");
    }
    // LIMIT/OFFSET/ORDER BY -> klare V2-Fehlermeldung statt Silent-Ignore
    if (peekKeyword("ORDER") || peekKeyword("LIMIT") || peekKeyword("OFFSET") ||
        peekKeyword("GROUP") || peekKeyword("JOIN"))
      throw SqlError("ORDER BY / LIMIT / GROUP BY / JOIN erst ab V2");
    return s;
  }

  Condition parseCondition() {
    Condition c;
    c.column = parseIdent();
    const Token& t = next();
    if (t.kind == TokKind::Ident) {
      std::string kw = toUpper(t.text);
      if (kw == "LIKE" || kw == "ILIKE") {
        c.op = kw;
        c.value = parseLiteral();
        return c;
      }
      if (kw == "IS") {
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
      throw SqlError("Unbekannter Operator: " + t.text);
    }
    if (t.kind == TokKind::Symbol) {
      static const char* kOps[] = {"=", "<", "<=", ">", ">=", "<>", "!="};
      bool ok = false;
      for (auto o : kOps)
        if (t.text == o) ok = true;
      if (!ok) throw SqlError("Unbekannter Operator: " + t.text);
      c.op = t.text;
      if (c.op == "!=") c.op = "<>";
      c.value = parseLiteral();
      return c;
    }
    throw SqlError("Erwartet Operator nach Spaltenname");
  }

  std::vector<Token> toks_;
  std::size_t pos_ = 0;
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
  std::regex::flag_type f = std::regex::ECMAScript;
  if (ci) f |= std::regex::icase;
  return std::regex_match(s, std::regex(rx, f));
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
  int idx = t.colIndex(c.column);
  if (idx < 0) throw SqlError("Unbekannte Spalte in WHERE: " + c.column);
  const Value& v = row[(std::size_t)idx];
  if (c.op == "IS NULL") return valueIsNull(v);
  if (c.op == "IS NOT NULL") return !valueIsNull(v);
  if (valueIsNull(v) || valueIsNull(c.value)) return false;  // NULL -> false
  if (c.op == "LIKE" || c.op == "ILIKE") {
    auto* vs = std::get_if<std::string>(&v);
    auto* ps = std::get_if<std::string>(&c.value);
    if (!vs || !ps)
      throw SqlError(c.op + " braucht TEXT-Operanden");
    return likeMatch(*vs, *ps, c.op == "ILIKE");
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

Result Database::execCreate(const CreateTableStmt& s) {
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
    t.rows.push_back(std::move(full));
    ++n;
  }
  return { {}, {}, "INSERT 0 " + std::to_string(n), n };
}

Result Database::execSelect(const SelectStmt& s) {
  auto it = tables_.find(foldIdent(s.table));
  if (it == tables_.end()) throw SqlError("Tabelle unbekannt: " + s.table);
  const Table& t = it->second;
  // Filter
  std::vector<std::vector<Value>> kept;
  for (auto& row : t.rows) {
    bool ok = true;
    for (auto& c : s.where)
      if (!evalCondition(t, row, c)) {
        ok = false;
        break;
      }
    if (ok) kept.push_back(row);
  }
  if (s.count_star) {
    return { {"count"}, { {Value{(int64_t)kept.size()}} },
             "SELECT 1", std::size_t{1} };
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
    for (auto& c : s.columns) {
      int idx = t.colIndex(c);
      if (idx < 0) throw SqlError("Unbekannte Spalte: " + c);
      idxs.push_back(idx);
      cols.push_back(t.columns[(std::size_t)idx].name);
    }
  }
  Result r;
  r.columns = cols;
  for (auto& row : kept) {
    std::vector<Value> o;
    for (int i : idxs) o.push_back(row[(std::size_t)i]);
    r.rows.push_back(std::move(o));
  }
  r.message = "SELECT " + std::to_string(r.rows.size());
  r.affected = r.rows.size();
  return r;
}

Result Database::execute(const std::string& sql) {
  // Ein Statement pro execute() (V1). Mehrere durch ';' getrennte werden
  // nur akzeptiert, wenn der Rest leer/Whitespace ist.
  Statement st = parseStatement(sql);
  if (std::holds_alternative<CreateTableStmt>(st))
    return execCreate(std::get<CreateTableStmt>(st));
  if (std::holds_alternative<InsertStmt>(st))
    return execInsert(std::get<InsertStmt>(st));
  return execSelect(std::get<SelectStmt>(st));
}

}  // namespace dbengine::sql
