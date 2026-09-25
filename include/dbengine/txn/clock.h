#pragma once

// s14-clock: TSO/HLC — Timestamp-Oracle + Hybrid-Logical-Clock, Commit-Wait,
// globale monoton Snapshot-Reads (F3.4, ohne Atomuhren).
//
// - HybridLogicalClock: wall_ms (physikalisch, ms) + logical (Zaehler).
//   Now() tickt lokal, Update(recv) mergt remote per HLC-Regel (max+1).
//   Thread-safe via Mutex -> totale Ordnung, strikt monoton.
// - TimestampOracle: PD-artiges Oracle, atomic fetch_add (TSO).
//   Update(seen) schiebt das Oracle per CAS nach vorn (z.B. nach HLC-Sync).
// - CommitWait: blockiert (Sleep-Poll) bis HLC > commit_ts (externe
//   Konsistenz, Spanner-artig, simuliert — kein TrueTime).
// - SnapshotIssuer: lock-freie RO-Snapshots global monoton, auch bei
//   rueckwaertsspringender Wall-Clock (Monotonie-Guard).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace dbengine::txn {

// ---- HLC-Zeitstempel ------------------------------------------------------
struct HlcTime {
  std::uint64_t wall_ms = 0;  // physikalische ms (system_clock)
  std::uint64_t logical = 0;  // logischer Zaehler bei gleicher wall_ms

  constexpr bool operator==(const HlcTime& o) const {
    return wall_ms == o.wall_ms && logical == o.logical;
  }
  constexpr bool operator!=(const HlcTime& o) const { return !(*this == o); }
  constexpr bool operator<(const HlcTime& o) const {
    if (wall_ms != o.wall_ms) return wall_ms < o.wall_ms;
    return logical < o.logical;
  }
  constexpr bool operator<=(const HlcTime& o) const { return !(o < *this); }
  constexpr bool operator>(const HlcTime& o) const { return o < *this; }
  constexpr bool operator>=(const HlcTime& o) const { return !(*this < o); }
};

// ---- Hybrid-Logical-Clock ---------------------------------------------------
class HybridLogicalClock {
 public:
  using WallFn = std::uint64_t (*)();  // ms seit Epoche, injizierbar f. Tests

  explicit HybridLogicalClock(WallFn wall_fn = &DefaultWallMs);

  HybridLogicalClock(const HybridLogicalClock&) = delete;
  HybridLogicalClock& operator=(const HybridLogicalClock&) = delete;

  // Lokaler Tick: pt = wall(); l' = max(last.wall, pt);
  // l' == last.wall ? {l', last.logical+1} : {l', 0}. Strikt monoton.
  HlcTime Now();

  // Remote-Merge (Kulkarni et al., vereinfacht):
  //   pt  = wall();
  //   l'  = max(last.wall, recv.wall, pt);
  //   l'==last && l'==recv ? max(logicals)+1
  //   l'==last             ? last.logical+1
  //   l'==recv             ? recv.logical+1
  //   sonst                  0
  HlcTime Update(HlcTime recv);

  HlcTime Current() const;

  void SetWallFn(WallFn fn);

  static std::uint64_t DefaultWallMs();

  // Packt HlcTime in ein uint64 zur Ordnung (48 bit wall + 16 bit logical,
  // logical saettigt bei 0xFFFF). Nur fuer Sortierung/Debug, kein Ersatz
  // fuer vollen Vergleich.
  static std::uint64_t Pack(HlcTime t);
  static HlcTime Unpack(std::uint64_t packed);

 private:
  WallFn wall_;
  mutable std::mutex mu_;
  HlcTime last_{0, 0};
};

// ---- Timestamp-Oracle (TSO/PD-Modell) ---------------------------------------
class TimestampOracle {
 public:
  explicit TimestampOracle(std::uint64_t start = 1);

  TimestampOracle(const TimestampOracle&) = delete;
  TimestampOracle& operator=(const TimestampOracle&) = delete;

  // Naechsten Timestamp ziehen (atomic fetch_add, strikt monoton, eindeutig).
  std::uint64_t Next() { return next_.fetch_add(1, std::memory_order_acq_rel); }
  std::uint64_t Current() const { return next_.load(std::memory_order_acquire); }

  // Oracle nach vorn schieben falls `seen` neuer ist (CAS-Schleife).
  // Wird z.B. nach HLC-Sync / Leader-Wechsel benutzt.
  void Update(std::uint64_t seen);

 private:
  std::atomic<std::uint64_t> next_;
};

// ---- Commit-Wait (simuliert, Spanner-artig) ---------------------------------
// Blockiert bis clock.Now() > commit_ts. Gibt true zurueck wenn die Bedingung
// vor Ablauf von `timeout` erreicht wurde, sonst false. Mit
// timeout == infinite (Default) wird unbegrenzt gewartet.
inline const std::chrono::milliseconds kCommitWaitInfinite =
    std::chrono::milliseconds::max();

bool CommitWaitUntilAfter(const HlcTime& commit_ts, HybridLogicalClock& clock,
                          std::chrono::milliseconds timeout =
                              kCommitWaitInfinite);

// Bequemer void-Wrapper (wartet unbegrenzt).
inline void CommitWait(const HlcTime& commit_ts, HybridLogicalClock& clock) {
  (void)CommitWaitUntilAfter(commit_ts, clock, kCommitWaitInfinite);
}

// ---- Monotoner Snapshot-Issuer ----------------------------------------------
// Globale Snapshot-Reads bleiben monoton, selbst wenn die Wall-Clock
// rueckwaerts springt (Guard gegen last_). HLC-Pfad nutzt Update/Now,
// TSO-Pfad nutzt Oracle + Guard.
class SnapshotIssuer {
 public:
  explicit SnapshotIssuer(HybridLogicalClock* hlc = nullptr,
                          TimestampOracle* tso = nullptr);

  // Frischer HLC-Snapshot (Now + Monotonie-Guard). Ohne HLC faellt er auf
  // einen lokalen Zaehler zurueck (wall=0, logical strikt steigend).
  HlcTime SnapshotHlc();

  // Frischer TSO-Snapshot (Oracle.Next + Monotonie-Guard).
  // Ohne Oracle faellt er auf einen lokalen Zaehler zurueck.
  std::uint64_t SnapshotTso();

  HlcTime LastHlc() const;
  std::uint64_t LastTso() const;

 private:
  HybridLogicalClock* hlc_;
  TimestampOracle* tso_;
  mutable std::mutex mu_;
  HlcTime last_hlc_{0, 0};
  std::uint64_t last_tso_ = 0;
  std::uint64_t local_fallback_ = 1;  // unter mu_, wenn kein Backend gesetzt
};

}  // namespace dbengine::txn
