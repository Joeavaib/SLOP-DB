#pragma once

// Raft-Shard Stub (s13-raft): Range/Tablet + 3er-Raft-Gruppe in-process.
// Kein echtes Netzwerk: Replikation = synchroner Methodenaufruf (Sim).
// Leader-Election via term/vote (Mehrheit), Log-Replikation mit
// replicateToFollowers, commitIndex/apply in eine KV-State-Machine.
// Failover: killLeader + failover() waehlt deterministisch den naechsten
// lebenden Knoten — in-process in Mikrosekunden, also <100ms sim.
// Timer-Election (Fake-Clock, Opt-in Auto-Modus): tick(now_ms) ersetzt im
// Auto-Modus das manuelle killLeader/failover — Baustein Richtung Netz-Raft
// (Heartbeat-Timeout -> Candidate -> electLeader, kein Thread/keine Echtzeit
// im Core, Zeit kommt als Parameter).
//
// Partition/Drop-Matrix (Prodsim, in-process): 3x3 Konnektivitaet, Default
// fully meshed, damit Bestandstest test_raft unveraendert grueng bleibt.
// isolate/heal sind permanente Cuts; set_drop_rate ist Verlust pro Aufruf
// (deterministischer PRNG, Seed 42). FoundationDB/Jepsen-Simulator-Stil,
// kein netem/tc und keine echten NICs — TCP-Loopback siehe TcpLoopbackPair
// und tools/prodsim_cluster.cpp.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
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
  // sXX Timer-Election (Fake-Clock, volatil, keine Persistenz): letzter
  // gesehener Leader-Heartbeat in ms, Timeout je Knoten deterministisch
  // 150-300ms aus node_id-Hash (kein RNG). Nur im Auto-Modus aktiv.
  std::uint64_t last_heartbeat_ms = 0;
  std::uint64_t election_timeout_ms = 150;
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
  // Probiert lebende Kandidaten in Id-Reihenfolge. Ein Vote von V zaehlt
  // nur wenn V alive und can_send(V,c) && can_send(c,V) (symmetrische
  // Partition). Default-Mesh: erster Lebender, alle Lebenden voten — wie
  // vor der Partition-API. Gibt Leader-Id zurueck, -1 ohne Mehrheit (>=2).
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
  void reviveNode(int node_id);  // Catch-up nur wenn Leader den Knoten erreicht

  // ---- Partition / Drop (in-process Fault-Injector) -----------------------
  // isolate(a,b): symmetrischer Cut, beide Richtungen. Node bleibt alive
  // (im Gegensatz zu killNode = Prozess-Crash). a==b wird ignoriert.
  // heal(a,b): symmetrisches Heilen + Catch-up erreichbarer Knoten; a==b
  // wird ignoriert. heal_all(): volle Mesh + Drop-Raten zuruecksetzen
  // (raeumt auch drops) + Catch-up.
  // set_drop_rate: 0..100, Verlust nur fuer diesen Call, kein Dauer-Cut.
  // can_send: Topology + Drop; Self-Send (from==to) immer true.
  void isolate(int a, int b);
  void heal(int a, int b);
  void isolate_from_all(int node_id);
  void heal_all();
  void set_drop_rate(int from, int to, int pct_0_100);
  void set_net_seed(std::uint32_t seed);
  [[nodiscard]] bool can_send(int from, int to) const;

  // ---- sXX: Timer-Election (Fake-Clock, Opt-in Auto-Modus) ------------------
  // Baustein Richtung Netz-Raft: ersetzt im Auto-Modus das manuelle
  // killLeader/failover. Fake-Clock: now_ms kommt als Parameter (kein Thread,
  // keine echte Zeit im Core, deterministisch testbar). Default AUS, damit
  // alle bestehenden Sim/Chaos-Sequenzen unveraendert gruen bleiben.
  // tick(now_ms): Leader lebend -> Heartbeats (last_heartbeat aller lebenden
  //   Knoten = now, KEIN Log-Eintrag). Follower ohne Heartbeat nach Timeout
  //   (now - last >= election_timeout) -> Candidate via electLeader-Logik
  //   (term++, Mehrheit), Heartbeats auf now resyncen. Gibt Leader-Id zurueck.
  // heartbeat(node_id, now_ms): empfangener Heartbeat -> last_heartbeat setzen.
  // enable_auto_election(now_ms): Auto-Modus an + alle last_heartbeat = now.
  // election_timeout_for(node_id): 150 + ((node_id*67+101) % 151), fix 150-300.
  static std::uint64_t election_timeout_for(int node_id) noexcept;
  void enable_auto_election(std::uint64_t now_ms);
  void disable_auto_election() noexcept;
  [[nodiscard]] bool auto_election() const;
  [[nodiscard]] std::uint64_t last_heartbeat_ms(int node_id) const;
  [[nodiscard]] std::uint64_t election_timeout_ms(int node_id) const;
  void heartbeat(int node_id, std::uint64_t now_ms);
  int tick(std::uint64_t now_ms);

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
  // Format "RAFT1": term u64, commit u64, base u64, n u64,
  // je Entry term/index/cmd.
  // Save: Leader-Log + term/commit, crash-sicher via tmp-File + rename +
  // fsync (File + Directory), analog storage/wal.cpp. Laesst bestehende
  // Zieldatei bei Fehler unversehrt. Load: stellt Log auf allen Knoten
  // wieder her (commit/apply). Gibt false bei IO-/Formatfehler.
  // Volatil: Save/Load beruehren die Partition/Drop-Matrix (partitioned_,
  // drop_pct_, net_rng_) NICHT — Netz-Faults ueberleben keinen Restart,
  // Default nach Konstrukt/Move-Quelle ist fully meshed (alle true, drops 0).
  bool SaveLog(const std::string& path) const;
  bool LoadLog(const std::string& path);

  // ---- s25: Opt-in Autosave -------------------------------------------------
  // Wenn gesetzt, schreibt append() nach jedem erfolgreichen Commit das volle
  // Leader-Log via SaveLog(pfad) zusaetzlich auf Platte (best effort: Fehler
  // lassen den Commit gueltig, naechster append versucht erneut).
  // KOSTEN: O(n)-Rewrite pro append (volles Log, n = Eintraege); nur fuer
  // kleine Gruppen/Tests bzw. explizites Durability-Opt-in gedacht, kein
  // inkrementelles Anhaengen.
  void set_autosave_log(std::string path);
  void clear_autosave();

  // ---- s25: Snapshots (State-Machine dump/load + Log-Compaction) ------------
  // Format "RSNP1": last_index u64, n u64, je Paar klen/vlen + bytes.
  // Save: crash-sicher via tmp-File + rename + fsync (File + Directory).
  // Load: setzt applied-Maps, kappt Log <= last_index, commit =
  // max(commit, last_index), last_applied = last_index + apply() (Tail
  // > last_index, <= commit wird re-applied; kein Commit-Verlust bei
  // aelterem Snapshot).
  bool SaveSnapshot(const std::string& path) const;
  bool LoadSnapshot(const std::string& path);

 private:
  // Wendet alle committeten, noch nicht angewendeten Eintraege eines Knotens an.
  // last_applied/commit sind echte Indizes (Basis log_base_).
  void apply(Node& node);
  static bool parseCommand(const std::string& cmd, std::string& op,
                           std::string& key, std::string& value);
  void electLocked(int candidate);  // Votes nur von erreichbaren Lebenden
  int electLeaderLocked();
  bool replicateLocked(const Entry& entry);
  [[nodiscard]] bool can_send_locked(int from, int to) const;
  [[nodiscard]] bool link_up_locked(int from, int to) const;
  void catchupNodeLocked(int node_id);
  void catchupReachableLocked();

  mutable std::mutex mutex_;
  int shard_id_ = 0;
  std::vector<Node> nodes_;
  int leader_id_ = -1;
  std::uint64_t term_ = 0;
  // s25: Log-Basis (kompaktierte Prefix-Laenge via Snapshot). Eintrag an
  // Position p hat Index log_base_ + p + 1. Ohne Snapshot 0 (V1-Semantik).
  std::uint64_t log_base_ = 0;
  // s25: Opt-in Autosave-Ziel (leer = aus). Unter mutex_, s. set/clear oben.
  std::string autosave_path_;
  // sXX: Timer-Election Auto-Modus (default false = manuell, Bestand grueng).
  bool auto_election_ = false;
  // Prodsim-Netz: Default fully meshed, Drop-PRNG Seed 42 (reproduzierbar).
  mutable std::mt19937 net_rng_{42};
  bool partitioned_[kGroupSize][kGroupSize]{};
  int drop_pct_[kGroupSize][kGroupSize]{};
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
