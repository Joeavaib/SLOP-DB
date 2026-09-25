// s13-raft Tests: Shard-Range, Election, Replikation, Failover <100ms sim.
// Framework-los (assert-light + cout), CTest-Name: raft (ctest -R raft).

#include <chrono>
#include <iostream>
#include <string>

#include "dbengine/raft/shard.h"

using dbengine::raft::RaftGroup;
using dbengine::raft::Shard;

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

void TestShardRange() {
  Shard s(7, "a", "m");
  Check(s.id() == 7, "shard/id");
  Check(s.rangeStart() == "a" && s.rangeEnd() == "m", "shard/range");
  Check(s.contains("a") && s.contains("g") && !s.contains("m") &&
            !s.contains("z") && !s.contains("`"),
        "shard/contains-half-open");
  Shard open(8, "m", "");  // letztes Tablet: end offen
  Check(open.contains("m") && open.contains("zzz"), "shard/open-end");
  Check(s.targetBytes() == 96ULL * 1024ULL * 1024ULL, "shard/96mb-logisch");
}

void TestElection() {
  RaftGroup g(1, "a", "m");
  int leader = g.leaderId();
  Check(leader >= 0 && leader < 3, "elect/initial-leader");
  Check(g.term() >= 1, "elect/term-bumped");
  Check(g.aliveCount() == 3, "elect/all-alive");
  int again = g.electLeader();
  Check(again >= 0 && g.term() >= 2, "elect/reelect-term-mono");
}

void TestReplication() {
  RaftGroup g(2, "", "");
  Check(g.append("put k1 v1") == 1, "repl/idx1");
  Check(g.append("put k2 v2") == 2, "repl/idx2");
  Check(g.append("del k1") == 3, "repl/idx3");
  Check(g.commitIndex() == 3, "repl/commit-index");
  Check(g.logSize() == 3, "repl/log-size");
  // Alle lebenden Knoten haben identisches Log (Sim-Replikation).
  bool same = true;
  for (int i = 1; i < 3; ++i) {
    const auto& a = g.node(0).log;
    const auto& b = g.node(i).log;
    if (a.size() != b.size()) {
      same = false;
      break;
    }
    for (std::size_t j = 0; j < a.size(); ++j) {
      if (a[j].index != b[j].index || a[j].term != b[j].term ||
          a[j].command != b[j].command) {
        same = false;
        break;
      }
    }
  }
  Check(same, "repl/logs-identical");
  // Apply: State-Machine auf allen Knoten.
  Check(g.get("k2") == std::optional<std::string>("v2"), "repl/apply-put");
  Check(!g.get("k1").has_value(), "repl/apply-del");
  Check(g.node(1).applied == g.node(0).applied, "repl/apply-all-nodes");
}

void TestFailover100ms() {
  RaftGroup g(3, "", "");
  g.append("put a 1");
  g.append("put b 2");
  int old_leader = g.leaderId();
  auto t0 = std::chrono::steady_clock::now();
  g.killLeader();
  Check(g.leaderId() == -1, "failover/no-leader-after-kill");
  int fresh = g.failover();
  auto t1 = std::chrono::steady_clock::now();
  double ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 -
                                                                            t0)
          .count();
  std::cout << "FAILOVER sim-time: " << ms << " ms (old=" << old_leader
            << " new=" << fresh << ")\n";
  Check(fresh >= 0 && fresh != old_leader, "failover/new-leader");
  Check(ms < 100.0, "failover/under-100ms-sim");
  Check(g.commitIndex() == 2, "failover/commit-survives");
  // Neue Writes auf neuem Leader funktionieren.
  Check(g.append("put c 3") == 3, "failover/append-after");
  Check(g.get("c") == std::optional<std::string>("3"), "failover/read-after");
}

void TestMajorityAndCatchup() {
  RaftGroup g(4, "", "");
  g.killNode(2);  // 2/3 lebend -> Quorum bleibt
  Check(g.aliveCount() == 2, "quorum/one-down-count");
  g.electLeader();
  Check(g.leaderId() >= 0, "quorum/leader-with-2");
  Check(g.append("put x 9") == 1, "quorum/append-with-2");
  Check(g.commitIndex() == 1, "quorum/commit-with-2");
  // Revive holt Log auf.
  g.reviveNode(2);
  Check(g.isAlive(2), "quorum/revived");
  Check(g.node(2).log.size() == 1, "quorum/catchup-log");
  Check(g.node(2).commit_index == 1, "quorum/catchup-commit");

  // Doppel-Ausfall -> kein Quorum, kein Commit.
  g.killNode(1);
  g.killNode(2);
  Check(g.aliveCount() == 1, "quorum/single-left");
  Check(g.electLeader() == -1, "quorum/no-leader-without-majority");
  Check(g.append("put y 1") == 0, "quorum/no-append-without-leader");
}

}  // namespace

int main() {
  TestShardRange();
  TestElection();
  TestReplication();
  TestFailover100ms();
  TestMajorityAndCatchup();

  if (g_failures == 0) {
    std::cout << "ALL RAFT TESTS PASSED\n";
    return 0;
  }
  std::cout << g_failures << " RAFT TEST(S) FAILED\n";
  return 1;
}
