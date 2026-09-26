// s13-raft Tests: Shard-Range, Election, Replikation, Failover <100ms sim.
// Framework-los (assert-light + cout), CTest-Name: raft (ctest -R raft).

#include <chrono>
#include <filesystem>
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

// Chaos-Sequenzen (deterministisch, eigener RNG Seed 7, feste Op-Folgen).
// Stil: Check()+g_failures wie Bestand. Temp-Files unter temp_directory_path
// mit unique Namen + Cleanup (Datei + ".tmp"). Laufzeit <<5s (in-process Sim).

void TestChaosStaleLoadMonoPhantom() {
  namespace fs = std::filesystem;
  // Eigener deterministischer RNG, Seed 7 (LCG, kein <random>-Include).
  std::uint32_t rng = 7u;
  auto next = [&]() -> std::uint32_t {
    rng = rng * 1664525u + 1013904223u;
    return rng;
  };
  const fs::path log_path =
      fs::temp_directory_path() / "raft_chaos7_a_stale.bin";
  fs::remove(log_path);
  fs::remove(fs::path(log_path.string() + ".tmp"));

  RaftGroup g(20, "", "");
  // Feste Op-Folge: 2 puts -> Save -> 3 puts (Tail).
  Check(g.append("put chaos_a1 v1") == 1, "chaos-a/idx1");
  Check(g.append("put chaos_a2 v2") == 2, "chaos-a/idx2");
  const std::uint64_t commit_saved = g.commitIndex();
  const std::uint64_t term_saved = g.term();
  Check(commit_saved == 2, "chaos-a/saved-commit-2");
  Check(g.SaveLog(log_path.string()), "chaos-a/save-ok");

  const char* tail[3] = {"put chaos_a3 v3", "put chaos_a4 v4",
                         "put chaos_a5 v5"};
  std::uint64_t prev = commit_saved;
  for (int i = 0; i < 3; ++i) {
    (void)next();  // deterministischer Takt, feste Folge bleibt fix
    std::uint64_t idx = g.append(tail[i]);
    Check(idx == static_cast<std::uint64_t>(3 + i), "chaos-a/tail-idx");
    Check(g.commitIndex() >= prev, "chaos-a/commit-mono-append");
    prev = g.commitIndex();
  }
  const std::uint64_t commit_before_kill = g.commitIndex();
  Check(commit_before_kill == 5, "chaos-a/commit-5-prekill");
  Check(g.is_caught_up(1) && g.is_caught_up(2),
        "chaos-a/caughtup-prekill");
  Check(g.follower_get(1, "chaos_a5") == std::optional<std::string>("v5"),
        "chaos-a/follower-prekill");

  int old_leader = g.leaderId();
  g.killLeader();
  Check(g.leaderId() == -1, "chaos-a/no-leader-after-kill");
  int fresh = g.failover();
  Check(fresh >= 0 && fresh != old_leader, "chaos-a/new-leader");
  // Commit-Monotonie ueber Failover (lebender Nachfolger hat gleichen Stand).
  Check(g.commitIndex() == commit_before_kill,
        "chaos-a/commit-mono-failover");
  Check(g.term() >= term_saved, "chaos-a/term-mono-failover");

  // Stale-Load: Recovery-Restore auf gespeicherten Stand (commit 2).
  // Invariante: exaktes Restore + Phantom-Freiheit (Tail-Keys unsichtbar),
  // Term-Monotonie (term = max). Kein In-place-Commit-Vorwaertsschutz:
  // LoadLog setzt bewusst zurueck (applied.clear + re-apply).
  Check(g.LoadLog(log_path.string()), "chaos-a/load-stale-ok");
  Check(g.commitIndex() == commit_saved,
        "chaos-a/load-restores-saved-commit");
  Check(g.term() >= term_saved, "chaos-a/term-mono-after-load");
  Check(!g.get("chaos_a3").has_value() &&
            !g.get("chaos_a4").has_value() &&
            !g.get("chaos_a5").has_value(),
        "chaos-a/phantom-free-get");
  int follower = -1;
  for (int i = 0; i < 3; ++i) {
    if (i != g.leaderId() && g.isAlive(i)) {
      follower = i;
      break;
    }
  }
  Check(follower >= 0, "chaos-a/alive-follower-present");
  if (follower >= 0) {
    Check(!g.follower_get(follower, "chaos_a3").has_value(),
          "chaos-a/phantom-free-follower");
    Check(g.is_caught_up(follower), "chaos-a/caughtup-after-load");
  }
  Check(g.get("chaos_a1") == std::optional<std::string>("v1") &&
            g.get("chaos_a2") == std::optional<std::string>("v2"),
        "chaos-a/saved-visible");

  fs::remove(log_path);
  fs::remove(fs::path(log_path.string() + ".tmp"));
}

