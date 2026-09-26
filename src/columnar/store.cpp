// s10-columnar: Columnar-Store mit Immutable Parts.
// Siehe include/dbengine/columnar/store.h fuer API-Doku.

#include "dbengine/columnar/store.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace dbengine::columnar {

namespace {
void ThrowIfSealed(bool sealed, const char* what) {
  if (sealed) throw std::logic_error(what);
}

void WriteU64(std::ofstream& o, uint64_t v) {
  o.write(reinterpret_cast<const char*>(&v), sizeof(v));
}
void WriteI64(std::ofstream& o, int64_t v) {
  o.write(reinterpret_cast<const char*>(&v), sizeof(v));
}
void WriteU32(std::ofstream& o, uint32_t v) {
  o.write(reinterpret_cast<const char*>(&v), sizeof(v));
}
bool ReadU64(std::ifstream& in, uint64_t& v) {
  return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(v)));
}
bool ReadI64(std::ifstream& in, int64_t& v) {
  return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(v)));
}
bool ReadU32(std::ifstream& in, uint32_t& v) {
  return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(v)));
}
}  // namespace

// ---- RLE -----------------------------------------------------------------
std::vector<RleRun> EncodeRle(const std::vector<int64_t>& values) {
  std::vector<RleRun> runs;
  if (values.empty()) return runs;
  runs.reserve(values.size() / 2 + 1);
  int64_t cur = values[0];
  uint64_t n = 1;
  for (size_t i = 1; i < values.size(); ++i) {
    if (values[i] == cur) {
      ++n;
    } else {
      runs.push_back({cur, n});
      cur = values[i];
      n = 1;
    }
  }
  runs.push_back({cur, n});
  return runs;
}

std::vector<int64_t> DecodeRle(const std::vector<RleRun>& runs) {
  size_t total = 0;
  for (const auto& r : runs) total += static_cast<size_t>(r.count);
  std::vector<int64_t> out;
  out.reserve(total);
  for (const auto& r : runs)
    for (uint64_t i = 0; i < r.count; ++i) out.push_back(r.value);
  return out;
}

// ---- IntColumnChunk -------------------------------------------------------
void IntColumnChunk::Append(int64_t v) {
  ThrowIfSealed(sealed_, "IntColumnChunk: append on sealed (immutable) chunk");
  if (!has_stats_) {
    min_ = max_ = v;
    has_stats_ = true;
  } else {
    if (v < min_) min_ = v;
    if (v > max_) max_ = v;
  }
  data_.push_back(v);
}

void IntColumnChunk::Seal() { sealed_ = true; }

int64_t IntColumnChunk::Min() const {
  if (!has_stats_) throw std::logic_error("IntColumnChunk: min on empty chunk");
  return min_;
}

int64_t IntColumnChunk::Max() const {
  if (!has_stats_) throw std::logic_error("IntColumnChunk: max on empty chunk");
  return max_;
}

// ---- StringDictChunk ------------------------------------------------------
void StringDictChunk::Append(const std::string& v) {
  ThrowIfSealed(sealed_, "StringDictChunk: append on sealed (immutable) chunk");
  // Simpler Dict-Aufbau: linearer Index via temporaerer Map waere O(1);
  // hier kleiner Hash-Cache ueber dict_vals_ Position. Da Append-Pfad im
  // Columnar-Tiering nicht heiss ist (Bulk-Load -> seal), reicht O(dict)
  // im Stub nicht -- deshalb lokaler statischer Index pro Chunk ueber
  // unordered_map<string,uint32_t> als Nebenstruktur? Wir halten es simpel:
  // lineare Suche wuerde bei vielen Distincts degenerieren, daher interner
  // Index via unordered_map als static thread_local Cache ist overkill.
  // Pragmatisch: unordered_map als lokale static-freie Member-Alternative:
  // Wir bauen den Index aus dict_vals_ nur bei Bedarf neu auf, wenn gross.
  // Fuer Testgroessen (wenige Distincts) reicht lineare Suche.
  for (uint32_t i = 0; i < dict_vals_.size(); ++i) {
    if (dict_vals_[i] == v) {
      codes_.push_back(i);
      return;
    }
  }
  uint32_t id = static_cast<uint32_t>(dict_vals_.size());
  dict_vals_.push_back(v);
  codes_.push_back(id);
}

