// raft_cluster: echter 3-Prozess-Raft-Cluster via fork() + TCP-Loopback.
// Jeder Kindprozess ist ein Raft-Knoten mit eigenem Term/Log/State (kein
// zweites Consensus-Protokoll: nur RequestVote/AppendEntries-RPCs, Framing
// via SendWire/RecvWire aus dbengine/raft/shard.h, Entry-Wire-Codec fuer
// Log-Eintraege). Wahl-Timeouts gestaffelt wie
// RaftGroup::election_timeout_for, Heartbeats (40ms) vom Leader.
// Selfcheck (--selfcheck, deterministisch): 3 Prozesse starten, Leader-Wahl
// abwarten, Client-Put an Leader, Replikation auf Mehrheit verifizieren,
// einen Follower killen + weiter committen. Exit 0/1 mit PASS/FAIL-Zeilen.
// POSIX-only, STL + Sockets, keine externen Deps.

#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "dbengine/raft/shard.h"

using dbengine::raft::DecodeEntryWire;
using dbengine::raft::EncodeEntryWire;
using dbengine::raft::Entry;
using dbengine::raft::RaftGroup;
using dbengine::raft::RecvWire;
using dbengine::raft::SendWire;

namespace {

constexpr int kNodes = 3;
constexpr std::uint64_t kHeartbeatMs = 40;
constexpr int kRpcTimeoutMs = 300;
constexpr std::uint64_t kNoLeaderEnc = 0xFFFFFFFFFFFFFFFFULL;

void SleepMs(std::uint64_t ms) {
  timespec ts{};
  ts.tv_sec = static_cast<time_t>(ms / 1000U);
  ts.tv_nsec = static_cast<long>((ms % 1000U) * 1000000U);
  ::nanosleep(&ts, nullptr);
}

std::uint64_t NowMs() {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000U +
         static_cast<std::uint64_t>(ts.tv_nsec) / 1000000U;
}

void PutU64(std::string& o, std::uint64_t v) {
  for (int i = 7; i >= 0; --i)
    o.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
}

void PutU32(std::string& o, std::uint32_t v) {
  for (int i = 3; i >= 0; --i)
    o.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
}

bool GetU64(const std::string& s, std::size_t& off, std::uint64_t& v) {
  if (off + 8 > s.size()) return false;
  v = 0;
  for (int i = 0; i < 8; ++i)
    v = (v << 8U) | static_cast<unsigned char>(s[off + static_cast<std::size_t>(i)]);
  off += 8;
  return true;
}

bool GetU32(const std::string& s, std::size_t& off, std::uint32_t& v) {
  if (off + 4 > s.size()) return false;
  v = 0;
  for (int i = 0; i < 4; ++i)
    v = (v << 8U) | static_cast<unsigned char>(s[off + static_cast<std::size_t>(i)]);
  off += 4;
  return true;
}

bool GetByte(const std::string& s, std::size_t& off, unsigned char& v) {
  if (off + 1 > s.size()) return false;
  v = static_cast<unsigned char>(s[off]);
  off += 1;
  return true;
}

std::uint64_t EncLeader(int id) {
  return id < 0 ? kNoLeaderEnc : static_cast<std::uint64_t>(id);
}

int DecLeader(std::uint64_t v) {
  return v == kNoLeaderEnc ? -1 : static_cast<int>(v);
}

void SetSockTimeout(int fd, int ms) {
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// Ein RPC = frische Loopback-Verbindung: connect, SendWire(req),
// RecvWire(resp), close. Kein persistenter Mesh noetig.
bool RpcImpl(std::uint16_t port, const std::string& req, std::string& resp,
             int timeout_ms) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  SetSockTimeout(fd, timeout_ms);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
            SendWire(fd, req) && RecvWire(fd, resp);
  ::close(fd);
  return ok;
}

bool Rpc(std::uint16_t port, const std::string& req, std::string& resp) {
  return RpcImpl(port, req, resp, kRpcTimeoutMs);
}

bool ListenOnLoopback(int& fd_out, std::uint16_t& port_out) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 16) != 0) {
    ::close(fd);
    return false;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return false;
  }
  fd_out = fd;
  port_out = ntohs(addr.sin_port);
  return true;
}

