#pragma once

// Raft-Shard Stub (s13-raft): Range/Tablet + 3er-Raft-Gruppe in-process.
// Kein echtes Netzwerk: Replikation = synchroner Methodenaufruf (Sim).
// Leader-Election via term/vote (Mehrheit), Log-Replikation mit
// replicateToFollowers, commitIndex/apply in eine KV-State-Machine.
// Failover: killLeader + failover() waehlt deterministisch den naechsten
// lebenden Knoten — in-process in Mikrosekunden, also <100ms sim.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dbengine::raft {

// Logisch 96MB-Tablet-Groesse (Platzhalter, keine echte Split-Logik in V1).
inline constexpr std::uint64_t kShardTargetBytes = 96ULL * 1024ULL * 1024ULL;

struct Entry {
  std::uint64_t term = 0;
  std::uint64_t index = 0;  // 1-basiert, 0 = leer
  std::string command;      // z.B. "put k v" / "del k"
};

enum class Role : std::uint8_t { Follower, Candidate, Leader };

struct Node {
  int id = -1;
  Role role = Role::Follower;
  std::uint64_t current_term = 0;
  int voted_for = -1;  // -1 = keine Stimme in current_term
  std::vector<Entry> log;
  std::uint64_t commit_index = 0;  // hoechster committeter Index (1-basiert)
  std::uint64_t last_applied = 0;  // hoechster angewendeter Index
  bool alive = true;
  std::map<std::string, std::string> applied;  // State-Machine (KV)
};

class RaftGroup {
 public:
  static constexpr int kGroupSize = 3;

  RaftGroup() : RaftGroup(0, "", "") {}
  RaftGroup(int shard_id, std::string range_start, std::string range_end);

  RaftGroup(const RaftGroup&) = delete;
  RaftGroup& operator=(const RaftGroup&) = delete;

  // ---- Election (term/vote, Mehrheit) ----------------------------------
  // Waehlt den ersten lebenden Knoten als Leader (deterministisch).
  // Erhoeht term, sammelt Votes aller lebenden Knoten, braucht Mehrheit.
  // Gibt Leader-Id zurueck, -1 wenn kein Quorum (<=1 lebend).
  int electLeader();
  [[nodiscard]] int leaderId() const;
  [[nodiscard]] std::uint64_t term() const;

  // ---- Log-Replikation ---------------------------------------------------
  // Nur Leader darf appenden. Repliziert synchron auf Follower
  // (replicateToFollowers), committet bei Mehrheit, wendet an (apply).
  // Gibt Log-Index zurueck, 0 bei Fehler (kein Leader / tot / kein Quorum).
  std::uint64_t append(std::string command);

  // Kopiert Eintrag auf alle lebenden Follower. True bei Mehrheit (inkl. Leader).
  bool replicateToFollowers(const Entry& entry);

  // ---- Failover ----------------------------------------------------------
  // killLeader(): Leader-Knoten auf alive=false setzen (Crash-Sim).
  // failover(): Neuwahl unter Lebenden. Gibt neue Leader-Id zurueck (-1 o. Quorum).
  void killLeader();
  int failover();
  void killNode(int node_id);
  void reviveNode(int node_id);  // holt Log auf (Catch-up) + apply

  // ---- Abfragen ------------------------------------------------------------
  [[nodiscard]] std::uint64_t commitIndex() const;
  [[nodiscard]] std::size_t logSize() const;  // Log-Laenge des Leaders (0 o. Leader)
  [[nodiscard]] std::size_t aliveCount() const;
  [[nodiscard]] bool isAlive(int node_id) const;
  [[nodiscard]] const Node& node(int node_id) const;
  [[nodiscard]] std::optional<std::string> get(const std::string& key) const;
  [[nodiscard]] int shardId() const noexcept { return shard_id_; }

 private:
  // Wendet alle committeten, noch nicht angewendeten Eintraege eines Knotens an.
  static void apply(Node& node);
  static bool parseCommand(const std::string& cmd, std::string& op,
                           std::string& key, std::string& value);
  void electLocked(int candidate);  // Setzt Rollen/Votes, braucht Mehrheit

  mutable std::mutex mutex_;
  int shard_id_ = 0;
  std::vector<Node> nodes_;
  int leader_id_ = -1;
  std::uint64_t term_ = 0;
};

// Shard/Tablet: id + Key-Range [range_start, range_end), traegt eine RaftGroup.
class Shard {
 public:
  Shard(int id, std::string range_start, std::string range_end);

  [[nodiscard]] int id() const noexcept { return id_; }
  [[nodiscard]] const std::string& rangeStart() const noexcept { return start_; }
  [[nodiscard]] const std::string& rangeEnd() const noexcept { return end_; }

  // Gehoert Key in [start, end)? Leeres end = offen (letztes Tablet).
  [[nodiscard]] bool contains(const std::string& key) const noexcept;
  [[nodiscard]] std::uint64_t targetBytes() const noexcept {
    return kShardTargetBytes;
  }

  RaftGroup& group() noexcept { return group_; }
  const RaftGroup& group() const noexcept { return group_; }

 private:
  int id_;
  std::string start_;
  std::string end_;
  RaftGroup group_;
};

}  // namespace dbengine::raft
