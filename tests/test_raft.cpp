// s13-raft Tests: Shard-Range, Election, Replikation, Failover <100ms sim.
// Framework-los (assert-light + cout), CTest-Name: raft (ctest -R raft).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

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

// Partition/Drop-Fault-Injector (deterministisch, Seed 42).
// Minderheit isoliert -> Quorum bleibt, Commit ok. Vollsplit -> kein
// Leader/Commit/Failover. Heal stellt Replikation wieder her.
// Drop 100% verhaelt sich wie Cut, Self-Send immer true.

void TestPartitionMinorityQuorum() {
  RaftGroup g(10, "", "");
  int leader = g.leaderId();
  Check(leader >= 0, "part-minority/has-leader");
  int iso = -1;
  for (int i = 0; i < 3; ++i) {
    if (i != leader) iso = i;
  }
  Check(iso >= 0 && iso != leader, "part-minority/follower-picked");
  int other = -1;
  for (int i = 0; i < 3; ++i) {
    if (i != leader && i != iso) other = i;
  }
  Check(other >= 0, "part-minority/other-picked");
  g.isolate_from_all(iso);
  Check(!g.can_send(leader, iso), "part-minority/cut");
  Check(!g.can_send(iso, leader), "part-minority/cut-sym");
  Check(g.can_send(leader, other), "part-minority/majority-up");
  Check(g.can_send(leader, leader), "part-minority/self-true");
  int l2 = g.electLeader();
  Check(l2 >= 0, "part-minority/quorum-kept");
  std::uint64_t idx = g.append("put pm1 v1");
  Check(idx == 1, "part-minority/commit-ok");
  Check(g.commitIndex() == 1, "part-minority/commit-index");
  Check(g.follower_get(other, "pm1") == std::optional<std::string>("v1"),
        "part-minority/majority-has-it");
  Check(!g.follower_get(iso, "pm1").has_value(),
        "part-minority/minority-stale");
}

void TestPartitionNoQuorum() {
  RaftGroup g(11, "", "");
  Check(g.append("put pre 1") == 1, "part-noquorum/pre-idx1");
  g.isolate(0, 1);
  g.isolate(0, 2);
  g.isolate(1, 2);
  Check(!g.can_send(0, 1) && !g.can_send(1, 0) && !g.can_send(0, 2) &&
            !g.can_send(2, 0) && !g.can_send(1, 2) && !g.can_send(2, 1),
        "part-noquorum/all-cut");
  Check(g.can_send(0, 0) && g.can_send(1, 1) && g.can_send(2, 2),
        "part-noquorum/self-true");
  // Leader noch gesetzt, aber ohne erreichbare Mehrheit kein Commit.
  Check(g.append("put nop x") == 0, "part-noquorum/no-commit");
  Check(g.electLeader() == -1, "part-noquorum/no-leader");
  Check(g.failover() == -1, "part-noquorum/no-failover");
  Check(g.append("put nop2 y") == 0, "part-noquorum/no-commit-no-leader");
}

void TestPartitionHeal() {
  RaftGroup g(12, "", "");
  Check(g.append("put h1 v1") == 1, "part-heal/idx1");
  int leader = g.leaderId();
  Check(leader >= 0, "part-heal/has-leader");
  int iso = -1;
  for (int i = 0; i < 3; ++i) {
    if (i != leader) iso = i;
  }
  int other = -1;
  for (int i = 0; i < 3; ++i) {
    if (i != leader && i != iso) other = i;
  }
  g.isolate_from_all(iso);
  std::uint64_t idx2 = g.append("put h2 v2");
  Check(idx2 == 2, "part-heal/append-during-partition");
  Check(!g.follower_get(iso, "h2").has_value(),
        "part-heal/stale-during-partition");
  // Paarweises Heilen (symmetrisch) stellt Replikation wieder her.
  g.heal(leader, iso);
  g.heal(other, iso);
  Check(g.can_send(leader, iso) && g.can_send(iso, leader),
        "part-heal/link-up");
  Check(g.is_caught_up(iso), "part-heal/caught-up-after-heal");
  Check(g.follower_get(iso, "h2") == std::optional<std::string>("v2"),
        "part-heal/replicated-after-heal");
  std::uint64_t idx3 = g.append("put h3 v3");
  Check(idx3 == 3, "part-heal/append-after-heal");
  Check(g.follower_get(iso, "h3") == std::optional<std::string>("v3"),
        "part-heal/follower-after-heal");
}

void TestDropEqualsCut() {
  RaftGroup g(13, "", "");
  int leader = g.leaderId();
  Check(leader >= 0, "drop/has-leader");
  int f1 = -1, f2 = -1;
  for (int i = 0; i < 3; ++i) {
    if (i == leader) continue;
    if (f1 < 0) {
      f1 = i;
    } else {
      f2 = i;
    }
  }
  Check(f1 >= 0 && f2 >= 0, "drop/followers-picked");
  // Self-Send immer true, selbst bei 100% Self-Drop.
  g.set_drop_rate(leader, leader, 100);
  Check(g.can_send(leader, leader), "drop/self-true");
  g.set_drop_rate(leader, leader, 0);
  // Beidseitiger 100%-Drop == symmetrischer Cut dieser Kante.
  g.set_drop_rate(leader, f1, 100);
  g.set_drop_rate(f1, leader, 100);
  Check(!g.can_send(leader, f1), "drop/cut-like");
  Check(!g.can_send(f1, leader), "drop/cut-like-sym");
  Check(g.can_send(leader, f2), "drop/other-up");
  // Quorum ueber Leader+f2 bleibt, Commit ok.
  Check(g.append("put d1 v1") != 0, "drop/quorum-kept");
  // Zweite Kante per Drop kappen -> keine erreichbare Mehrheit mehr.
  g.set_drop_rate(leader, f2, 100);
  Check(!g.can_send(leader, f2), "drop/second-cut");
  Check(g.append("put d2 v2") == 0, "drop/no-quorum-like-cut");
  // heal_all raeumt auch Drop-Raten weg und holt auf.
  g.heal_all();
  Check(g.can_send(leader, f1) && g.can_send(leader, f2), "drop/healed");
  Check(g.append("put d3 v3") != 0, "drop/append-after-heal");
  // Determinismus: gleicher Seed -> gleiche Drop-Sequenz (50% Verlust).
  g.set_drop_rate(leader, f1, 50);
  g.set_net_seed(123);
  std::vector<bool> seq;
  for (int i = 0; i < 20; ++i) seq.push_back(g.can_send(leader, f1));
  g.set_net_seed(123);
  bool same = true;
  for (int i = 0; i < 20; ++i) {
    if (g.can_send(leader, f1) != seq[static_cast<std::size_t>(i)]) {
      same = false;
      break;
    }
  }
  Check(same, "drop/seed-deterministic");
  g.set_drop_rate(leader, f1, 0);
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
  TestPartitionMinorityQuorum();
  TestPartitionNoQuorum();
  TestPartitionHeal();
  TestDropEqualsCut();
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
