#include "dbengine/raft/shard.h"

namespace dbengine::raft {

RaftGroup::RaftGroup(int shard_id, std::string range_start, std::string range_end)
    : shard_id_(shard_id) {
  (void)range_start;
  (void)range_end;
  nodes_.reserve(kGroupSize);
  for (int i = 0; i < kGroupSize; ++i) {
    Node n;
    n.id = i;
    nodes_.push_back(std::move(n));
  }
  // Initiale Wahl, damit die Gruppe direkt einen Leader hat.
  electLeader();
}

bool RaftGroup::parseCommand(const std::string& cmd, std::string& op,
                             std::string& key, std::string& value) {
  // Mini-Format: "put <key> <value>" / "del <key>". Value darf Spaces enthalten.
  if (cmd.compare(0, 4, "put ") == 0) {
    std::size_t ks = 4;
    std::size_t sp = cmd.find(' ', ks);
    if (sp == std::string::npos) return false;
    op = "put";
    key = cmd.substr(ks, sp - ks);
    value = cmd.substr(sp + 1);
    return !key.empty();
  }
  if (cmd.compare(0, 4, "del ") == 0) {
    op = "del";
    key = cmd.substr(4);
    value.clear();
    return !key.empty();
  }
  return false;
}

void RaftGroup::apply(Node& node) {
  while (node.last_applied < node.commit_index &&
         node.last_applied < node.log.size()) {
    const Entry& e = node.log[static_cast<std::size_t>(node.last_applied)];
    std::string op, key, value;
    if (parseCommand(e.command, op, key, value)) {
      if (op == "put") {
        node.applied[key] = value;
      } else {
        node.applied.erase(key);
      }
    }
    ++node.last_applied;
  }
}

void RaftGroup::electLocked(int candidate) {
  // Alle lebenden Knoten stimmen fuer den Kandidaten (Sim-Vereinfachung:
  // kein Split-Vote, kein Timeout — deterministisch, ein Round-Trip).
  for (auto& n : nodes_) {
    if (!n.alive) continue;
    n.current_term = term_;
    n.voted_for = candidate;
    n.role = Role::Follower;
  }
  nodes_[static_cast<std::size_t>(candidate)].role = Role::Leader;
  nodes_[static_cast<std::size_t>(candidate)].voted_for = candidate;
  leader_id_ = candidate;
}

int RaftGroup::electLeader() {
  std::lock_guard<std::mutex> lock(mutex_);
  int alive = 0;
  int candidate = -1;
  for (const auto& n : nodes_) {
    if (n.alive) {
      ++alive;
      if (candidate < 0) candidate = n.id;
    }
  }
  if (alive < 2 || candidate < 0) {  // kein Quorum bei 3er-Gruppe
    leader_id_ = -1;
    for (auto& n : nodes_) {
      if (n.alive) n.role = Role::Follower;
    }
    return -1;
  }
  ++term_;
  electLocked(candidate);
  return leader_id_;
}

int RaftGroup::leaderId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return leader_id_;
}

std::uint64_t RaftGroup::term() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return term_;
}

bool RaftGroup::replicateToFollowers(const Entry& entry) {
  // Hinweis: wird i.d.R. unter Lock aus append() aufgerufen; hier eigene
  // Variante ohne Lock fuers externe Testen nicht noetig — append lockt.
  // Diese Methode lockt selbst (nicht aus append heraus aufrufen).
  std::lock_guard<std::mutex> lock(mutex_);
  if (leader_id_ < 0) return false;
  int ack = 0;
  for (auto& n : nodes_) {
    if (!n.alive) continue;
    if (n.id == leader_id_) {
      ++ack;  // Leader hat den Eintrag bereits
      continue;
    }
    // Follower-Append: Luecken per Leader-Log auffuellen (Catch-up light),
    // dann Eintrag anhaengen. Sim ersetzt die AppendEntries-RPC.
    const Node& leader = nodes_[static_cast<std::size_t>(leader_id_)];
    if (n.current_term != term_) {
      n.current_term = term_;
      n.voted_for = leader_id_;
      n.role = Role::Follower;
    }
    // Log angleichen: fehlende Suffixe vom Leader kopieren (ohne entry selbst).
    while (n.log.size() + 1 < entry.index) {
      std::size_t src = n.log.size();
      if (src < leader.log.size()) {
        n.log.push_back(leader.log[src]);
      } else {
        break;
      }
    }
    if (n.log.size() + 1 == entry.index) {
      n.log.push_back(entry);
      ++ack;
    } else if (n.log.size() >= entry.index && entry.index > 0 &&
               n.log[static_cast<std::size_t>(entry.index - 1)].index ==
                   entry.index) {
      ++ack;  // bereits vorhanden (Retry/Idempotenz)
    }
  }
  return ack >= 2;  // Mehrheit bei 3 Knoten
}