// Mini-Command-Format wie RaftGroup: "put <k> <v>" / "del <k>".
bool ParsePut(const std::string& cmd, std::string& key, std::string& val) {
  if (cmd.compare(0, 4, "put ") != 0) return false;
  std::size_t sp = cmd.find(' ', 4);
  if (sp == std::string::npos || sp == 4) return false;
  key = cmd.substr(4, sp - 4);
  val = cmd.substr(sp + 1);
  return true;
}

bool ParseDel(const std::string& cmd, std::string& key) {
  if (cmd.compare(0, 4, "del ") != 0 || cmd.size() <= 4) return false;
  key = cmd.substr(4);
  return true;
}

enum class Role : std::uint8_t { Follower, Candidate, Leader };

struct NodeState {
  int id = -1;
  int listen_fd = -1;
  std::uint16_t peers[kNodes]{};
  Role role = Role::Follower;
  std::uint64_t term = 0;
  int voted_for = -1;
  int leader_known = -1;
  std::vector<Entry> log;  // 1-basiert, lueckenlos, Index = Position+1
  std::uint64_t commit = 0;
  std::uint64_t applied_idx = 0;
  std::map<std::string, std::string> applied;
  std::uint64_t next_idx[kNodes]{};
  std::uint64_t match_idx[kNodes]{};
  std::uint64_t deadline_ms = 0;
  std::uint64_t next_hb_ms = 0;
  bool running = true;
};

std::uint64_t LastIdx(const NodeState& s) { return s.log.size(); }

std::uint64_t LastTerm(const NodeState& s) {
  return s.log.empty() ? 0 : s.log.back().term;
}

std::uint64_t TimeoutFor(int id) { return RaftGroup::election_timeout_for(id); }

void ResetDeadline(NodeState& s) { s.deadline_ms = NowMs() + TimeoutFor(s.id); }

void StepDown(NodeState& s, std::uint64_t new_term) {
  s.term = new_term;
  s.role = Role::Follower;
  s.voted_for = -1;
  s.leader_known = -1;
  ResetDeadline(s);
}

void ApplyEntries(NodeState& s) {
  while (s.applied_idx < s.commit && s.applied_idx < s.log.size()) {
    const Entry& e = s.log[static_cast<std::size_t>(s.applied_idx)];
    std::string key, val;
    if (ParsePut(e.command, key, val)) {
      s.applied[key] = val;
    } else if (ParseDel(e.command, key)) {
      s.applied.erase(key);
    }
    ++s.applied_idx;
  }
}

// --- Nachrichten ----------------------------------------------------------
// 'V' RV-Req: term cand lastIdx lastTerm | 'v' RV-Rep: term granted
// 'A' AE-Req: term leader prevIdx prevTerm leaderCommit elen entry | 'a': term ok
// 'C' ClientPut: klen key vlen val | 'c': ok index leaderHint
// 'G' ClientGet: klen key | 'g': found vlen val
// 'S' Status | 's': term leader commit logsize isLeader
// 'X' Shutdown | 'x': 1

std::string BuildRvReq(const NodeState& s) {
  std::string o = "V";
  PutU64(o, s.term);
  PutU64(o, static_cast<std::uint64_t>(s.id));
  PutU64(o, LastIdx(s));
  PutU64(o, LastTerm(s));
  return o;
}

std::string BuildAeReq(const NodeState& s, int peer, bool& has_entry,
                       std::uint64_t& sent_idx) {
  std::string o = "A";
  PutU64(o, s.term);
  PutU64(o, static_cast<std::uint64_t>(s.id));
  std::uint64_t nxt = s.next_idx[peer];
  if (nxt < 1) nxt = 1;
  std::uint64_t prev = nxt - 1;
  std::uint64_t prev_term = 0;
  if (prev > 0 && prev <= s.log.size())
    prev_term = s.log[static_cast<std::size_t>(prev - 1)].term;
  PutU64(o, prev);
  PutU64(o, prev_term);
  PutU64(o, s.commit);
  std::string entry_bytes;
  has_entry = false;
  sent_idx = 0;
  if (nxt <= s.log.size()) {
    entry_bytes = EncodeEntryWire(s.log[static_cast<std::size_t>(nxt - 1)]);
    has_entry = true;
    sent_idx = nxt;
  }
  PutU32(o, static_cast<std::uint32_t>(entry_bytes.size()));
  o.append(entry_bytes);
  return o;
}