void TestChaosSnapshotTailReplay() {
  namespace fs = std::filesystem;
  std::uint32_t rng = 7u;
  auto next = [&]() -> std::uint32_t {
    rng = rng * 1664525u + 1013904223u;
    return rng;
  };
  const fs::path snap_path =
      fs::temp_directory_path() / "raft_chaos7_b_snap.bin";
  fs::remove(snap_path);
  fs::remove(fs::path(snap_path.string() + ".tmp"));

  RaftGroup g(21, "", "");
  // Feste Folge: 3 puts -> Snapshot bei C -> 5 appends bis C+5.
  Check(g.append("put chaos_b1 v1") == 1, "chaos-b/idx1");
  Check(g.append("put chaos_b2 v2") == 2, "chaos-b/idx2");
  Check(g.append("put chaos_b3 v3") == 3, "chaos-b/idx3");
  const std::uint64_t c = g.commitIndex();
  Check(c == 3, "chaos-b/snap-base-c");
  Check(g.SaveSnapshot(snap_path.string()), "chaos-b/save-snap-ok");

  const char* tail[5] = {"put chaos_b4 v4", "put chaos_b5 v5",
                         "put chaos_b6 v6", "put chaos_b7 v7",
                         "put chaos_b8 v8"};
  for (int i = 0; i < 5; ++i) {
    (void)next();
    std::uint64_t idx = g.append(tail[i]);
    Check(idx == c + static_cast<std::uint64_t>(i + 1), "chaos-b/tail-idx");
  }
  Check(g.commitIndex() == c + 5, "chaos-b/commit-c-plus-5");

  // Aelteren Snapshot (C) laden: kein Commit-Verlust, Tail-Replay.
  Check(g.LoadSnapshot(snap_path.string()), "chaos-b/load-snap-ok");
  Check(g.commitIndex() == c + 5, "chaos-b/commit-preserved-after-snap");
  bool all_visible = true;
  for (int i = 1; i <= 8; ++i) {
    std::string k = "chaos_b" + std::to_string(i);
    std::string v = "v" + std::to_string(i);
    if (g.get(k) != std::optional<std::string>(v)) {
      all_visible = false;
      break;
    }
  }
  Check(all_visible, "chaos-b/tail-replay-visible");
  Check(g.is_caught_up(1) && g.is_caught_up(2),
        "chaos-b/caughtup-after-snap");
  // Naechster Append-Index = C+6 (Basis + Tail + 1).
  std::uint64_t nxt = g.append("put chaos_b9 v9");
  Check(nxt == c + 6, "chaos-b/append-c-plus-6");
  Check(g.get("chaos_b9") == std::optional<std::string>("v9"),
        "chaos-b/read-after-replay");

  fs::remove(snap_path);
  fs::remove(fs::path(snap_path.string() + ".tmp"));
}