std::uint64_t RaftGroup::append(std::string command) {
  Entry entry;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (leader_id_ < 0) return 0;
    Node& leader = nodes_[static_cast<std::size_t>(leader_id_)];
    if (!leader.alive || command.empty()) return 0;
    entry.term = term_;
    entry.index = leader.log.size() + 1;
    entry.command = std::move(command);
    leader.log.push_back(entry);
  }
  // Replikation ausserhalb des append-Locks? replicateToFollowers lockt selbst.
  // Zwischen den Locks kann kein konkurrierender append interleaven, da Tests
  // single-threaded sind; C++-seitig bleibt es via Mutex korrekt, nur die
  // Commit-Zuweisung braucht nochmals den Lock.
  if (!replicateToFollowers(entry)) {
    return 0;  // kein Quorum — Eintrag bleibt uncommitted
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& n : nodes_) {
    if (!n.alive) continue;
    if (!n.log.empty() && n.log.back().index >= entry.index) {
      if (n.commit_index < entry.index) n.commit_index = entry.index;
      apply(n);
    }
  }
  return entry.index;
}

void RaftGroup::killLeader() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (leader_id_ >= 0) {
    Node& l = nodes_[static_cast<std::size_t>(leader_id_)];
    l.alive = false;
    l.role = Role::Follower;
    leader_id_ = -1;
  }
}

int RaftGroup::failover() {
  // Neuwahl = electLeader (term++, Vote, Mehrheit). Synchron/in-process,
  // daher typischerweise <1ms, garantiert <100ms sim.
  return electLeader();
}

void RaftGroup::killNode(int node_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (node_id < 0 || node_id >= static_cast<int>(nodes_.size())) return;
  nodes_[static_cast<std::size_t>(node_id)].alive = false;
  nodes_[static_cast<std::size_t>(node_id)].role = Role::Follower;
  if (leader_id_ == node_id) leader_id_ = -1;
}

void RaftGroup::reviveNode(int node_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (node_id < 0 || node_id >= static_cast<int>(nodes_.size())) return;
  Node& n = nodes_[static_cast<std::size_t>(node_id)];
  n.alive = true;
  n.role = Role::Follower;
  // Catch-up: volles Leader-Log kopieren, commit angleichen, apply.
  if (leader_id_ >= 0 && leader_id_ != node_id) {
    const Node& leader = nodes_[static_cast<std::size_t>(leader_id_)];
    n.log = leader.log;
    n.current_term = term_;
    n.voted_for = leader_id_;
    n.commit_index = leader.commit_index;
    apply(n);
  }
}

std::uint64_t RaftGroup::commitIndex() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (leader_id_ < 0) return 0;
  return nodes_[static_cast<std::size_t>(leader_id_)].commit_index;
}

std::size_t RaftGroup::logSize() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (leader_id_ < 0) return 0;
  return nodes_[static_cast<std::size_t>(leader_id_)].log.size();
}

std::size_t RaftGroup::aliveCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t n = 0;
  for (const auto& nd : nodes_) {
    if (nd.alive) ++n;
  }
  return n;
}

bool RaftGroup::isAlive(int node_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (node_id < 0 || node_id >= static_cast<int>(nodes_.size())) return false;
  return nodes_[static_cast<std::size_t>(node_id)].alive;
}

const Node& RaftGroup::node(int node_id) const {
  return nodes_.at(static_cast<std::size_t>(node_id));
}

std::optional<std::string> RaftGroup::get(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (leader_id_ < 0) return std::nullopt;
  const auto& applied = nodes_[static_cast<std::size_t>(leader_id_)].applied;
  auto it = applied.find(key);
  if (it == applied.end()) return std::nullopt;
  return it->second;
}

// ---- Shard ---------------------------------------------------------------

Shard::Shard(int id, std::string range_start, std::string range_end)
    : id_(id),
      start_(std::move(range_start)),
      end_(std::move(range_end)),
      group_(id_, start_, end_) {}

bool Shard::contains(const std::string& key) const noexcept {
  if (key < start_) return false;
  if (!end_.empty() && !(key < end_)) return false;
  return true;
}

}  // namespace dbengine::raft