void TryAdvanceCommit(NodeState& s) {
  std::uint64_t last = LastIdx(s);
  for (std::uint64_t n = last; n > s.commit; --n) {
    int have = 1;
    for (int p = 0; p < kNodes; ++p) {
      if (p != s.id && s.match_idx[p] >= n) ++have;
    }
    if (have >= 2) {
      s.commit = n;
      break;
    }
  }
  ApplyEntries(s);
}

void Broadcast(NodeState& s) {
  for (int p = 0; p < kNodes; ++p) {
    if (p == s.id) continue;
    bool has_entry = false;
    std::uint64_t sent_idx = 0;
    std::string req = BuildAeReq(s, p, has_entry, sent_idx);
    std::string resp;
    if (!Rpc(s.peers[p], req, resp) || resp.size() < 10 || resp[0] != 'a')
      continue;
    std::size_t off = 1;
    std::uint64_t rterm = 0;
    unsigned char ok = 0;
    if (!GetU64(resp, off, rterm) || !GetByte(resp, off, ok)) continue;
    if (rterm > s.term) {
      StepDown(s, rterm);
      return;
    }
    if (s.role != Role::Leader) return;
    if (ok == 1) {
      if (has_entry) {
        s.match_idx[p] = sent_idx;
        s.next_idx[p] = sent_idx + 1;
      } else {
        if (s.match_idx[p] < LastIdx(s) && s.next_idx[p] <= LastIdx(s) + 1) {
          // Heartbeat-Ack: nichts zu tun.
        }
      }
    } else if (s.next_idx[p] > 1) {
      --s.next_idx[p];
    }
  }
  if (s.role == Role::Leader) TryAdvanceCommit(s);
  s.next_hb_ms = NowMs() + kHeartbeatMs;
}

void BecomeLeader(NodeState& s) {
  s.role = Role::Leader;
  s.leader_known = s.id;
  std::uint64_t last = LastIdx(s);
  for (int p = 0; p < kNodes; ++p) {
    s.next_idx[p] = last + 1;
    s.match_idx[p] = 0;
  }
  s.next_hb_ms = 0;  // sofort Heartbeat
}

void StartElection(NodeState& s) {
  ++s.term;
  s.role = Role::Candidate;
  s.voted_for = s.id;
  s.leader_known = -1;
  ResetDeadline(s);
  int votes = 1;
  std::string req = BuildRvReq(s);
  for (int p = 0; p < kNodes; ++p) {
    if (p == s.id) continue;
    std::string resp;
    if (!Rpc(s.peers[p], req, resp) || resp.size() < 10 || resp[0] != 'v')
      continue;
    std::size_t off = 1;
    std::uint64_t rterm = 0;
    unsigned char granted = 0;
    if (!GetU64(resp, off, rterm) || !GetByte(resp, off, granted)) continue;
    if (rterm > s.term) {
      StepDown(s, rterm);
      return;
    }
    if (granted == 1 && s.role == Role::Candidate) ++votes;
  }
  if (s.role == Role::Candidate && votes >= 2) BecomeLeader(s);
}

bool LogUpToDate(const NodeState& s, std::uint64_t cand_last_term,
                 std::uint64_t cand_last_idx) {
  if (cand_last_term != LastTerm(s)) return cand_last_term > LastTerm(s);
  return cand_last_idx >= LastIdx(s);
}

