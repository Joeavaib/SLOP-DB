#pragma once

// Columnar-Tiering (s10): Immutable Parts, Parquet-like.
// Fokus: Immutable Parts + Zonemap-Pruning. Kein OLTP-Retrofit.
// Encoding hier nur als Stub: RLE fuer ints (echt, simpel), Dict fuer
// strings (echt, simpel). ZSTD/Parquet/Arrow-IPC sind bewusst Stubs
// (Export als CSV/Binaer = Arrow-Vorstufe, S3 als Pfad-Stub).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dbengine::columnar {

// ---- Encoding-Stub: RLE fuer int64 --------------------------------------
// Format: vector<RleRun>{value, count}. Kompression wirkt nur bei Runs.
// Parquet nutzt RLE/Bit-Packing hybrid + Dict + ZSTD; das hier ist die
// Minimal-Vorstufe (Kommentar + einfache RLE fuer ints, s. Vorgabe).
struct RleRun {
  int64_t value = 0;
  uint64_t count = 0;
};

std::vector<RleRun> EncodeRle(const std::vector<int64_t>& values);
std::vector<int64_t> DecodeRle(const std::vector<RleRun>& runs);

// ---- Spalten-Chunks (spaltenweise Speicherung) ---------------------------

class IntColumnChunk {
 public:
  void Append(int64_t v);
  void Seal();  // macht Chunk read-only (immutable)
  bool sealed() const { return sealed_; }
  size_t size() const { return data_.size(); }
  bool empty() const { return data_.empty(); }

  int64_t Min() const;
  int64_t Max() const;
  bool HasStats() const { return has_stats_; }

  const std::vector<int64_t>& data() const { return data_; }

  // Encoding-Stub Zugriff.
  std::vector<RleRun> EncodeRle() const { return columnar::EncodeRle(data_); }
  size_t BytesRaw() const { return data_.size() * sizeof(int64_t); }
  static size_t BytesRle(const std::vector<RleRun>& runs) {
    return runs.size() * (sizeof(int64_t) + sizeof(uint64_t));
  }

 private:
  std::vector<int64_t> data_;
  bool sealed_ = false;
  bool has_stats_ = false;
  int64_t min_ = 0;
  int64_t max_ = 0;
};

// String-Spalte mit Dict-Encoding-Stub:
// codes_[row] -> dict_vals_[code]. Echter Dict-Ansatz, aber ohne
// Bit-Packing/ZSTD (Parquet-Follow-up).
class StringDictChunk {
 public:
  void Append(const std::string& v);
  void Seal();
  bool sealed() const { return sealed_; }
  size_t size() const { return codes_.size(); }
  bool empty() const { return codes_.empty(); }

  std::string At(size_t i) const;
  size_t DictSize() const { return dict_vals_.size(); }
  const std::vector<std::string>& dict_values() const { return dict_vals_; }
  const std::vector<uint32_t>& codes() const { return codes_; }

 private:
  // Linearer unordered_map-Lookup waere schneller; fuer Determinismus +
  // Stub reicht map-auf-vector mit Hash-Index.
  std::vector<std::string> dict_vals_;
  // Kleiner Hash-Index, wird nach Seal() nicht mehr mutiert.
  // (mutable als Impl-Detail: Lookup-Cache, kein logischer State)
  std::vector<uint32_t> codes_;
  bool sealed_ = false;
};

// ---- Immutable Part ------------------------------------------------------
// Ein Part = ein Satz Spalten-Chunks gleicher Laenge (hier: 1x int + 1x str).
// Nach seal() read-only: jeder Append wirft. Parts sind die
// Parquet-Row-Group-Analogie (Zonemaps = min/max je Part).
class Part {
 public:
  explicit Part(uint64_t id = 0, std::string name = "");

  void Append(int64_t int_val, const std::string& str_val = "");
  void Seal();

  bool sealed() const { return sealed_; }
  bool empty() const { return ints_.empty(); }
  size_t size() const { return ints_.size(); }
  uint64_t id() const { return id_; }
  const std::string& name() const { return name_; }

  bool HasStats() const { return ints_.HasStats(); }
  int64_t Min() const { return ints_.Min(); }
  int64_t Max() const { return ints_.Max(); }

  const IntColumnChunk& ints() const { return ints_; }
  const StringDictChunk& strs() const { return strs_; }

  // Scan mit Pushdown: SUM(ints) WHERE ints < threshold.
  // Min/Max-Pruning:
  //   max < threshold  -> ganze Part summieren (kein Zeilenvergleich)
  //   min >= threshold -> Part ueberspringen (pruned=true, 0)
  //   sonst            -> Zeilenfilter.
  // Gibt (sum, pruned) zurueck, zaehlt gescannte Zeilen in rows_scanned.
  std::pair<int64_t, bool> SumLessThan(int64_t threshold,
                                       size_t* rows_scanned = nullptr) const;

