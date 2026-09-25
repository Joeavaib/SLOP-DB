#include "dbengine/txn/clock.h"

#include <chrono>
#include <thread>

namespace dbengine::txn {

HybridLogicalClock::HybridLogicalClock(WallFn wall_fn)
    : wall_(wall_fn ? wall_fn : &DefaultWallMs) {}

std::uint64_t HybridLogicalClock::DefaultWallMs() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

HlcTime HybridLogicalClock::Now() {
  const std::uint64_t pt = wall_();
  std::lock_guard<std::mutex> g(mu_);
  if (last_.wall_ms >= pt) {
    // Wall steht oder laeuft rueckwaerts -> logical bump, strikt monoton.
    ++last_.logical;
  } else {
    last_.wall_ms = pt;
    last_.logical = 0;
  }
  return last_;
}

HlcTime HybridLogicalClock::Update(HlcTime recv) {
  const std::uint64_t pt = wall_();
  std::lock_guard<std::mutex> g(mu_);
  std::uint64_t l = last_.wall_ms;
  if (recv.wall_ms > l) l = recv.wall_ms;
  if (pt > l) l = pt;

  std::uint64_t c = 0;
  const bool same_last = (l == last_.wall_ms);
  const bool same_recv = (l == recv.wall_ms);
  if (same_last && same_recv) {
    c = (last_.logical > recv.logical ? last_.logical : recv.logical) + 1;
  } else if (same_last) {
    c = last_.logical + 1;
  } else if (same_recv) {
    c = recv.logical + 1;
  } else {
    c = 0;
  }
  last_.wall_ms = l;
  last_.logical = c;
  return last_;
}

HlcTime HybridLogicalClock::Current() const {
  std::lock_guard<std::mutex> g(mu_);
  return last_;
}

void HybridLogicalClock::SetWallFn(WallFn fn) {
  std::lock_guard<std::mutex> g(mu_);
  wall_ = fn ? fn : &DefaultWallMs;
}

std::uint64_t HybridLogicalClock::Pack(HlcTime t) {
  constexpr std::uint64_t kWallBits = 48;
  constexpr std::uint64_t kLogMask = 0xFFFFu;
  std::uint64_t w = t.wall_ms;
  const std::uint64_t max_wall = (kWallBits == 64 ? ~0ull : ((1ull << kWallBits) - 1ull));
  if (w > max_wall) w = max_wall;
  std::uint64_t l = t.logical > kLogMask ? kLogMask : t.logical;
  return (w << 16) | l;
}

HlcTime HybridLogicalClock::Unpack(std::uint64_t packed) {
  HlcTime t;
  t.wall_ms = packed >> 16;
  t.logical = packed & 0xFFFFu;
  return t;
}

// ---- TimestampOracle ---------------------------------------------------------

TimestampOracle::TimestampOracle(std::uint64_t start) : next_(start) {}

void TimestampOracle::Update(std::uint64_t seen) {
  // Schiebe next_ auf > seen (CAS-Schleife, lock-frei).
  std::uint64_t cur = next_.load(std::memory_order_acquire);
  while (seen >= cur) {
    const std::uint64_t want = seen + 1;
    if (next_.compare_exchange_weak(cur, want, std::memory_order_acq_rel,
                                    std::memory_order_acquire)) {
      break;
    }
    // cur wurde aktualisiert -> erneut pruefen
  }
}

// ---- Commit-Wait --------------------------------------------------------------

bool CommitWaitUntilAfter(const HlcTime& commit_ts, HybridLogicalClock& clock,
                          std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const bool infinite = (timeout == kCommitWaitInfinite);
  for (;;) {
    HlcTime cur = clock.Now();
    if (cur > commit_ts) return true;
    if (!infinite && std::chrono::steady_clock::now() >= deadline) {
      // Letzter Check nach Deadline (Wall kann inzwischen vorangekommen sein).
      return clock.Current() > commit_ts;
    }
    // Kurzer Sleep statt Busy-Spin (simuliert TrueTime-Wait).
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// ---- SnapshotIssuer ------------------------------------------------------------

SnapshotIssuer::SnapshotIssuer(HybridLogicalClock* hlc, TimestampOracle* tso)
    : hlc_(hlc), tso_(tso) {}

HlcTime SnapshotIssuer::SnapshotHlc() {
  HlcTime fresh{0, 0};
  if (hlc_ != nullptr) {
    fresh = hlc_->Now();
  } else {
    std::lock_guard<std::mutex> g(mu_);
    fresh.wall_ms = 0;
    fresh.logical = local_fallback_++;
    if (fresh <= last_hlc_) {
      fresh.wall_ms = last_hlc_.wall_ms;
      fresh.logical = last_hlc_.logical + 1;
    }
    last_hlc_ = fresh;
    return fresh;
  }
  std::lock_guard<std::mutex> g(mu_);
  if (fresh <= last_hlc_) {
    // Wall lief rueckwaerts oder Konkurrenz zog gleichen Tick:
    // Guard stellt globale Monotonie sicher.
    fresh.wall_ms = last_hlc_.wall_ms;
    fresh.logical = last_hlc_.logical + 1;
  }
  last_hlc_ = fresh;
  return fresh;
}

std::uint64_t SnapshotIssuer::SnapshotTso() {
  std::uint64_t fresh = 0;
  if (tso_ != nullptr) {
    fresh = tso_->Next();
  } else {
    std::lock_guard<std::mutex> g(mu_);
    fresh = local_fallback_++;
  }
  std::lock_guard<std::mutex> g(mu_);
  if (fresh <= last_tso_) fresh = last_tso_ + 1;
  last_tso_ = fresh;
  // Falls ein Oracle dahintersteht und der Guard erhoehen musste (kann bei
  // manuellem Zuruecksetzen passieren), Oracle nach vorn schieben.
  if (tso_ != nullptr && fresh >= tso_->Current()) tso_->Update(fresh);
  return fresh;
}

HlcTime SnapshotIssuer::LastHlc() const {
  std::lock_guard<std::mutex> g(mu_);
  return last_hlc_;
}

std::uint64_t SnapshotIssuer::LastTso() const {
  std::lock_guard<std::mutex> g(mu_);
  return last_tso_;
}

}  // namespace dbengine::txn
