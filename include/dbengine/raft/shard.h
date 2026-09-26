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
#include <utility>
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
  // Move (fuer Shard::split-Rueckgabe): sperrt other, verschiebt Zustand.
  RaftGroup(RaftGroup&& other) noexcept;
  RaftGroup& operator=(RaftGroup&& other) noexcept;

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

  // ---- s25: Follower-Reads (read_index light) ------------------------------
  // follower_get liest die State-Machine eines Followers (kann stale sein).
  // is_caught_up: follower.commit == leader.commit (linearisierbar-lesbar).
  [[nodiscard]] std::optional<std::string> follower_get(
      int node_id, const std::string& key) const;
  [[nodiscard]] bool is_caught_up(int node_id) const;

  // ---- s25: persistentes Log (Datei + Replay) -------------------------------
  // Format "RAFT1": term u64, commit u64, n u64, je Entry term/index/cmd.
  // Save: Leader-Log + term/commit. Load: stellt Log auf allen lebenden
  // Knoten wieder her (commit/apply). Gibt false bei IO-/Formatfehler.
  bool SaveLog(const std::string& path) const;
  bool LoadLog(const std::string& path);

  // ---- s25: Snapshots (State-Machine dump/load + Log-Compaction) ------------
  // Format "RSNP1": last_index u64, n u64, je Paar klen/vlen + bytes.
  // Load: setzt applied-Maps, kappt Log <= last_index, commit/last_applied
  // auf max(commit, last_index).
  bool SaveSnapshot(const std::string& path) const;
  bool LoadSnapshot(const std::string& path);

 private:
  // Wendet alle committeten, noch nicht angewendeten Eintraege eines Knotens an.
  // last_applied/commit sind echte Indizes (Basis log_base_).
  void apply(Node& node);
  static bool parseCommand(const std::string& cmd, std::string& op,
                           std::string& key, std::string& value);
  void electLocked(int candidate);  // Setzt Rollen/Votes, braucht Mehrheit

  mutable std::mutex mutex_;
  int shard_id_ = 0;
  std::vector<Node> nodes_;
  int leader_id_ = -1;
  std::uint64_t term_ = 0;
  // s25: Log-Basis (kompaktierte Prefix-Laenge via Snapshot). Eintrag an
  // Position p hat Index log_base_ + p + 1. Ohne Snapshot 0 (V1-Semantik).
  std::uint64_t log_base_ = 0;
};

// Shard/Tablet: id + Key-Range [range_start, range_end), traegt eine RaftGroup.
class Shard {
 public:
  Shard(int id, std::string range_start, std::string range_end);
  Shard(Shard&&) noexcept = default;
  Shard& operator=(Shard&&) noexcept = default;
  Shard(const Shard&) = delete;
  Shard& operator=(const Shard&) = delete;

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

  // s25: Split/Merge vorbereitet (reine Range-Ops, kein Daten-Move in V2).
  // split(mid): [start,mid) + [mid,end). mid muss in (start,end) liegen
  // (leeres end = offen, dann reicht mid > start).
  [[nodiscard]] std::pair<Shard, Shard> split(const std::string& mid) const;
  [[nodiscard]] bool can_merge_with(const Shard& other) const;

 private:
  int id_;
  std::string start_;
  std::string end_;
  RaftGroup group_;
};

// ---- s25: TCP-Wire-Codec (len-prefixed Entry-Replikation) -------------------
// Payload = term u64 BE | index u64 BE | cmd_len u32 BE | cmd bytes.
// Framing = msg_len u32 BE (Payload-Laenge) + Payload. SendWire/RecvWire
// arbeiten auf beliebigem Stream-fd (TCP-Socket, socketpair, pipe).
std::string EncodeEntryWire(const Entry& e);
bool DecodeEntryWire(const std::string& payload, Entry& out);
bool SendWire(int fd, const std::string& payload);
bool RecvWire(int fd, std::string& payload);
// Loopback-Helfer (POSIX-TCP, 127.0.0.1, ephemeral Port): listen_fd + port
// erzeugen, verbinden, accepten. Rueckgabe verbundene fds (client, server).
bool TcpLoopbackPair(int& client_fd, int& server_fd);

}  // namespace dbengine::raft