void StringDictChunk::Seal() { sealed_ = true; }

std::string StringDictChunk::At(size_t i) const {
  if (i >= codes_.size()) throw std::out_of_range("StringDictChunk::At");
  return dict_vals_[codes_[i]];
}

// ---- Part -----------------------------------------------------------------
Part::Part(uint64_t id, std::string name) : id_(id), name_(std::move(name)) {}

void Part::Append(int64_t int_val, const std::string& str_val) {
  ThrowIfSealed(sealed_, "Part: append on sealed (immutable) part");
  ints_.Append(int_val);
  strs_.Append(str_val);
}

void Part::Seal() {
  ints_.Seal();
  strs_.Seal();
  sealed_ = true;
}

std::pair<int64_t, bool> Part::SumLessThan(int64_t threshold,
                                           size_t* rows_scanned) const {
  if (rows_scanned) *rows_scanned = 0;
  if (empty() || !HasStats()) return {0, true};  // leer == geprunt
  // Zonemap-Pushdown (min/max prune), analog Parquet Row-Group Stats.
  // Vektorisiert: Blockverarbeitung (2048 Zeilen), einfacher akkumulierender
  // Loop (Compiler auto-vektorisiert, -O2/-march=native profitiert).
  constexpr size_t kBlock = 2048;
  const auto& col = ints_.data();
  if (Max() < threshold) {
    // Ganze Part ohne Zeilenpraedikat summieren.
    int64_t s = 0;
    const size_t n = col.size();
    for (size_t b = 0; b < n; b += kBlock) {
      const size_t e = std::min(n, b + kBlock);
      for (size_t i = b; i < e; ++i) s += col[i];
    }
    if (rows_scanned) *rows_scanned = 0;  // kein Zeilenfilter noetig
    return {s, false};
  }
  if (Min() >= threshold) {
    return {0, true};  // geprunt
  }
  int64_t s = 0;
  const size_t n = col.size();
  for (size_t b = 0; b < n; b += kBlock) {
    const size_t e = std::min(n, b + kBlock);
    for (size_t i = b; i < e; ++i) {
      if (col[i] < threshold) s += col[i];
    }
  }
  if (rows_scanned) *rows_scanned = n;
  return {s, false};
}

bool Part::ExportCsv(const std::string& path) const {
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  // Arrow-Vorstufe: CSV mit Header (id,int,str). Real: Arrow IPC RecordBatch.
  out << "id,int_val,str_val\n";
  for (size_t i = 0; i < size(); ++i) {
    out << i << "," << ints_.data()[i] << "," << strs_.At(i) << "\n";
    if (!out) return false;
  }
  return true;
}

bool Part::ExportBinary(const std::string& path) const {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  // Binaer-Dump als Arrow-Vorstufe:
  // magic "ARW1" | u64 rows | int64[min] | int64[max] | rows x int64 LE |
  // dann je Zeile: u32 len + bytes (str). Real: Arrow IPC + Dict/RLE/ZSTD.
  const char magic[4] = {'A', 'R', 'W', '1'};
  out.write(magic, 4);
  uint64_t rows = static_cast<uint64_t>(size());
  out.write(reinterpret_cast<const char*>(&rows), sizeof(rows));
  int64_t mn = empty() ? 0 : Min();
  int64_t mx = empty() ? 0 : Max();
  out.write(reinterpret_cast<const char*>(&mn), sizeof(mn));
  out.write(reinterpret_cast<const char*>(&mx), sizeof(mx));
  for (int64_t v : ints_.data())
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
  for (size_t i = 0; i < size(); ++i) {
    std::string s = strs_.At(i);
    uint32_t len = static_cast<uint32_t>(s.size());
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    if (!s.empty()) out.write(s.data(), static_cast<std::streamsize>(s.size()));
  }
  return static_cast<bool>(out);
}