  // Arrow-Export-Stub: CSV + Binaer-Dump als Arrow-Vorstufe.
  // Real: Arrow IPC / Parquet + ZSTD (Follow-up, s. store.cpp).
  bool ExportCsv(const std::string& path) const;
  bool ExportBinary(const std::string& path) const;
  static bool DecodeBinary(const std::string& path,
                           std::vector<int64_t>* out_ints,
                           std::vector<std::string>* out_strs);

  // s24: persistentes Part-File COL1 (Header + RLE-Ints + Dict-Strings)
  // + COL2 (Header + FOR/Bitpacking-Ints + Dict-Strings, STL-only).
  // Save schreibt COL2; Load liest COL1 (Fallback) UND COL2.
  // COL2-Int-Layout: u32 bitwidth | u64 nwords | nwords x u64 LE-Worte;
  // Deltas = value - min (mod 2^64), LSB-first bitgepackt, bitwidth =
  // bit_width(max_delta) (0 => alle == min, keine Worte). Roundtrip exakt.
  // Save/Load sind roundtrip-treu (id/name/Rows/Zonemaps). Load gibt
  // versiegelten Part zurueck.
  bool Save(const std::string& path) const;
  static Part Load(const std::string& path);
  // Sorted-Merge zweier Parts (nach int, stabil): neuer versiegelter Part.
  static Part Merge(const Part& a, const Part& b, uint64_t new_id = 0);

 private:
  uint64_t id_ = 0;
  std::string name_;
  IntColumnChunk ints_;
  StringDictChunk strs_;
  bool sealed_ = false;
};

// ---- ColumnarStore: Tiering aus Immutable Parts --------------------------
// Aktiver (mutabler) Part nimmt Writes auf; seal_active() friert ihn ein
// (immutable) und startet einen neuen. Scans laufen ueber alle Parts mit
// Part-Level Pruning. S3-Tiering ist Stub (Pfad-URI, kein Upload).
class ColumnarStore {
 public:
  ColumnarStore();

  void Append(int64_t int_val, const std::string& str_val = "");
  void SealActive();  // aktiven Part sealen + als immutable ablegen

  size_t ActiveSize() const { return active_.size(); }
  size_t NumSealedParts() const { return parts_.size(); }
  size_t TotalRows() const;

  struct ScanResult {
    int64_t sum = 0;
    size_t parts_total = 0;
    size_t parts_pruned = 0;
    size_t parts_full = 0;  // via max< threshold ohne Zeilenvergleich
    size_t rows_scanned = 0;
  };
  ScanResult ScanSumLessThan(int64_t threshold) const;
  // Paralleler Scan (Baseline: ScanSumLessThan bleibt unveraendert).
  //  n_threads==0 -> hardware_concurrency (0->4 als Fallback), 1 -> Single-Pfad.
  //  Ein Thread pro sealed Part (immutable, kein Lock) + aktiver Part im
  //  Caller-Thread, Teilsummen via Futures, deterministisch in Part-Reihenfolge
  //  kombiniert (sealed 0..N-1, dann aktiv). Fallback (byte-identisch):
  //  n_threads<=1 oder Scan-Units<=1 -> ScanSumLessThan(threshold).
  ScanResult ScanSumLessThanParallel(int64_t threshold,
                                     unsigned n_threads = 0) const;

  bool ExportCsv(const std::string& path) const;
  bool ExportBinary(const std::string& path) const;

  // s24: Store-Persistenz (Verzeichnis mit Manifest + part-*.col) + Compaction.
  // Save persistiert sealed Parts als part-<id>.col plus -- falls nicht leer --
  // den aktiven (unsealed) Part als part-<id>-active.col; Manifest-Zeilen sind
  // "<id> <fname> <rows> <active 0/1>" (0 = sealed, 1 = aktiv). Manifest wird
  // ZULETZT atomar (tmp + rename + fsync) geschrieben; leere Active => kein
  // File (Verhalten wie bisher). Load stellt aktive Rows unsealed/mutabel
  // wieder her (TotalRows identisch) und gibt false bei fehlendem/korruptem
  // Part-File zurueck.
  bool Save(const std::string& dir) const;
  bool Load(const std::string& dir);
  void Compact();
  const Part& sealed_part(size_t i) const { return parts_.at(i); }

  // S3-Tier Stub: kein Upload, nur deterministische URI-Bildung.
  // Real: Parquet-Parts -> S3 Put + Manifest (Follow-up).
  std::string StageToS3(const std::string& bucket,
                        const std::string& prefix) const;

 private:
  std::vector<Part> parts_;  // sealed, immutable
  Part active_;              // einziger mutabler Part
  uint64_t next_id_ = 0;
};

}  // namespace dbengine::columnar
