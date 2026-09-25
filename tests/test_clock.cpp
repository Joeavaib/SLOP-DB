// s14-clock Tests: TSO-Monotonie ueber Threads, HLC merge max+1,
// snapshot reads monoton, Commit-Wait. Framework-los (CTest-Name: clock).

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <algorithm>

#include "dbengine/txn/clock.h"

using dbengine::txn::CommitWaitUntilAfter;
using dbengine::txn::HlcTime;
using dbengine::txn::HybridLogicalClock;
using dbengine::txn::SnapshotIssuer;
using dbengine::txn::TimestampOracle;

static int g_pass = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __func__ << ":" << __LINE__ << ": " #cond "\n"; \
      return false;                                                          \
    }                                                                        \
  } while (0)

// ---- Manuelle Wall-Clock fuer deterministische HLC-Tests ---------------------
static std::atomic<std::uint64_t> g_manual_wall{1000};
static std::uint64_t ManualWall() { return g_manual_wall.load(); }

static bool t_hlc_merge_max_plus_one() {
  g_manual_wall.store(1000);
  HybridLogicalClock clk(&ManualWall);
  HlcTime t0 = clk.Now();  // {1000,0}
  CHECK(t0.wall_ms == 1000 && t0.logical == 0);

  // Gleicher wall, groesseres logical remote -> max+1
  HlcTime m1 = clk.Update(HlcTime{1000, 7});
  CHECK(m1.wall_ms == 1000 && m1.logical == 8);

  // Remote wall groesser -> uebernehmen + recv.logical+1
  HlcTime m2 = clk.Update(HlcTime{2000, 0});
  CHECK(m2.wall_ms == 2000 && m2.logical == 1);

  // Physikalische Wall groesser als beide -> {pt, 0}
  g_manual_wall.store(3000);
  HlcTime m3 = clk.Update(HlcTime{2000, 5});
  CHECK(m3.wall_ms == 3000 && m3.logical == 0);

  // Altes remote darf Uhr nicht zurueckdrehen (monoton)
  HlcTime before = clk.Current();
  HlcTime m4 = clk.Update(HlcTime{500, 99});
  CHECK(m4 > before);
  CHECK(m4.wall_ms == 3000);

  // Gleicher Tick beidseitig -> max(logicals)+1
  g_manual_wall.store(3000);
  // clk steht bei {3000,1} nach m4; remote {3000,1} -> {3000,2}
  HlcTime m5 = clk.Update(HlcTime{3000, 1});
  CHECK(m5.wall_ms == 3000);
  CHECK(m5.logical == 2);
  return true;
}

static bool t_hlc_local_monotonic() {
  g_manual_wall.store(5000);
  HybridLogicalClock clk(&ManualWall);
  HlcTime prev{0, 0};
  for (int i = 0; i < 1000; ++i) {
    HlcTime cur = clk.Now();  // wall steht -> logical muss steigen
    CHECK(cur > prev);
    prev = cur;
  }
  CHECK(prev.wall_ms == 5000 && prev.logical == 999);
  // Wall-Sprung rueckwaerts darf Monotonie nicht brechen
  g_manual_wall.store(100);
  HlcTime after = clk.Now();
  CHECK(after > prev);
  return true;
}

static bool t_tso_monotonic_threads() {
  TimestampOracle oracle(1);
  constexpr int kThreads = 8, kPer = 2000;
  std::vector<std::uint64_t> all;
  all.reserve(static_cast<std::size_t>(kThreads * kPer));
  std::mutex mu;
  std::vector<std::thread> th;
  for (int t = 0; t < kThreads; ++t) {
    th.emplace_back([&] {
      std::vector<std::uint64_t> local;
      local.reserve(kPer);
      for (int i = 0; i < kPer; ++i) local.push_back(oracle.Next());
      std::lock_guard<std::mutex> g(mu);
      all.insert(all.end(), local.begin(), local.end());
    });
  }
  for (auto& t : th) t.join();
  CHECK(all.size() == static_cast<std::size_t>(kThreads * kPer));
  std::sort(all.begin(), all.end());
  for (std::size_t i = 0; i < all.size(); ++i) {
    CHECK(all[i] == i + 1);  // lueckenlos, eindeutig, monoton
  }
  return true;
}

static bool t_hlc_monotonic_threads() {
  HybridLogicalClock clk;  // echte Wall-Clock
  constexpr int kThreads = 8, kPer = 1000;
  std::vector<HlcTime> all;
  all.reserve(static_cast<std::size_t>(kThreads * kPer));
  std::mutex mu;
  std::vector<std::thread> th;
  for (int t = 0; t < kThreads; ++t) {
    th.emplace_back([&] {
      std::vector<HlcTime> local;
      local.reserve(kPer);
      for (int i = 0; i < kPer; ++i) local.push_back(clk.Now());
      std::lock_guard<std::mutex> g(mu);
      all.insert(all.end(), local.begin(), local.end());
    });
  }
  for (auto& t : th) t.join();
  CHECK(all.size() == static_cast<std::size_t>(kThreads * kPer));
  std::sort(all.begin(), all.end(),
            [](const HlcTime& a, const HlcTime& b) { return a < b; });
  for (std::size_t i = 1; i < all.size(); ++i) {
    CHECK(all[i - 1] < all[i]);  // strikt monoton, keine Duplikate
  }
  return true;
}