std::string HandleRv(NodeState& s, const std::string& req) {
  std::size_t off = 1;
  std::uint64_t rterm = 0, cand = 0, last_idx = 0, last_term = 0;
  unsigned char granted = 0;
  if (GetU64(req, off, rterm) && GetU64(req, off, cand) &&
      GetU64(req, off, last_idx) && GetU64(req, off, last_term) &&
      off == req.size()) {
    int cand_id = static_cast<int>(cand);
    if (rterm > s.term) StepDown(s, rterm);
    if (rterm == s.term && cand_id >= 0 && cand_id < kNodes &&
        (s.voted_for == -1 || s.voted_for == cand_id) &&
        LogUpToDate(s, last_term, last_idx)) {
      s.voted_for = cand_id;
      s.role = Role::Follower;
      granted = 1;
      ResetDeadline(s);
    }
  }
  std::string o = "v";
  PutU64(o, s.term);
  o.push_back(static_cast<char>(granted));
  return o;
}

std::string HandleAe(NodeState& s, const std::string& req) {
  std::size_t off = 1;
  std::uint64_t rterm = 0, leader = 0, prev = 0, prev_term = 0, lcommit = 0;
  std::uint32_t elen = 0;
  unsigned char ok = 0;
  if (GetU64(req, off, rterm) && GetU64(req, off, leader) &&
      GetU64(req, off, prev) && GetU64(req, off, prev_term) &&
      GetU64(req, off, lcommit) && GetU32(req, off, elen) &&
      off + elen == req.size()) {
    int lid = static_cast<int>(leader);
    if (rterm >= s.term && lid >= 0 && lid < kNodes) {
      if (rterm > s.term) {
        StepDown(s, rterm);
      } else {
        s.role = Role::Follower;
      }
      s.leader_known = lid;
      ResetDeadline(s);
      bool consistent = (prev == 0) ||
                        (prev <= s.log.size() &&
                         s.log[static_cast<std::size_t>(prev - 1)].term == prev_term);
      if (consistent) {
        if (elen > 0) {
          Entry e;
          if (DecodeEntryWire(req.substr(off, elen), e) && e.index == prev + 1) {
            // Konflikt-Suffix kappen, dann anhaengen.
            while (s.log.size() > prev) s.log.pop_back();
            if (s.log.size() == prev) s.log.push_back(e);
            consistent = s.log.size() > prev;
          } else {
            consistent = false;
          }
        }
        if (consistent) {
          if (lcommit > s.commit)
            s.commit = lcommit < LastIdx(s) ? lcommit : LastIdx(s);
          ApplyEntries(s);
          ok = 1;
        }
      }
    }
  }
  std::string o = "a";
  PutU64(o, s.term);
  o.push_back(static_cast<char>(ok));
  return o;
}

std::string HandleClientPut(NodeState& s, const std::string& req) {
  std::size_t off = 1;
  std::uint32_t klen = 0, vlen = 0;
  unsigned char ok = 0;
  std::uint64_t idx = 0;
  if (GetU32(req, off, klen) && off + klen <= req.size()) {
    std::string key = req.substr(off, klen);
    off += klen;
    if (GetU32(req, off, vlen) && off + vlen == req.size()) {
      std::string val = req.substr(off, vlen);
      if (!key.empty() && s.role == Role::Leader) {
        Entry e{s.term, LastIdx(s) + 1, "put " + key + " " + val};
        s.log.push_back(e);
        idx = e.index;
        // Synchron replizieren, aber zeitbegrenzt (Client wartet mit
        // eigenem langem Timeout; Rest holt der naechste Heartbeat).
        std::uint64_t end = NowMs() + 4000;
        while (s.role == Role::Leader && s.commit < idx && NowMs() < end) {
          Broadcast(s);
          if (s.commit >= idx) break;
          SleepMs(10);
        }
        if (s.commit >= idx) ok = 1;
      }
    }
  }
  std::string o = "c";
  o.push_back(static_cast<char>(ok));
  PutU64(o, idx);
  PutU64(o, EncLeader(s.leader_known));
  return o;
}

std::string HandleClientGet(NodeState& s, const std::string& req) {
  std::size_t off = 1;
  std::uint32_t klen = 0;
  unsigned char found = 0;
  std::string val;
  if (GetU32(req, off, klen) && off + klen == req.size()) {
    auto it = s.applied.find(req.substr(off, klen));
    if (it != s.applied.end()) {
      found = 1;
      val = it->second;
    }
  }
  std::string o = "g";
  o.push_back(static_cast<char>(found));
  PutU32(o, static_cast<std::uint32_t>(val.size()));
  o.append(val);
  return o;
}

