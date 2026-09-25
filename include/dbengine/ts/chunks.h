#pragma once

// s17-ts: Time-Series Hypertable-Chunks + Retention + Continuous-Agg + ASOF-Join.
// Fokus: Timescale-Analogie minimal, single-node, in-memory:
//   Hypertable  -> routed Inserts auf Zeit-Chunks (chunk_interval_ms)
//   Chunk       -> rows zeit-sortiert, [t_start, t_end), Chunk-Pruning bei Scans
//   Retention   -> dropChunksBefore (Drop ganzer Chunks, kein Zeilen-Tombstone)
//   ContinuousAgg -> inkrementelle Bucket-Aggregate (sum/count, avg abgeleitet)
//   AsofJoin    -> Backward-ASOF Stub (letzte rechte Zeile <= linke Zeit)
// Kein WAL/Persistenz, keine Kompression (Follow-up: Gorilla/ZSTD, S3-Tiering).

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace dbengine::ts {

// ---- Zeile ----------------------------------------------------------------
// ts_ms: Millisekunden seit Epoche (kann negativ sein, Floor-Division beachten).
// value: Messwert (double, Summen in double akkumuliert).
struct Row {
  int64_t ts_ms = 0;
  double value = 0.0;
};

// Floor-Division-Bucket: funktioniert auch fuer negative ts.
// Gibt Bucket-/Chunk-Start fuer gegebenes Intervall zurueck.
inline int64_t FloorBucket(int64_t ts_ms, int64_t interval_ms) {
  int64_t q = ts_ms / interval_ms;
  int64_t r = ts_ms % interval_ms;
  if ((r != 0) && ((r < 0) != (interval_ms < 0))) --q;
  return q * interval_ms;
}

// ---- Chunk ----------------------------------------------------------------
// Ein Chunk deckt [t_start, t_start + interval) ab, rows sind strikt nach
// Insert-Reihenfolge zeit-sortiert gehalten (Append-Fast-Path + lower_bound
// fuer Out-of-Order). Chunks sind logisch immutable nach Retention-Drop,
// aber vor dem Drop mutabel (aktive Ingest-Chunks).
class Chunk {
 public:
  explicit Chunk(int64_t t_start = 0, int64_t interval_ms = 1);

  // Fuegt Zeile ein, haelt rows() zeit-sortiert. Wirft std::out_of_range
  // wenn ts ausserhalb [t_start, t_end) liegt (Routing-Fehler).
  void Insert(Row r);

  bool Contains(int64_t ts_ms) const {
    return ts_ms >= t_start_ && ts_ms < t_end_;
  }
  bool Overlaps(int64_t t0_ms, int64_t t1_ms) const {
    return t_start_ < t1_ms && t_end_ > t0_ms;
  }

  int64_t t_start() const { return t_start_; }
  int64_t t_end() const { return t_end_; }
  int64_t interval_ms() const { return t_end_ - t_start_; }
  size_t Size() const { return rows_.size(); }
  bool empty() const { return rows_.empty(); }
  const std::vector<Row>& rows() const { return rows_; }

  // Pruning-freundliche Scans ueber [t0, t1).
  std::vector<Row> ScanRange(int64_t t0_ms, int64_t t1_ms) const;
  double SumRange(int64_t t0_ms, int64_t t1_ms) const;
  size_t CountRange(int64_t t0_ms, int64_t t1_ms) const;

  // Prueft Zeit-Ordnung (fuer Tests/Debug).
  bool IsOrdered() const;

 private:
  int64_t t_start_ = 0;
  int64_t t_end_ = 1;
  std::vector<Row> rows_;
};

// ---- Hypertable ------------------------------------------------------------
// Routet Inserts per FloorBucket(ts, chunk_interval_ms) in den passenden
// Chunk (std::map<chunk_start, Chunk>, sortiert -> Range-Pruning).
// Single-Writer-Annahme (kein Locking), analog Single-Writer pro Shard.
class Hypertable {
 public:
  explicit Hypertable(int64_t chunk_interval_ms);

  void Insert(int64_t ts_ms, double value);
  void InsertRow(Row r);

  size_t TotalRows() const;
  size_t NumChunks() const { return chunks_.size(); }
  int64_t chunk_interval_ms() const { return chunk_interval_ms_; }
  const std::map<int64_t, Chunk>& chunks() const { return chunks_; }

  // Range-Scan / Aggregate mit Chunk-Pruning (nur ueberlappende Chunks).
  std::vector<Row> QueryRange(int64_t t0_ms, int64_t t1_ms) const;
  double SumRange(int64_t t0_ms, int64_t t1_ms) const;
  size_t CountRange(int64_t t0_ms, int64_t t1_ms) const;

  // Retention: droppt alle Chunks mit t_end <= cutoff_ms.
  // Gibt Anzahl gedroppter Chunks zurueck.
  size_t DropChunksBefore(int64_t cutoff_ms);

  bool AllChunksOrdered() const;

 private:
  int64_t chunk_interval_ms_;
  std::map<int64_t, Chunk> chunks_;
  size_t total_rows_ = 0;
};

// ---- Continuous Aggregate ---------------------------------------------------
// Inkrementelle Downsampling-Aggregate (Timescale Continuous-Agg-Analogie):
// Observe() pro Punkt O(log B), Rebuild() aus Hypertable zur Verifikation.
// Bucket: [bucket_start, bucket_start + bucket_interval).
struct AggBucket {
  double sum = 0.0;
  size_t count = 0;
  double Avg() const {
    return count == 0 ? 0.0 : sum / static_cast<double>(count);
  }
};

class ContinuousAgg {
 public:
  explicit ContinuousAgg(int64_t bucket_interval_ms);

  // Inkrementelles Update pro Punkt.
  void Observe(int64_t ts_ms, double value);

  // Vollstaendiger Neuaufbau aus Hypertable (Ground-Truth fuer Tests).
  void Rebuild(const Hypertable& table);
  // Alias (Timescale refresh_policy-Analogie, hier Voll-Refresh).
  void Refresh(const Hypertable& table) { Rebuild(table); }

  AggBucket Bucket(int64_t bucket_start_ms) const;
  bool HasBucket(int64_t bucket_start_ms) const;
  const std::map<int64_t, AggBucket>& buckets() const { return buckets_; }
  size_t NumBuckets() const { return buckets_.size(); }
  int64_t bucket_interval_ms() const { return bucket_interval_ms_; }

  double TotalSum() const;
  size_t TotalCount() const;
  double SumRange(int64_t t0_ms, int64_t t1_ms) const;

  void Clear() { buckets_.clear(); }

 private:
  int64_t bucket_interval_ms_;
  std::map<int64_t, AggBucket> buckets_;
};

// ---- ASOF-Join (Stub) -------------------------------------------------------
// Backward-ASOF: zu jeder linken Zeile die letzte rechte Zeile mit
// right.ts <= left.ts. Beide Seiten muessen zeit-sortiert sein (wird nicht
// sortiert, nur per Binaersuche ausgewertet). Real-Follow-up: Forward/Nearest,
// Toleranz, Interpolation, Partition-by-Keys.
struct AsofRow {
  Row left{};
  bool has_right = false;
  Row right{};
};

std::vector<AsofRow> AsofJoin(const std::vector<Row>& left,
                              const std::vector<Row>& right);

}  // namespace dbengine::ts