bool Part::DecodeBinary(const std::string& path,
                        std::vector<int64_t>* out_ints,
                        std::vector<std::string>* out_strs) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  char magic[4];
  if (!in.read(magic, 4) || std::memcmp(magic, "ARW1", 4) != 0) return false;
  uint64_t rows = 0;
  int64_t mn = 0, mx = 0;
  if (!in.read(reinterpret_cast<char*>(&rows), sizeof(rows))) return false;
  if (!in.read(reinterpret_cast<char*>(&mn), sizeof(mn))) return false;
  if (!in.read(reinterpret_cast<char*>(&mx), sizeof(mx))) return false;
  std::vector<int64_t> ints(rows);
  for (uint64_t i = 0; i < rows; ++i)
    if (!in.read(reinterpret_cast<char*>(&ints[i]), sizeof(int64_t)))
      return false;
  std::vector<std::string> strs;
  strs.reserve(static_cast<size_t>(rows));
  for (uint64_t i = 0; i < rows; ++i) {
    uint32_t len = 0;
    if (!in.read(reinterpret_cast<char*>(&len), sizeof(len))) return false;
    std::string s(len, '\0');
    if (len > 0 && !in.read(s.data(), len)) return false;
    strs.push_back(std::move(s));
  }
  (void)mn;
  (void)mx;
  if (out_ints) *out_ints = std::move(ints);
  if (out_strs) *out_strs = std::move(strs);
  return true;
}

// ---- s24: COL1 Part-File (Header + RLE + Dict) -----------------------------
// Layout (LE, host == x86-64 LE):
// magic[4]="COL1" | u64 id | u64 name_len | name | u64 rows |
// i64 min | i64 max | u64 nruns | runs(value i64, count u64) |
// u64 dict_size | per entry u32 len + bytes | u64 ncodes | codes u32
bool Part::Save(const std::string& path) const {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write("COL1", 4);
  WriteU64(out, id_);
  WriteU64(out, static_cast<uint64_t>(name_.size()));
  if (!name_.empty()) out.write(name_.data(), (std::streamsize)name_.size());
  const uint64_t rows = static_cast<uint64_t>(size());
  WriteU64(out, rows);
  WriteI64(out, empty() ? 0 : Min());
  WriteI64(out, empty() ? 0 : Max());
  auto runs = EncodeRle(ints_.data());
  WriteU64(out, static_cast<uint64_t>(runs.size()));
  for (const auto& r : runs) {
    WriteI64(out, r.value);
    WriteU64(out, r.count);
  }
  const auto& dict = strs_.dict_values();
  const auto& codes = strs_.codes();
  WriteU64(out, static_cast<uint64_t>(dict.size()));
  for (const auto& s : dict) {
    WriteU32(out, static_cast<uint32_t>(s.size()));
    if (!s.empty()) out.write(s.data(), (std::streamsize)s.size());
  }
  WriteU64(out, static_cast<uint64_t>(codes.size()));
  for (uint32_t c : codes) WriteU32(out, c);
  out.flush();
  return static_cast<bool>(out);
}

