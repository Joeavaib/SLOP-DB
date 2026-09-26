// s11-executor: Parser + KV + MVCC + WAL-Verdrahtung.

#include "dbengine/sql/executor.h"

#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>

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

}  // namespace

Executor::Executor(kv::KVStore& kv, txn::MvccStore& mvcc, storage::Wal* wal)
    : kv_(kv), mvcc_(mvcc), wal_(wal) {}

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

Result Executor::execute(const std::string& sql) {
  Statement st = parseStatement(sql);
  if (std::holds_alternative<CreateTableStmt>(st))
    return execCreate(std::get<CreateTableStmt>(st));
  if (std::holds_alternative<InsertStmt>(st))
    return execInsert(std::get<InsertStmt>(st));
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
    }
    wal_->flush();
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

  // Snapshot-Read: Key-Mengen aus EINEM KV-Snapshot, Werte aus EINEM
  // MVCC-Snapshot (beide Seiten konsistent). Sichtbar NUR bei committed
  // MVCC-Version (kein KV-Fallback-Dirty-Read).
  auto snap = kv_.GetSnapshot();
  txn::Transaction rtxn = mvcc_.BeginRead();
  auto loadRows = [&](const std::string& tnorm, const TableSchema& sc) {
    std::vector<std::vector<Value>> rows;
    auto kvs = snap->Scan(tablePrefix(tnorm));
    rows.reserve(kvs.size());
    for (auto& [k, v] : kvs) {
      (void)v;  // Key-Menge aus KV, Wert NUR aus MVCC.
      auto mv = mvcc_.Read(rtxn, k);
      if (!mv.has_value()) continue;
      const std::string& enc = *mv;
      try {
        rows.push_back(decodeRow(enc, sc.columns.size()));
      } catch (...) {
        continue;  // korrupte Zeile ueberspringen (sollte nicht passieren)
      }
    }
    return rows;
  };
  std::vector<std::vector<Value>> allRows = loadRows(norm, sch);
  std::vector<std::vector<Value>> allRRows;
  if (needRight) allRRows = loadRows(rnorm, *rsch);
  mvcc_.Commit(rtxn);

  // Filter/Projektion/JOIN an In-Memory-Database delegieren
  // (parser-kompatible Semantik: =, <>, LIKE/ILIKE, AND, IS NULL ...).
  Database tmp;
  tmp.execCreate(CreateTableStmt{s.table, sch.columns, false});
  if (!allRows.empty()) {
    InsertStmt ins;
    ins.table = s.table;
    ins.rows = allRows;
    tmp.execInsert(ins);
  }
  if (needRight) {
    tmp.execCreate(CreateTableStmt{s.join_table, rsch->columns, false});
    if (!allRRows.empty()) {
      InsertStmt ins;
      ins.table = s.join_table;
      ins.rows = allRRows;
      tmp.execInsert(ins);
    }
  }
  return tmp.execSelect(s);
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
}

bool Executor::applyInsertRecord(const std::string& table, const std::string& key,
                                 const std::string& rowEnc) {
  const std::string norm = normalizeTable(table);
  if (tables_.find(norm) == tables_.end()) return false;  // ohne Schema nicht replaybar
  kv_.Put(key, rowEnc);
  txn::Transaction w = mvcc_.BeginWriteBlocking();
  if (mvcc_.Write(w, key, rowEnc)) {
    if (mvcc_.Commit(w)) return true;
    mvcc_.Abort(w);
    return false;
  }
  mvcc_.Abort(w);
  return false;
}

std::size_t Executor::recover() {
  recover_skipped_ = 0;
  recover_applied_ = 0;
  if (wal_ == nullptr) return 0;
  auto recs = wal_->replay();
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
  return recover_skipped_;
}

}  // namespace dbengine::sql
