// s17-ts: Hypertable-Chunks, Retention-Drop, Continuous-Agg, ASOF-Join.
// Siehe include/dbengine/ts/chunks.h fuer API-Doku.

#include "dbengine/ts/chunks.h"

#include <algorithm>
#include <stdexcept>

namespace dbengine::ts {

// ---- Chunk -----------------------------------------------------------------

Chunk::Chunk(int64_t t_start, int64_t interval_ms)
    : t_start_(t_start), t_end_(t_start + interval_ms) {
  if (interval_ms <= 0)
    throw std::invalid_argument("Chunk: interval_ms muss > 0 sein");
}

void Chunk::Insert(Row r) {
  if (!Contains(r.ts_ms))
    throw std::out_of_range("Chunk::Insert: ts ausserhalb [t_start,t_end)");
  // Fast-Path: In-Order-Append (heisser Ingest-Pfad, O(1)).
  if (rows_.empty() || r.ts_ms >= rows_.back().ts_ms) {
    rows_.push_back(r);
    return;
  }
  // Out-of-Order: sortiert einfügen (lower_bound, stabil).
  auto it = std::lower_bound(
      rows_.begin(), rows_.end(), r.ts_ms,
      [](const Row& row, int64_t ts) { return row.ts_ms < ts; });
  rows_.insert(it, r);
}

std::vector<Row> Chunk::ScanRange(int64_t t0_ms, int64_t t1_ms) const {
  std::vector<Row> out;
  if (rows_.empty() || t1_ms <= t0_ms) return out;
  if (!Overlaps(t0_ms, t1_ms)) return out;
  // Binaersuche auf sortierten rows (untere/obere Schranke).
  auto lo = std::lower_bound(
      rows_.begin(), rows_.end(), t0_ms,
      [](const Row& row, int64_t ts) { return row.ts_ms < ts; });
  auto hi = std::lower_bound(
      rows_.begin(), rows_.end(), t1_ms,
      [](const Row& row, int64_t ts) { return row.ts_ms < ts; });
  out.insert(out.end(), lo, hi);
  return out;
}

double Chunk::SumRange(int64_t t0_ms, int64_t t1_ms) const {
  if (rows_.empty() || t1_ms <= t0_ms) return 0.0;
  if (!Overlaps(t0_ms, t1_ms)) return 0.0;
  // Voll abgedeckt + Chunk-Grenzen innerhalb Query -> kein Vergleich noetig.
  if (t0_ms <= t_start_ && t1_ms >= t_end_) {
    double s = 0.0;
    for (const auto& r : rows_) s += r.value;
    return s;
  }
  double s = 0.0;
  auto lo = std::lower_bound(
      rows_.begin(), rows_.end(), t0_ms,
      [](const Row& row, int64_t ts) { return row.ts_ms < ts; });
  auto hi = std::lower_bound(
      rows_.begin(), rows_.end(), t1_ms,
      [](const Row& row, int64_t ts) { return row.ts_ms < ts; });
  for (auto it = lo; it != hi; ++it) s += it->value;
  return s;
}

size_t Chunk::CountRange(int64_t t0_ms, int64_t t1_ms) const {
  if (rows_.empty() || t1_ms <= t0_ms) return 0;
  if (!Overlaps(t0_ms, t1_ms)) return 0;
  auto lo = std::lower_bound(
      rows_.begin(), rows_.end(), t0_ms,
      [](const Row& row, int64_t ts) { return row.ts_ms < ts; });
  auto hi = std::lower_bound(
      rows_.begin(), rows_.end(), t1_ms,
      [](const Row& row, int64_t ts) { return row.ts_ms < ts; });
  return static_cast<size_t>(hi - lo);
}

bool Chunk::IsOrdered() const {
  for (size_t i = 1; i < rows_.size(); ++i)
    if (rows_[i].ts_ms < rows_[i - 1].ts_ms) return false;
  return true;
}

// ---- Hypertable --------------------------------------------------------------

Hypertable::Hypertable(int64_t chunk_interval_ms)
    : chunk_interval_ms_(chunk_interval_ms) {
  if (chunk_interval_ms <= 0)
    throw std::invalid_argument("Hypertable: chunk_interval_ms muss > 0 sein");
}

void Hypertable::Insert(int64_t ts_ms, double value) {
  InsertRow(Row{ts_ms, value});
}

void Hypertable::InsertRow(Row r) {
  const int64_t start = FloorBucket(r.ts_ms, chunk_interval_ms_);
  auto it = chunks_.find(start);
  if (it == chunks_.end()) {
    it = chunks_.emplace(start, Chunk(start, chunk_interval_ms_)).first;
  }
  it->second.Insert(r);
  ++total_rows_;
}

size_t Hypertable::TotalRows() const { return total_rows_; }

std::vector<Row> Hypertable::QueryRange(int64_t t0_ms, int64_t t1_ms) const {
  std::vector<Row> out;
  if (t1_ms <= t0_ms) return out;
  // Nur ueberlappende Chunks anfassen (Chunk-Pruning). lower_bound auf den
  // ersten Chunk, der t0 enthalten koennte (inkl. Vorgaenger wegen Overlap).
  const int64_t first = FloorBucket(t0_ms, chunk_interval_ms_);
  auto it = chunks_.lower_bound(first);
  // Vorgaenger-Chunk kann t0 noch ueberlappen (nur wenn t0 exakt auf Grenze
  // liegt nicht, Overlaps prueft das).
  if (it != chunks_.begin()) {
    auto prev = std::prev(it);
    if (prev->second.Overlaps(t0_ms, t1_ms)) it = prev;
  }
  for (; it != chunks_.end(); ++it) {
    if (it->first >= t1_ms) break;  // sortiert -> Rest liegt rechts
    auto part = it->second.ScanRange(t0_ms, t1_ms);
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

double Hypertable::SumRange(int64_t t0_ms, int64_t t1_ms) const {
  if (t1_ms <= t0_ms) return 0.0;
  double s = 0.0;
  const int64_t first = FloorBucket(t0_ms, chunk_interval_ms_);
  auto it = chunks_.lower_bound(first);
  if (it != chunks_.begin()) {
    auto prev = std::prev(it);
    if (prev->second.Overlaps(t0_ms, t1_ms)) it = prev;
  }
  for (; it != chunks_.end(); ++it) {
    if (it->first >= t1_ms) break;
    s += it->second.SumRange(t0_ms, t1_ms);
  }
  return s;
}

size_t Hypertable::CountRange(int64_t t0_ms, int64_t t1_ms) const {
  if (t1_ms <= t0_ms) return 0;
  size_t n = 0;
  const int64_t first = FloorBucket(t0_ms, chunk_interval_ms_);
  auto it = chunks_.lower_bound(first);
  if (it != chunks_.begin()) {
    auto prev = std::prev(it);
    if (prev->second.Overlaps(t0_ms, t1_ms)) it = prev;
  }
  for (; it != chunks_.end(); ++it) {
    if (it->first >= t1_ms) break;
    n += it->second.CountRange(t0_ms, t1_ms);
  }
  return n;
}

size_t Hypertable::DropChunksBefore(int64_t cutoff_ms) {
  // Drop-Bedingung: chunk.t_end <= cutoff (ganzer Chunk vor Retention-Fenster).
  size_t dropped = 0;
  auto it = chunks_.begin();
  while (it != chunks_.end() && it->second.t_end() <= cutoff_ms) {
    total_rows_ -= it->second.Size();
    it = chunks_.erase(it);
    ++dropped;
  }
  return dropped;
}

bool Hypertable::AllChunksOrdered() const {
  for (const auto& [k, c] : chunks_)
    if (!c.IsOrdered()) return false;
  return true;
}

// ---- ContinuousAgg -----------------------------------------------------------

ContinuousAgg::ContinuousAgg(int64_t bucket_interval_ms)
    : bucket_interval_ms_(bucket_interval_ms) {
  if (bucket_interval_ms <= 0)
    throw std::invalid_argument(
        "ContinuousAgg: bucket_interval_ms muss > 0 sein");
}

void ContinuousAgg::Observe(int64_t ts_ms, double value) {
  const int64_t b = FloorBucket(ts_ms, bucket_interval_ms_);
  AggBucket& agg = buckets_[b];  // default {0,0}
  agg.sum += value;
  agg.count += 1;
}

void ContinuousAgg::Rebuild(const Hypertable& table) {
  buckets_.clear();
  for (const auto& [ckey, chunk] : table.chunks()) {
    for (const auto& r : chunk.rows()) Observe(r.ts_ms, r.value);
  }
}

AggBucket ContinuousAgg::Bucket(int64_t bucket_start_ms) const {
  auto it = buckets_.find(bucket_start_ms);
  if (it == buckets_.end()) return AggBucket{};
  return it->second;
}

bool ContinuousAgg::HasBucket(int64_t bucket_start_ms) const {
  return buckets_.find(bucket_start_ms) != buckets_.end();
}

double ContinuousAgg::TotalSum() const {
  double s = 0.0;
  for (const auto& [k, b] : buckets_) s += b.sum;
  return s;
}

size_t ContinuousAgg::TotalCount() const {
  size_t n = 0;
  for (const auto& [k, b] : buckets_) n += b.count;
  return n;
}

double ContinuousAgg::SumRange(int64_t t0_ms, int64_t t1_ms) const {
  if (t1_ms <= t0_ms) return 0.0;
  // Bucket-granulare Summe: nur voll abgedeckte Buckets exakt, Rand-Buckets
  // werden pro-rata NICHT aufgeteilt (Downsampling-Semantik: Bucket gewinnt).
  // Fuer exakte Zeilen-Summen Hypertable::SumRange nutzen.
  // Hier: Summe aller Buckets, deren Start in [t0, t1) liegt.
  double s = 0.0;
  auto it = buckets_.lower_bound(FloorBucket(t0_ms, bucket_interval_ms_));
  for (; it != buckets_.end() && it->first < t1_ms; ++it) s += it->second.sum;
  return s;
}

// ---- AsofJoin (Stub) ----------------------------------------------------------

std::vector<AsofRow> AsofJoin(const std::vector<Row>& left,
                              const std::vector<Row>& right) {
  std::vector<AsofRow> out;
  out.reserve(left.size());
  for (const auto& l : left) {
    AsofRow row;
    row.left = l;
    // upper_bound(right.ts <= l.ts): erster > l.ts, dann einen zurueck.
    auto it = std::upper_bound(
        right.begin(), right.end(), l.ts_ms,
        [](int64_t ts, const Row& r) { return ts < r.ts_ms; });
    // Achtung: upper_bound mit heterogenem Vergleich oben ist
    // value-first; korrekt: upper_bound(right.begin, right.end, l.ts,
    // comparator). Wir nutzen manuell lower_bound fuer Klarheit:
    auto hi = std::lower_bound(
        right.begin(), right.end(), l.ts_ms,
        [](const Row& r, int64_t ts) { return r.ts_ms < ts; });
    // hi zeigt auf erstes right.ts >= l.ts; wir brauchen <= l.ts:
    // falls hi auf exakten Treffer zeigt, ist das der Kandidat (letzter mit
    // gleichem ts: einen weiter nach vorn bis Grenze).
    const Row* cand = nullptr;
    if (hi != right.end() && hi->ts_ms == l.ts_ms) {
      // Letzte Zeile mit gleichem ts nehmen (stabile Backward-Semantik).
      auto j = hi;
      while (std::next(j) != right.end() && std::next(j)->ts_ms == l.ts_ms)
        ++j;
      cand = &(*j);
    } else if (hi != right.begin()) {
      cand = &(*std::prev(hi));
    }
    // 'it' oben ist ungenutzt (historischer Rest) -> stilllegen.
    (void)it;
    if (cand != nullptr) {
      row.has_right = true;
      row.right = *cand;
    }
    out.push_back(row);
  }
  return out;
}

}  // namespace dbengine::ts