std::string HandleStatus(const NodeState& s) {
  std::string o = "s";
  PutU64(o, s.term);
  PutU64(o, EncLeader(s.leader_known));
  PutU64(o, s.commit);
  PutU64(o, LastIdx(s));
  o.push_back(static_cast<char>(s.role == Role::Leader ? 1 : 0));
  return o;
}

void NodeMain(int id, int listen_fd, const std::uint16_t* peer_ports) {
  ::signal(SIGPIPE, SIG_IGN);
  NodeState s;
  s.id = id;
  s.listen_fd = listen_fd;
  for (int i = 0; i < kNodes; ++i) s.peers[i] = peer_ports[i];
  ResetDeadline(s);

  while (s.running) {
    fd_set rfds{};
    FD_ZERO(&rfds);
    FD_SET(listen_fd, &rfds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 10000;  // 10ms Takt: Timer-Granularitaet << Timeouts
    int r = ::select(listen_fd + 1, &rfds, nullptr, nullptr, &tv);
    if (r > 0 && FD_ISSET(listen_fd, &rfds)) {
      int cfd = ::accept(listen_fd, nullptr, nullptr);
      if (cfd >= 0) {
        SetSockTimeout(cfd, kRpcTimeoutMs);
        std::string req;
        if (RecvWire(cfd, req) && !req.empty()) {
          std::string resp;
          switch (req[0]) {
            case 'V': resp = HandleRv(s, req); break;
            case 'A': resp = HandleAe(s, req); break;
            case 'C': resp = HandleClientPut(s, req); break;
            case 'G': resp = HandleClientGet(s, req); break;
            case 'S': resp = HandleStatus(s); break;
            case 'X': resp = "x1"; s.running = false; break;
            default: resp.clear(); break;
          }
          if (!resp.empty()) (void)SendWire(cfd, resp);
        }
        ::close(cfd);
      }
    }
    std::uint64_t now = NowMs();
    if (s.role == Role::Leader) {
      if (now >= s.next_hb_ms) Broadcast(s);
    } else if (now >= s.deadline_ms) {
      StartElection(s);
      if (s.role == Role::Leader) Broadcast(s);
    }
  }
  ::close(listen_fd);
}

// --- Parent / Selfcheck ----------------------------------------------------

int g_failures = 0;

void Check(bool cond, const std::string& name) {
  if (cond) {
    std::cout << "PASS " << name << "\n";
  } else {
    std::cout << "FAIL " << name << "\n";
    ++g_failures;
  }
  std::cout.flush();
}

struct Status {
  bool ok = false;
  std::uint64_t term = 0;
  int leader = -1;
  std::uint64_t commit = 0;
  std::uint64_t logsize = 0;
  bool is_leader = false;
};

Status QueryStatus(std::uint16_t port) {
  Status st;
  std::string resp;
  if (!Rpc(port, "S", resp) || resp.size() != 34 || resp[0] != 's')
    return st;
  std::size_t off = 1;
  std::uint64_t leader_enc = 0, commit = 0, logsize = 0, term = 0;
  unsigned char is_leader = 0;
  if (!GetU64(resp, off, term) || !GetU64(resp, off, leader_enc) ||
      !GetU64(resp, off, commit) || !GetU64(resp, off, logsize) ||
      !GetByte(resp, off, is_leader))
    return st;
  st.ok = true;
  st.term = term;
  st.leader = DecLeader(leader_enc);
  st.commit = commit;
  st.logsize = logsize;
  st.is_leader = is_leader == 1;
  return st;
}

struct PutResult {
  bool ok = false;
  std::uint64_t index = 0;
  int hint = -1;
};

PutResult ClientPut(std::uint16_t port, const std::string& key,
                    const std::string& val) {
  PutResult r;
  std::string req = "C";
  PutU32(req, static_cast<std::uint32_t>(key.size()));
  req.append(key);
  PutU32(req, static_cast<std::uint32_t>(val.size()));
  req.append(val);
  std::string resp;
  // Langes Timeout: Leader repliziert synchron (bis ~4s) vor der Antwort.
  if (!RpcImpl(port, req, resp, 8000) || resp.size() != 18 || resp[0] != 'c')
    return r;
  std::size_t off = 1;
  unsigned char ok = 0;
  std::uint64_t idx = 0, hint_enc = 0;
  if (!GetByte(resp, off, ok) || !GetU64(resp, off, idx) ||
      !GetU64(resp, off, hint_enc))
    return r;
  r.ok = ok == 1;
  r.index = idx;
  r.hint = DecLeader(hint_enc);
  return r;
}

std::optional<std::string> ClientGet(std::uint16_t port, const std::string& key) {
  std::string req = "G";
  PutU32(req, static_cast<std::uint32_t>(key.size()));
  req.append(key);
  std::string resp;
  if (!Rpc(port, req, resp) || resp.size() < 6 || resp[0] != 'g')
    return std::nullopt;
  std::size_t off = 1;
  unsigned char found = 0;
  std::uint32_t vlen = 0;
  if (!GetByte(resp, off, found) || !GetU32(resp, off, vlen) ||
      off + vlen != resp.size())
    return std::nullopt;
  if (found != 1) return std::nullopt;
  return resp.substr(off, vlen);
}

// Put mit Leader-Redirect (max. 3 Hops).
PutResult PutViaLeader(const std::uint16_t* ports, int leader_hint,
                       const std::string& key, const std::string& val) {
  int target = leader_hint;
  for (int hop = 0; hop < 3; ++hop) {
    if (target < 0 || target >= kNodes) return {};
    PutResult r = ClientPut(ports[target], key, val);
    if (r.ok) return r;
    if (r.hint < 0 || r.hint == target) return r;
    target = r.hint;
  }
  return {};
}

// Stabiler Leader: K meldet is_leader UND ein anderer Knoten kennt K,
// in zwei Runden (100ms Abstand) derselbe.
int WaitLeader(const std::uint16_t* ports, std::uint64_t timeout_ms) {
  std::uint64_t start = NowMs();
  int stable = -1;
  while (NowMs() - start < timeout_ms) {
    int cand = -1;
    for (int i = 0; i < kNodes; ++i) {
      Status st = QueryStatus(ports[i]);
      if (st.ok && st.is_leader) {
        cand = i;
        break;
      }
    }
    if (cand >= 0) {
      bool confirmed = false;
      for (int j = 0; j < kNodes; ++j) {
        if (j == cand) continue;
        Status o = QueryStatus(ports[j]);
        if (o.ok && o.leader == cand) {
          confirmed = true;
          break;
        }
      }
      if (confirmed) {
        if (stable == cand) return cand;
        stable = cand;
        SleepMs(100);
        continue;
      }
    }
    stable = -1;
    SleepMs(50);
  }
  return -1;
}

// Warte bis mindestens 2 (Mehrheit bei 3 Knoten) key==val lesen.
bool WaitMajorityValue(const std::uint16_t* ports, const bool* alive,
                       const std::string& key, const std::string& val,
                       std::uint64_t timeout_ms) {
  std::uint64_t start = NowMs();
  while (NowMs() - start < timeout_ms) {
    int have = 0;
    for (int i = 0; i < kNodes; ++i) {
      if (!alive[i]) continue;
      auto got = ClientGet(ports[i], key);
      if (got.has_value() && *got == val) ++have;
    }
    if (have >= 2) return true;
    SleepMs(50);
  }
  return false;
}

void ShutdownNode(std::uint16_t port) {
  std::string resp;
  (void)Rpc(port, "X", resp);
}

int Selfcheck() {
  ::signal(SIGPIPE, SIG_IGN);
  int listen_fds[kNodes]{-1, -1, -1};
  std::uint16_t ports[kNodes]{};
  for (int i = 0; i < kNodes; ++i) {
    if (!ListenOnLoopback(listen_fds[i], ports[i])) {
      std::cout << "FAIL cluster/listen-" << i << "\n";
      for (int j = 0; j < i; ++j) ::close(listen_fds[j]);
      return 1;
    }
  }

  pid_t pids[kNodes]{-1, -1, -1};
  std::cout.flush();
  for (int i = 0; i < kNodes; ++i) {
    pid_t pid = ::fork();
    if (pid < 0) {
      std::cout << "FAIL cluster/fork-" << i << "\n";
      for (int j = 0; j < kNodes; ++j) {
        if (pids[j] > 0) {
          ::kill(pids[j], SIGKILL);
          int st = 0;
          ::waitpid(pids[j], &st, 0);
        }
      }
      for (int j = 0; j < kNodes; ++j) {
        if (listen_fds[j] >= 0) ::close(listen_fds[j]);
      }
      return 1;
    }
    if (pid == 0) {
      for (int j = 0; j < kNodes; ++j) {
        if (j != i) ::close(listen_fds[j]);
      }
      NodeMain(i, listen_fds[i], ports);
      ::_exit(0);
    }
    pids[i] = pid;
  }
  for (int i = 0; i < kNodes; ++i) ::close(listen_fds[i]);
  Check(true, "cluster/3-started");

  bool alive[kNodes]{true, true, true};

  int leader = WaitLeader(ports, 10000);
  Check(leader >= 0, "cluster/leader-elected");
  if (leader >= 0) {
    Status lst = QueryStatus(ports[leader]);
    std::cout << "INFO leader=" << leader << " term=" << lst.term << "\n";
  }
  if (leader < 0) leader = 0;  // best effort fortsetzen

  PutResult p1 = PutViaLeader(ports, leader, "raft_selfcheck_k1", "v1");
  Check(p1.ok && p1.index > 0, "cluster/put-k1-committed");
  {
    Status cur = QueryStatus(ports[leader]);
    if (cur.ok && cur.leader >= 0) leader = cur.leader;
  }

  Check(WaitMajorityValue(ports, alive, "raft_selfcheck_k1", "v1", 5000),
        "cluster/replication-majority");

  // Einen Follower killen (deterministisch: kleinste Id != Leader).
  int victim = -1;
  {
    Status lst = QueryStatus(ports[leader]);
    int cur = (lst.ok && lst.leader >= 0) ? lst.leader : leader;
    for (int i = 0; i < kNodes; ++i) {
      if (i != cur) {
        victim = i;
        break;
      }
    }
    leader = cur;
  }
  bool killed = false;
  if (victim >= 0) {
    ::kill(pids[victim], SIGKILL);
    int st = 0;
    (void)::waitpid(pids[victim], &st, 0);  // Tod per waitpid bestaetigt
    pids[victim] = -1;
    alive[victim] = false;
    killed = true;
  }
  Check(killed, "cluster/follower-killed");

  // Weiter committen mit 2/3 (Mehrheit): Leader neu aufloesen, Put, Mehrheit.
  int leader2 = WaitLeader(ports, 5000);
  if (leader2 < 0) leader2 = leader;
  PutResult p2 = PutViaLeader(ports, leader2, "raft_selfcheck_k2", "v2");
  Check(p2.ok && p2.index > 0, "cluster/put-k2-after-kill");
  Check(WaitMajorityValue(ports, alive, "raft_selfcheck_k2", "v2", 5000),
        "cluster/commit-majority-after-kill");

  for (int i = 0; i < kNodes; ++i) {
    if (pids[i] > 0) ShutdownNode(ports[i]);
  }
  SleepMs(200);
  for (int i = 0; i < kNodes; ++i) {
    if (pids[i] <= 0) continue;
    int st = 0;
    pid_t r = ::waitpid(pids[i], &st, WNOHANG);
    if (r != pids[i]) {
      ::kill(pids[i], SIGKILL);
      (void)::waitpid(pids[i], &st, 0);
    }
  }

  if (g_failures == 0) {
    std::cout << "ALL RAFT_CLUSTER SELFCHECK PASSED\n";
    return 0;
  }
  std::cout << g_failures << " RAFT_CLUSTER SELFCHECK FAILED\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--selfcheck") return Selfcheck();
  std::cerr << "Usage: raft_cluster --selfcheck\n";
  return 2;
}