Part Part::Load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("Part::Load: cannot open " + path);
  char magic[4];
  if (!in.read(magic, 4) || std::memcmp(magic, "COL1", 4) != 0)
    throw std::runtime_error("Part::Load: bad magic " + path);
  uint64_t id = 0, name_len = 0, rows = 0;
  if (!ReadU64(in, id) || !ReadU64(in, name_len))
    throw std::runtime_error("Part::Load: truncated header");
  if (name_len > (1u << 20))
    throw std::runtime_error("Part::Load: name too long");
  std::string name(name_len, '\0');
  if (name_len && !in.read(name.data(), (std::streamsize)name_len))
    throw std::runtime_error("Part::Load: truncated name");
  if (!ReadU64(in, rows))
    throw std::runtime_error("Part::Load: truncated rows");
  if (rows > (1ULL << 32))
    throw std::runtime_error("Part::Load: rows implausible");
  int64_t mn = 0, mx = 0;
  if (!ReadI64(in, mn) || !ReadI64(in, mx))
    throw std::runtime_error("Part::Load: truncated stats");
  uint64_t nruns = 0;
  if (!ReadU64(in, nruns) || nruns > rows + 1)
    throw std::runtime_error("Part::Load: bad nruns");
  std::vector<RleRun> runs;
  runs.reserve((size_t)std::min<uint64_t>(nruns, 1 << 20));
  for (uint64_t i = 0; i < nruns; ++i) {
    int64_t v = 0;
    uint64_t c = 0;
    if (!ReadI64(in, v) || !ReadU64(in, c))
      throw std::runtime_error("Part::Load: truncated runs");
    runs.push_back({v, c});
  }
  std::vector<int64_t> ints = DecodeRle(runs);
  if (ints.size() != rows)
    throw std::runtime_error("Part::Load: rows mismatch (RLE)");
  uint64_t dict_size = 0;
  if (!ReadU64(in, dict_size) || dict_size > (1u << 24))
    throw std::runtime_error("Part::Load: bad dict");
  std::vector<std::string> dict;
  dict.reserve((size_t)dict_size);
  for (uint64_t i = 0; i < dict_size; ++i) {
    uint32_t len = 0;
    if (!ReadU32(in, len) || len > (1u << 24))
      throw std::runtime_error("Part::Load: bad dict entry");
    std::string s(len, '\0');
    if (len && !in.read(s.data(), len))
      throw std::runtime_error("Part::Load: truncated dict");
    dict.push_back(std::move(s));
  }
  uint64_t ncodes = 0;
  if (!ReadU64(in, ncodes) || ncodes != rows)
    throw std::runtime_error("Part::Load: bad ncodes");
  std::vector<uint32_t> codes;
  codes.reserve((size_t)ncodes);
  for (uint64_t i = 0; i < ncodes; ++i) {
    uint32_t c = 0;
    if (!ReadU32(in, c) || (dict_size && c >= dict_size))
      throw std::runtime_error("Part::Load: bad code");
    codes.push_back(c);
  }
  Part p(id, name);
  for (uint64_t i = 0; i < rows; ++i) {
    std::string s = dict_size ? dict[codes[i]] : "";
    p.Append(ints[(size_t)i], s);
  }
  p.Seal();
  return p;
}

Part Part::Merge(const Part& a, const Part& b, uint64_t new_id) {
  // Sorted-Merge nach int (stabil, Tie-Break str): frische Zonemaps via Append.
  std::vector<std::pair<int64_t, std::string>> all;
  all.reserve(a.size() + b.size());
  for (size_t i = 0; i < a.size(); ++i)
    all.emplace_back(a.ints().data()[i], a.strs().At(i));
  for (size_t i = 0; i < b.size(); ++i)
    all.emplace_back(b.ints().data()[i], b.strs().At(i));
  std::stable_sort(all.begin(), all.end(),
                   [](const auto& x, const auto& y) {
                     if (x.first != y.first) return x.first < y.first;
                     return x.second < y.second;
                   });
  const uint64_t id =
      (new_id != 0) ? new_id : (std::max(a.id(), b.id()) + 1);
  Part out(id, a.name().empty() ? b.name() : a.name());
  for (auto& [v, s] : all) out.Append(v, s);
  out.Seal();
  return out;
}

// ---- ColumnarStore --------------------------------------------------------
ColumnarStore::ColumnarStore() : active_(0), next_id_(1) {}

void ColumnarStore::Append(int64_t int_val, const std::string& str_val) {
  active_.Append(int_val, str_val);
}

void ColumnarStore::SealActive() {
  active_.Seal();
  if (!active_.empty()) {
    parts_.push_back(std::move(active_));
  }
  active_ = Part(next_id_++);
}

size_t ColumnarStore::TotalRows() const {
  size_t n = active_.size();
  for (const auto& p : parts_) n += p.size();
  return n;
}

ColumnarStore::ScanResult ColumnarStore::ScanSumLessThan(
    int64_t threshold) const {
  ScanResult r;
  auto scan_one = [&](const Part& p) {
    ++r.parts_total;
    if (p.empty()) {
      ++r.parts_pruned;
      return;
    }
    size_t scanned = 0;
    auto [sum, pruned] = p.SumLessThan(threshold, &scanned);
    r.sum += sum;
    r.rows_scanned += scanned;
    if (pruned)
      ++r.parts_pruned;
    else if (scanned == 0)
      ++r.parts_full;  // via max-prune, kein Zeilenvergleich
  };
  for (const auto& p : parts_) scan_one(p);
  // Aktiver Part ist per Definition nicht immutable, wird aber beim Scan
  // gleich behandelt (Pruning greift identisch). Leerer Aktiv-Part wird
  // nicht mitgezaehlt, damit parts_total/pruned nur echte Parts zaehlen.
  // Seal-Pflicht gilt nur fuer Tiering/Export-Konsistenz, nicht fuers Lesen.
  if (!active_.empty()) scan_one(active_);
  return r;
}

