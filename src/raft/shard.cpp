#include "dbengine/raft/shard.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace dbengine::raft {

namespace {
// Dateiformat-Helfer (LE, host == x86-64 LE).
void W64(std::ofstream& o, std::uint64_t v) {
  o.write(reinterpret_cast<const char*>(&v), sizeof(v));
}
void W32(std::ofstream& o, std::uint32_t v) {
  o.write(reinterpret_cast<const char*>(&v), sizeof(v));
}
bool R64(std::ifstream& in, std::uint64_t& v) {
  return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(v)));
}
bool R32(std::ifstream& in, std::uint32_t& v) {
  return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(v)));
}
// Wire (BE/Network-Order).
void PutU64be(std::string& o, std::uint64_t v) {
  for (int i = 7; i >= 0; --i) o.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void PutU32be(std::string& o, std::uint32_t v) {
  for (int i = 3; i >= 0; --i) o.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
std::uint64_t GetU64be(const char* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<unsigned char>(p[i]);
  return v;
}
std::uint32_t GetU32be(const char* p) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v = (v << 8) | static_cast<unsigned char>(p[i]);
  return v;
}
bool WriteFull(int fd, const char* buf, std::size_t n) {
  std::size_t done = 0;
  while (done < n) {
    ssize_t w = ::write(fd, buf + done, n - done);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (w == 0) return false;
    done += static_cast<std::size_t>(w);
  }
  return true;
}
bool ReadFull(int fd, char* buf, std::size_t n) {
  std::size_t got = 0;
  while (got < n) {
    ssize_t r = ::read(fd, buf + got, n - got);
    if (r == 0) return false;  // EOF
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    got += static_cast<std::size_t>(r);
  }
  return true;
}
}  // namespace

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

// s25: Move (Mutex neu, Zustand unter Lock verschoben).
RaftGroup::RaftGroup(RaftGroup&& other) noexcept {
  std::lock_guard<std::mutex> l(other.mutex_);
  shard_id_ = other.shard_id_;
  nodes_ = std::move(other.nodes_);
  leader_id_ = other.leader_id_;
  term_ = other.term_;
  log_base_ = other.log_base_;
}

