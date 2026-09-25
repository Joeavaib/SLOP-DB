// s10-columnar: Columnar-Store mit Immutable Parts.
// Siehe include/dbengine/columnar/store.h fuer API-Doku.

#include "dbengine/columnar/store.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace dbengine::columnar {

namespace {
void ThrowIfSealed(bool sealed, const char* what) {
  if (sealed) throw std::logic_error(what);
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
  if (Max() < threshold) {
    // Ganze Part ohne Zeilenpraedikat summieren.
    int64_t s = 0;
    for (int64_t v : ints_.data()) s += v;
    if (rows_scanned) *rows_scanned = 0;  // kein Zeilenfilter noetig
    return {s, false};
  }
  if (Min() >= threshold) {
    return {0, true};  // geprunt
  }
  int64_t s = 0;
  size_t n = 0;
  for (int64_t v : ints_.data()) {
    if (v < threshold) s += v;
    ++n;
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