static bool t_snapshot_reads_monotonic() {
  HybridLogicalClock clk;
  TimestampOracle oracle(1);
  SnapshotIssuer issuer(&clk, &oracle);

  // Sequentiell: HLC- und TSO-Snapshots muessen monoton steigen.
  HlcTime prev_h{0, 0};
  std::uint64_t prev_t = 0;
  for (int i = 0; i < 1000; ++i) {
    HlcTime h = issuer.SnapshotHlc();
    std::uint64_t t = issuer.SnapshotTso();
    CHECK(h > prev_h);
    CHECK(t > prev_t);
    prev_h = h;
    prev_t = t;
  }

  // Wall-Sprung rueckwaerts: Issuer-Guard haelt Monotonie trotzdem.
  g_manual_wall.store(1'000'000);
  HybridLogicalClock clk2(&ManualWall);
  SnapshotIssuer iss2(&clk2, nullptr);
  HlcTime a = iss2.SnapshotHlc();
  g_manual_wall.store(1);  // Uhr springt zurueck
  HlcTime b = iss2.SnapshotHlc();
  HlcTime c = iss2.SnapshotHlc();
  CHECK(b > a);
  CHECK(c > b);

  // Parallel: sortierte Snapshots global monoton.
  std::vector<HlcTime> hs;
  std::vector<std::uint64_t> ts;
  std::mutex mu;
  std::vector<std::thread> th;
  for (int t = 0; t < 4; ++t) {
    th.emplace_back([&] {
      std::vector<HlcTime> lh;
      std::vector<std::uint64_t> lt;
      for (int i = 0; i < 500; ++i) {
        lh.push_back(issuer.SnapshotHlc());
        lt.push_back(issuer.SnapshotTso());
      }
      std::lock_guard<std::mutex> g(mu);
      hs.insert(hs.end(), lh.begin(), lh.end());
      ts.insert(ts.end(), lt.begin(), lt.end());
    });
  }
  for (auto& t : th) t.join();
  std::sort(hs.begin(), hs.end(),
            [](const HlcTime& x, const HlcTime& y) { return x < y; });
  std::sort(ts.begin(), ts.end());
  for (std::size_t i = 1; i < hs.size(); ++i) CHECK(hs[i - 1] < hs[i]);
  for (std::size_t i = 1; i < ts.size(); ++i) CHECK(ts[i - 1] < ts[i]);
  return true;
}

static bool t_commit_wait() {
  HybridLogicalClock clk;  // echte Wall
  HlcTime now = clk.Now();
  // Commit in naher Zukunft (+60ms): Wait muss blockieren bis HLC drueber.
  HlcTime commit{now.wall_ms + 60, 0};
  auto t0 = std::chrono::steady_clock::now();
  bool ok = CommitWaitUntilAfter(commit, clk, std::chrono::seconds(5));
  auto dt = std::chrono::steady_clock::now() - t0;
  CHECK(ok);
  CHECK(clk.Current() > commit);
  // Muss tatsaechlich gewartet haben (mind. ~50ms, tolerant nach unten).
  CHECK(dt >= std::chrono::milliseconds(40));

  // Commit in Vergangenheit: sofort true, kein langes Blockieren.
  HlcTime past{1, 0};
  t0 = std::chrono::steady_clock::now();
  CHECK(CommitWaitUntilAfter(past, clk, std::chrono::seconds(5)));
  dt = std::chrono::steady_clock::now() - t0;
  CHECK(dt < std::chrono::milliseconds(50));

  // Timeout-Pfad: weit entfernter Commit + kurzes Timeout -> false.
  HlcTime far{now.wall_ms + 600'000, 0};
  CHECK(!CommitWaitUntilAfter(far, clk, std::chrono::milliseconds(20)));
  return true;
}

static bool t_oracle_update_forward() {
  TimestampOracle oracle(100);
  CHECK(oracle.Next() == 100);
  oracle.Update(500);  // nach vorn schieben
  CHECK(oracle.Current() == 501);
  CHECK(oracle.Next() == 501);
  oracle.Update(10);  // kleiner Wert darf nicht zurueckdrehen
  CHECK(oracle.Next() == 502);
  return true;
}

static bool t_pack_order_preserving() {
  // Pack soll Ordnung fuer typische Werte erhalten.
  HlcTime a{1000, 0}, b{1000, 1}, c{1001, 0};
  CHECK(HybridLogicalClock::Pack(a) < HybridLogicalClock::Pack(b));
  CHECK(HybridLogicalClock::Pack(b) < HybridLogicalClock::Pack(c));
  auto rt = HybridLogicalClock::Unpack(HybridLogicalClock::Pack(b));
  CHECK(rt == b);
  return true;
}

int main() {
  using Fn = bool (*)();
  std::vector<std::pair<std::string, Fn>> cases = {
      {"hlc_merge_max_plus_one", t_hlc_merge_max_plus_one},
      {"hlc_local_monotonic", t_hlc_local_monotonic},
      {"tso_monotonic_threads", t_tso_monotonic_threads},
      {"hlc_monotonic_threads", t_hlc_monotonic_threads},
      {"snapshot_reads_monotonic", t_snapshot_reads_monotonic},
      {"commit_wait", t_commit_wait},
      {"oracle_update_forward", t_oracle_update_forward},
      {"pack_order_preserving", t_pack_order_preserving},
  };
  int fail = 0;
  for (auto& [name, fn] : cases) {
    bool ok = false;
    try {
      ok = fn();
    } catch (const std::exception& e) {
      std::cerr << "EXC " << name << ": " << e.what() << "\n";
      ok = false;
    } catch (...) {
      std::cerr << "EXC " << name << ": unknown\n";
      ok = false;
    }
    std::cout << (ok ? "PASS " : "FAIL ") << name << "\n";
    if (ok)
      ++g_pass;
    else
      ++fail;
  }
  std::cout << g_pass << "/" << cases.size() << " clock tests passed\n";
  return fail == 0 ? 0 : 1;
}