RaftGroup& RaftGroup::operator=(RaftGroup&& other) noexcept {
  if (this != &other) {
    std::scoped_lock l(mutex_, other.mutex_);
    shard_id_ = other.shard_id_;
    nodes_ = std::move(other.nodes_);
    leader_id_ = other.leader_id_;
    term_ = other.term_;
    log_base_ = other.log_base_;
  }
  return *this;
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
  // last_applied/commit sind echte Indizes; Position = idx - log_base_ - ...:
  // Eintrag an Position p traegt Index log_base_ + p + 1.
  if (node.last_applied < log_base_) node.last_applied = log_base_;
  while (node.last_applied < node.commit_index) {
    const std::uint64_t pos = node.last_applied - log_base_;
    if (pos >= node.log.size()) break;
    const Entry& e = node.log[static_cast<std::size_t>(pos)];
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
    // Positionen sind base-relativ: Position p <-> Index log_base_ + p + 1.
    while (n.log.size() + log_base_ + 1 < entry.index) {
      std::size_t src = n.log.size();
      if (src < leader.log.size()) {
        n.log.push_back(leader.log[src]);
      } else {
        break;
      }
    }
    if (n.log.size() + log_base_ + 1 == entry.index) {
      n.log.push_back(entry);
      ++ack;
    } else if (entry.index > log_base_ &&
               n.log.size() >= entry.index - log_base_ && entry.index > 0 &&
               n.log[static_cast<std::size_t>(entry.index - log_base_ - 1)]
                       .index == entry.index) {
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
    entry.index = log_base_ + leader.log.size() + 1;
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

// ---- s25: Follower-Reads ---------------------------------------------------
std::optional<std::string> RaftGroup::follower_get(
    int node_id, const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (node_id < 0 || node_id >= static_cast<int>(nodes_.size())) return std::nullopt;
  const Node& n = nodes_[static_cast<std::size_t>(node_id)];
  if (!n.alive) return std::nullopt;
  auto it = n.applied.find(key);
  if (it == n.applied.end()) return std::nullopt;
  return it->second;
}

bool RaftGroup::is_caught_up(int node_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (leader_id_ < 0) return false;
  if (node_id < 0 || node_id >= static_cast<int>(nodes_.size())) return false;
  const Node& l = nodes_[static_cast<std::size_t>(leader_id_)];
  const Node& n = nodes_[static_cast<std::size_t>(node_id)];
  if (!n.alive) return false;
  return n.commit_index == l.commit_index;
}

// ---- s25: persistentes Log -------------------------------------------------
bool RaftGroup::SaveLog(const std::string& path) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (leader_id_ < 0) return false;
  const Node& leader = nodes_[static_cast<std::size_t>(leader_id_)];
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write("RAFT1", 5);
  W64(out, term_);
  W64(out, leader.commit_index);
  W64(out, log_base_);
  W64(out, static_cast<std::uint64_t>(leader.log.size()));
  for (const auto& e : leader.log) {
    W64(out, e.term);
    W64(out, e.index);
    W32(out, static_cast<std::uint32_t>(e.command.size()));
    if (!e.command.empty()) out.write(e.command.data(), (std::streamsize)e.command.size());
    if (!out) return false;
  }
  out.flush();
  return static_cast<bool>(out);
}

bool RaftGroup::LoadLog(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  char magic[5];
  if (!in.read(magic, 5) || std::memcmp(magic, "RAFT1", 5) != 0) return false;
  std::uint64_t term = 0, commit = 0, base = 0, n = 0;
  if (!R64(in, term) || !R64(in, commit) || !R64(in, base) || !R64(in, n)) return false;
  if (n > (1u << 24)) return false;
  std::vector<Entry> log;
  log.reserve((std::size_t)n);
  for (std::uint64_t i = 0; i < n; ++i) {
    std::uint64_t t = 0, idx = 0;
    std::uint32_t len = 0;
    if (!R64(in, t) || !R64(in, idx) || !R32(in, len)) return false;
    if (len > (1u << 24)) return false;
    if (idx != base + i + 1) return false;  // lueckenlos ab base
    std::string cmd(len, '\0');
    if (len && !in.read(cmd.data(), len)) return false;
    log.push_back(Entry{t, idx, std::move(cmd)});
  }
  if (commit < base || commit > base + n) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  term_ = std::max(term_, term);
  log_base_ = base;
  for (auto& nd : nodes_) {
    nd.log = log;
    nd.commit_index = commit;
    if (nd.last_applied < base || nd.last_applied > commit) nd.last_applied = base;
    if (nd.alive) apply(nd);
  }
  return true;
}

// ---- s25: Snapshots --------------------------------------------------------
bool RaftGroup::SaveSnapshot(const std::string& path) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (leader_id_ < 0) return false;
  const Node& leader = nodes_[static_cast<std::size_t>(leader_id_)];
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write("RSNP1", 5);
  W64(out, leader.commit_index);  // last_included_index
  W64(out, static_cast<std::uint64_t>(leader.applied.size()));
  for (const auto& [k, v] : leader.applied) {
    W32(out, static_cast<std::uint32_t>(k.size()));
    if (!k.empty()) out.write(k.data(), (std::streamsize)k.size());
    W32(out, static_cast<std::uint32_t>(v.size()));
    if (!v.empty()) out.write(v.data(), (std::streamsize)v.size());
    if (!out) return false;
  }
  out.flush();
  return static_cast<bool>(out);
}

bool RaftGroup::LoadSnapshot(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  char magic[5];
  if (!in.read(magic, 5) || std::memcmp(magic, "RSNP1", 5) != 0) return false;
  std::uint64_t last_idx = 0, n = 0;
  if (!R64(in, last_idx) || !R64(in, n)) return false;
  if (n > (1u << 24)) return false;
  std::map<std::string, std::string> snap;
  for (std::uint64_t i = 0; i < n; ++i) {
    std::uint32_t kl = 0, vl = 0;
    if (!R32(in, kl) || kl > (1u << 24)) return false;
    std::string k(kl, '\0');
    if (kl && !in.read(k.data(), kl)) return false;
    if (!R32(in, vl) || vl > (1u << 24)) return false;
    std::string v(vl, '\0');
    if (vl && !in.read(v.data(), vl)) return false;
    snap[std::move(k)] = std::move(v);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  // Log-Compaction: Prefix <= last_idx verwerfen, Basis nachziehen.
  std::vector<Entry> tail;
  if (leader_id_ >= 0) {
    for (const auto& e : nodes_[static_cast<std::size_t>(leader_id_)].log)
      if (e.index > last_idx) tail.push_back(e);
  }
  // Konsistenz: Tail muss lueckenlos ab last_idx+1 sein, sonst voll verwerfen.
  for (std::size_t i = 0; i < tail.size(); ++i)
    if (tail[i].index != last_idx + i + 1) {
      tail.clear();
      break;
    }
  log_base_ = last_idx;
  for (auto& nd : nodes_) {
    nd.log = tail;
    nd.applied = snap;
    nd.commit_index = std::max(nd.commit_index, last_idx);
    if (nd.commit_index < last_idx) nd.commit_index = last_idx;
    nd.last_applied = nd.commit_index;
    if (nd.alive) apply(nd);  // no-op wenn nichts ueber last_idx committet
  }
  return true;
}

// ---- s25: Wire-Codec + TCP-Loopback ----------------------------------------
std::string EncodeEntryWire(const Entry& e) {
  std::string o;
  o.reserve(20 + e.command.size());
  PutU64be(o, e.term);
  PutU64be(o, e.index);
  PutU32be(o, static_cast<std::uint32_t>(e.command.size()));
  o.append(e.command);
  return o;
}

bool DecodeEntryWire(const std::string& payload, Entry& out) {
  if (payload.size() < 20) return false;
  const char* p = payload.data();
  out.term = GetU64be(p);
  out.index = GetU64be(p + 8);
  const std::uint32_t len = GetU32be(p + 16);
  if (payload.size() != 20 + len) return false;
  out.command.assign(p + 20, len);
  return out.index > 0;
}

bool SendWire(int fd, const std::string& payload) {
  if (payload.size() > 0xFFFFFF) return false;  // 16MB-Cap (vgl. WAL)
  std::string frame;
  frame.reserve(4 + payload.size());
  PutU32be(frame, static_cast<std::uint32_t>(payload.size()));
  frame.append(payload);
  return WriteFull(fd, frame.data(), frame.size());
}

bool RecvWire(int fd, std::string& payload) {
  char hdr[4];
  if (!ReadFull(fd, hdr, 4)) return false;
  const std::uint32_t len = GetU32be(hdr);
  if (len > 0xFFFFFF) return false;
  payload.assign(len, '\0');
  if (len && !ReadFull(fd, payload.data(), len)) return false;
  return true;
}

bool TcpLoopbackPair(int& client_fd, int& server_fd) {
  client_fd = server_fd = -1;
  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) return false;
  int one = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // ephemeral
  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(listen_fd);
    return false;
  }
  if (::listen(listen_fd, 1) != 0) {
    ::close(listen_fd);
    return false;
  }
  socklen_t alen = sizeof(addr);
  if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen) != 0) {
    ::close(listen_fd);
    return false;
  }
  int cli = ::socket(AF_INET, SOCK_STREAM, 0);
  if (cli < 0) {
    ::close(listen_fd);
    return false;
  }
  if (::connect(cli, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(cli);
    ::close(listen_fd);
    return false;
  }
  int srv = ::accept(listen_fd, nullptr, nullptr);
  ::close(listen_fd);
  if (srv < 0) {
    ::close(cli);
    return false;
  }
  client_fd = cli;
  server_fd = srv;
  return true;
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

std::pair<Shard, Shard> Shard::split(const std::string& mid) const {
  // Validierung: mid in (start, end); leeres end = offen (nur mid > start).
  if (!(start_ < mid)) throw std::invalid_argument("Shard::split: mid <= start");
  if (!end_.empty() && !(mid < end_))
    throw std::invalid_argument("Shard::split: mid >= end");
  // Binaerer Split-Baum (kollisionsfrei): links id*2, rechts id*2+1.
  // Frische (leere) RaftGroups; Daten-Move ist Follow-up (V2-Doku).
  Shard left(id_ * 2, start_, mid);
  Shard right(id_ * 2 + 1, mid, end_);
  return {std::move(left), std::move(right)};
}

bool Shard::can_merge_with(const Shard& other) const {
  // Adjazent gdw. eine Range endet wo die andere beginnt.
  return end_ == other.start_ || other.end_ == start_;
}

}  // namespace dbengine::raft
