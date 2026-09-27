// Prodsim: in-process Fault-Injector auf RaftGroup (Partition/Drop/HLC)
// plus Linearisierbarkeitsskizze. Framework-los, CTest-Name: prodsim.
// Kein Jepsen, kein multi-DC — Determinismus statt Sleep.

#include <atomic>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "dbengine/raft/shard.h"
#include "dbengine/txn/clock.h"

using dbengine::raft::RaftGroup;
using dbengine::txn::HlcTime;
using dbengine::txn::HybridLogicalClock;

namespace {

int g_failures = 0;

void Check(bool cond, const std::string& name) {
  if (cond) {
    std::cout << "PASS " << name << "\n";
  } else {
    std::cout << "FAIL " << name << "\n";
    ++g_failures;
  }
}

int CountLeaders(const RaftGroup& g) {
  int n = 0;
  for (int i = 0; i < RaftGroup::kGroupSize; ++i) {
    if (g.isAlive(i) && g.node(i).role == dbengine::raft::Role::Leader) ++n;
  }
  return n;
}

bool MapsEqual(const std::map<std::string, std::string>& a,
               const std::map<std::string, std::string>& b) {
  return a == b;
}

void TestDefaultMesh() {
  RaftGroup g(0, "", "");
  Check(g.can_send(0, 1) && g.can_send(1, 0) && g.can_send(0, 2),
        "mesh/default-fully-connected");
  Check(g.can_send(0, 0), "mesh/self-send");
  Check(g.leaderId() == 0, "mesh/initial-leader-0");
}

// A. Majority partition can commit.
void TestA_MajorityPartition() {
  RaftGroup g(10, "", "");
  Check(g.leaderId() == 0, "A/leader-0");
  g.isolate(2, 0);
  g.isolate(2, 1);
  Check(!g.can_send(0, 2) && !g.can_send(2, 0), "A/node2-cut");
  Check(g.can_send(0, 1) && g.can_send(1, 0), "A/majority-link-up");
  Check(g.append("put a 1") != 0, "A/majority-append");
  Check(g.get("a") == std::optional<std::string>("1"), "A/majority-read");
  Check(g.commitIndex() == 1, "A/majority-commit");
  Check(!g.follower_get(2, "a").has_value(), "A/minority-stale");
  Check(g.node(2).commit_index < g.commitIndex(), "A/minority-commit-behind");
  g.heal_all();
  Check(g.follower_get(2, "a") == std::optional<std::string>("1"),
        "A/heal-catchup-key");
  Check(MapsEqual(g.node(2).applied, g.node(g.leaderId()).applied),
        "A/applied-maps-equal");
  Check(CountLeaders(g) == 1, "A/one-leader-after-heal");
}

// B. Isolated leader cannot commit; majority elects; no forked committed history.
void TestB_SplitBrain() {
  RaftGroup g(11, "", "");
  Check(g.append("put k base") != 0, "B/seed");
  const std::uint64_t pre = g.commitIndex();
  const int old = g.leaderId();
  Check(old == 0, "B/old-leader-0");
  g.isolate_from_all(old);
  Check(g.append("put k isolated") == 0, "B/isolated-append-zero");
  Check(g.node(old).commit_index == pre, "B/isolated-commit-frozen");
  // Auto-Election: Heartbeat kommt bei den Followern nicht an.
  g.enable_auto_election(0);
  const std::uint64_t to1 = g.election_timeout_ms(1);
  const std::uint64_t to2 = g.election_timeout_ms(2);
  const std::uint64_t t = (to1 > to2 ? to1 : to2) + 1;
  int via_tick = g.tick(t);
  int nl = via_tick;
  if (nl < 0 || nl == old) nl = g.electLeader();
  Check(nl >= 0 && nl != old, "B/majority-new-leader");
  Check(g.append("put k majority") != 0, "B/majority-commit");
  Check(g.get("k") == std::optional<std::string>("majority"), "B/majority-value");
  auto iso = g.node(old).applied.find("k");
  if (iso != g.node(old).applied.end()) {
    Check(iso->second != "isolated", "B/isolated-did-not-commit-conflict");
  } else {
    Check(true, "B/isolated-did-not-commit-conflict");
  }
  Check(g.node(old).commit_index == pre, "B/isolated-still-frozen");
  g.heal_all();
  Check(CountLeaders(g) == 1, "B/one-leader-after-heal");
  Check(g.get("k") == std::optional<std::string>("majority"),
        "B/no-fork-after-heal");
  const int L = g.leaderId();
  Check(L >= 0, "B/leader-after-heal");
  if (L >= 0) {
    Check(MapsEqual(g.node(0).applied, g.node(L).applied) &&
              MapsEqual(g.node(1).applied, g.node(L).applied) &&
              MapsEqual(g.node(2).applied, g.node(L).applied),
          "B/catchup-all-applied");
  }
}

// C. Total split 1+1+1: no commit, no majority election.
void TestC_TotalSplit() {
  RaftGroup g(12, "", "");
  g.isolate_from_all(0);
  g.isolate_from_all(1);
  g.isolate_from_all(2);
  Check(g.electLeader() == -1, "C/no-election");
  Check(g.append("put x 1") == 0, "C/no-append");
  g.heal_all();
  Check(g.electLeader() >= 0, "C/election-after-heal");
  Check(g.append("put x 1") != 0, "C/append-after-heal");
  Check(g.get("x") == std::optional<std::string>("1"), "C/read-after-heal");
}

// D. Kill-9 stand-in: SaveLog, fresh group LoadLog, failover.
void TestD_Kill9Log() {
  namespace fs = std::filesystem;
  const fs::path path = fs::temp_directory_path() / "prodsim_d_raft.bin";
  fs::remove(path);
  fs::remove(fs::path(path.string() + ".tmp"));
  RaftGroup g(13, "", "");
  Check(g.append("put p 1") != 0, "D/put1");
  Check(g.append("put q 2") != 0, "D/put2");
  Check(g.SaveLog(path.string()), "D/save");
  g.killLeader();
  RaftGroup h(14, "", "");
  Check(h.LoadLog(path.string()), "D/load");
  Check(h.failover() >= 0, "D/failover");
  Check(h.get("p") == std::optional<std::string>("1") &&
            h.get("q") == std::optional<std::string>("2"),
        "D/committed-survives");
  fs::remove(path);
  fs::remove(fs::path(path.string() + ".tmp"));
}

// E. HLC clock skew: wall jumps + remote Update stay strictly monotonic.
static std::atomic<std::uint64_t> g_wall{10000};
static std::uint64_t ProdsimWall() { return g_wall.load(); }

void TestE_HlcSkew() {
  g_wall.store(10000);
  HybridLogicalClock clk(&ProdsimWall);
  HlcTime t1 = clk.Now();
  g_wall.store(1);  // Sprung rueckwaerts
  HlcTime t2 = clk.Now();
  Check(t2 > t1, "E/mono-after-backwards-jump");
  g_wall.store(50000);  // Sprung vorwaerts
  HlcTime t3 = clk.Now();
  Check(t3 > t2, "E/mono-after-forwards-jump");
  HlcTime remote{t3.wall_ms, t3.logical + 9};
  HlcTime t4 = clk.Update(remote);
  Check(t4 > t3, "E/mono-after-remote-update");
  HlcTime old{50, 99};
  HlcTime t5 = clk.Update(old);
  Check(t5 > t4, "E/mono-stale-remote");
}

// F. Deterministic drop: some appends fail; acked keys survive.
void TestF_Drop() {
  RaftGroup g(15, "", "");
  g.set_net_seed(42);
  std::vector<std::string> acked;
  Check(g.append("put k0 v0") != 0, "F/seed");
  acked.push_back("k0");
  g.set_drop_rate(0, 1, 70);
  g.set_drop_rate(0, 2, 70);
  int fails = 0, oks = 0;
  for (int i = 1; i <= 40; ++i) {
    std::string k = "k" + std::to_string(i);
    std::uint64_t idx = g.append("put " + k + " v");
    if (idx == 0) {
      ++fails;
    } else {
      ++oks;
      acked.push_back(k);
    }
  }
  Check(fails > 0, "F/some-appends-dropped");
  g.set_drop_rate(0, 1, 0);
  g.set_drop_rate(0, 2, 0);
  g.heal_all();
  if (g.leaderId() < 0) (void)g.electLeader();
  Check(g.append("put kz vz") != 0, "F/append-after-heal");
  acked.push_back("kz");
  bool all = true;
  for (const auto& k : acked) {
    if (!g.get(k).has_value()) all = false;
  }
  Check(all, "F/acked-prefix-survives");
  (void)oks;
}

// G. After heal, minority applied map equals majority.
void TestG_Catchup() {
  RaftGroup g(16, "", "");
  g.isolate_from_all(2);
  const int N = 8;
  for (int i = 0; i < N; ++i) {
    std::string cmd = "put g" + std::to_string(i) + " v" + std::to_string(i);
    Check(g.append(cmd) != 0, "G/majority-write");
  }
  Check(!g.follower_get(2, "g0").has_value(), "G/minority-behind");
  g.heal(2, 0);
  g.heal(2, 1);
  const int L = g.leaderId();
  Check(L >= 0, "G/leader");
  Check(MapsEqual(g.node(2).applied, g.node(L).applied), "G/maps-equal-after-heal");
  Check(g.is_caught_up(2), "G/caught-up");
}

// H. Sequential client: only append()!=0 is an ack; no dirty, no lost ack.
void TestH_Linearizability() {
  RaftGroup g(17, "", "");
  std::string model;
  auto write = [&](const std::string& v) {
    std::uint64_t idx = g.append("put k " + v);
    if (idx != 0) model = v;
    return idx;
  };
  auto read = [&](const std::string& name) {
    auto r = g.get("k");
    if (model.empty()) {
      Check(!r.has_value(), name);
    } else {
      Check(r == std::optional<std::string>(model), name);
    }
  };
  Check(write("a") != 0, "H/ack-a");
  read("H/read-a");
  Check(write("b") != 0, "H/ack-b");
  read("H/read-b");
  g.isolate_from_all(2);
  Check(write("c") != 0, "H/ack-c-majority");
  read("H/read-c");
  g.heal_all();  // 2 holt c; danach Majority {1,2} kann den alten Leader ersetzen
  read("H/read-after-minority-heal");
  const int old = g.leaderId();
  g.isolate_from_all(old);
  Check(write("dirty") == 0, "H/unacked-isolated");
  auto iso = g.node(old).applied.find("k");
  Check(iso == g.node(old).applied.end() || iso->second != "dirty",
        "H/no-dirty-apply");
  int nl = g.electLeader();
  Check(nl >= 0 && nl != old, "H/failover-majority");
  read("H/read-after-failover");
  Check(g.get("k") == std::optional<std::string>("c"), "H/no-lost-ack");
  g.heal_all();
  if (g.leaderId() < 0) (void)g.electLeader();
  read("H/read-after-heal");
  Check(CountLeaders(g) == 1, "H/one-leader");
}

}  // namespace

int main() {
  TestDefaultMesh();
  TestA_MajorityPartition();
  TestB_SplitBrain();
  TestC_TotalSplit();
  TestD_Kill9Log();
  TestE_HlcSkew();
  TestF_Drop();
  TestG_Catchup();
  TestH_Linearizability();

  if (g_failures == 0) {
    std::cout << "ALL PRODSIM TESTS PASSED\n";
    return 0;
  }
  std::cout << g_failures << " PRODSIM TEST(S) FAILED\n";
  return 1;
}