void TestChaosReviveAfterSnapshotAutosave() {
  namespace fs = std::filesystem;
  std::uint32_t rng = 7u;
  auto next = [&]() -> std::uint32_t {
    rng = rng * 1664525u + 1013904223u;
    return rng;
  };
  (void)next();
  const fs::path snap_path =
      fs::temp_directory_path() / "raft_chaos7_c_snap.bin";
  const fs::path auto_path =
      fs::temp_directory_path() / "raft_chaos7_c_auto.bin";
  fs::remove(snap_path);
  fs::remove(fs::path(snap_path.string() + ".tmp"));
  fs::remove(auto_path);
  fs::remove(fs::path(auto_path.string() + ".tmp"));

  RaftGroup g(22, "", "");
  Check(g.append("put chaos_c1 v1") == 1, "chaos-c/idx1");
  Check(g.append("put chaos_c2 v2") == 2, "chaos-c/idx2");
  Check(g.SaveSnapshot(snap_path.string()), "chaos-c/save-snap-ok");

  // reviveNode-Pfad: Knoten 2 tot, Leader schreibt weiter, Revive holt auf.
  g.killNode(2);
  Check(!g.is_caught_up(2), "chaos-c/dead-not-caughtup");
  Check(g.append("put chaos_c3 v3") == 3, "chaos-c/idx3-quorum");
  g.reviveNode(2);
  Check(g.isAlive(2), "chaos-c/revived");
  Check(!g.node(2).log.empty(), "chaos-c/catchup-log-nonempty");
  Check(g.node(2).commit_index == 3, "chaos-c/catchup-commit");
  Check(g.is_caught_up(2), "chaos-c/caughtup-after-revive");
  Check(g.follower_get(2, "chaos_c3") == std::optional<std::string>("v3"),
        "chaos-c/follower-after-revive");

  // Aelteren Snapshot (C=2) laden, dann erneut revive (nach Snapshot).
  Check(g.LoadSnapshot(snap_path.string()), "chaos-c/load-snap-ok");
  Check(g.commitIndex() == 3, "chaos-c/commit-preserved");
  Check(g.get("chaos_c3") == std::optional<std::string>("v3"),
        "chaos-c/tail-replay");
  g.killNode(1);
  g.reviveNode(1);
  Check(g.is_caught_up(1), "chaos-c/caughtup-revive-after-snap");
  Check(g.follower_get(1, "chaos_c3") == std::optional<std::string>("v3"),
        "chaos-c/follower-after-snap-revive");

  // Autosave-Pfad: jeder Commit schreibt volles Log (kompaktiert, Basis 2).
  g.set_autosave_log(auto_path.string());
  std::uint64_t idx4 = g.append("put chaos_c4 v4");
  Check(idx4 == 4, "chaos-c/autosave-idx4");
  Check(fs::exists(auto_path), "chaos-c/autosave-file-exists");
  // Frische Gruppe kann Autosave-Datei per LoadLog lesen.
  RaftGroup h(23, "", "");
  Check(h.LoadLog(auto_path.string()), "chaos-c/autosave-load-ok");
  Check(h.commitIndex() == g.commitIndex(),
        "chaos-c/autosave-commit-match");
  Check(h.get("chaos_c3") == std::optional<std::string>("v3") &&
            h.get("chaos_c4") == std::optional<std::string>("v4"),
        "chaos-c/autosave-tail-visible");
  g.clear_autosave();

  fs::remove(snap_path);
  fs::remove(fs::path(snap_path.string() + ".tmp"));
  fs::remove(auto_path);
  fs::remove(fs::path(auto_path.string() + ".tmp"));
}

}  // namespace

int main() {
  TestShardRange();
  TestElection();
  TestReplication();
  TestFailover100ms();
  TestMajorityAndCatchup();
  TestChaosStaleLoadMonoPhantom();
  TestChaosSnapshotTailReplay();
  TestChaosReviveAfterSnapshotAutosave();

  if (g_failures == 0) {
    std::cout << "ALL RAFT TESTS PASSED\n";
    return 0;
  }
  std::cout << g_failures << " RAFT TEST(S) FAILED\n";
  return 1;
}