bool ColumnarStore::ExportCsv(const std::string& path) const {
  // Exportiert sealed Parts + aktiven Part (falls nicht leer) in eine Datei.
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << "id,int_val,str_val\n";
  size_t id = 0;
  auto dump = [&](const Part& p) -> bool {
    for (size_t i = 0; i < p.size(); ++i) {
      out << id++ << "," << p.ints().data()[i] << "," << p.strs().At(i)
          << "\n";
      if (!out) return false;
    }
    return true;
  };
  for (const auto& p : parts_)
    if (!dump(p)) return false;
  if (!active_.empty())
    if (!dump(active_)) return false;
  return true;
}

bool ColumnarStore::ExportBinary(const std::string& path) const {
  // Vereinfacht: concatenierter Dump (eine logische Part-Datei).
  // Real: ein File je Part (Parquet) + Manifest.
  ColumnarStore flat;
  // Hinweis: Kopie ueber Parts hinweg ist O(n), ok fuer Stub/Tests.
  for (const auto& p : parts_)
    for (size_t i = 0; i < p.size(); ++i)
      flat.Append(p.ints().data()[i], p.strs().At(i));
  for (size_t i = 0; i < active_.size(); ++i)
    flat.Append(active_.ints().data()[i], active_.strs().At(i));
  flat.SealActive();
  if (flat.parts_.empty()) {
    Part empty(0);
    empty.Seal();
    return empty.ExportBinary(path);
  }
  return flat.parts_[0].ExportBinary(path);
}

bool ColumnarStore::Save(const std::string& dir) const {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) return false;
  std::ofstream man(std::filesystem::path(dir) / "manifest.txt",
                    std::ios::trunc);
  if (!man) return false;
  man << parts_.size() << "\n";
  for (const auto& p : parts_) {
    const std::string fname = "part-" + std::to_string(p.id()) + ".col";
    man << p.id() << " " << fname << " " << p.size() << "\n";
  }
  man.flush();
  if (!man) return false;
  for (const auto& p : parts_) {
    const std::string fpath =
        (std::filesystem::path(dir) / ("part-" + std::to_string(p.id()) + ".col"))
            .string();
    if (!p.Save(fpath)) return false;
  }
  return true;
}

bool ColumnarStore::Load(const std::string& dir) {
  std::ifstream man(std::filesystem::path(dir) / "manifest.txt");
  if (!man) return false;
  size_t n = 0;
  if (!(man >> n)) return false;
  std::vector<Part> parts;
  parts.reserve(n);
  uint64_t max_id = 0;
  for (size_t i = 0; i < n; ++i) {
    uint64_t id = 0, rows_expect = 0;
    std::string fname;
    if (!(man >> id >> fname >> rows_expect)) return false;
    const std::string fpath = (std::filesystem::path(dir) / fname).string();
    try {
      Part p = Part::Load(fpath);
      if (p.size() != rows_expect) return false;
      max_id = std::max(max_id, p.id());
      parts.push_back(std::move(p));
    } catch (...) {
      return false;
    }
  }
  parts_ = std::move(parts);
  next_id_ = max_id + 1;
  active_ = Part(next_id_++);
  return true;
}

void ColumnarStore::Compact() {
  if (parts_.size() <= 1) return;
  Part acc = std::move(parts_[0]);
  for (size_t i = 1; i < parts_.size(); ++i)
    acc = Part::Merge(acc, parts_[i], next_id_++);
  parts_.clear();
  parts_.push_back(std::move(acc));
}

std::string ColumnarStore::StageToS3(const std::string& bucket,
                                     const std::string& prefix) const {
  // S3-Tier Stub: bildet nur die Ziel-URI. Kein Netzwerk/Upload.
  // Real (Follow-up): Parquet-Parts serialisieren, S3 multipart-Put,
  // Manifest + Zonemaps aktualisieren, lokale Parts evicten.
  std::string p = prefix;
  if (!p.empty() && p.back() != '/') p += '/';
  return "s3://" + bucket + "/" + p + "columnar-" +
         std::to_string(parts_.size()) + "parts/";
}

}  // namespace dbengine::columnar
