#pragma once

// Metrics-Endpoint (Prometheus-Textformat, STL/POSIX-only).
// MetricsServer haelt KEINE globalen Stores: Aufrufer fuellt Snapshot,
// Server rendert Textformat und serviert GET /metrics ueber TCP
// (127.0.0.1, ephemeral Port bei 0).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace dbengine::metrics {

// Flacher Input-Counter. Aufrufer liest Rohzaehler aus den Stores
// (WAL/KV/Raft/Columnar/HNSW) und fuellt dieses Struct.
struct Snapshot {
  // WAL (storage/wal.h): durable_lsn/next_lsn/group_stats.
  std::uint64_t wal_durable_lsn = 0;
  std::uint64_t wal_next_lsn = 0;
  std::uint64_t wal_appends = 0;
  std::uint64_t wal_flushes = 0;
  // Raft (raft/shard.h): commitIndex/logSize/aliveCount/leaderId/term.
  std::uint64_t raft_commit_index = 0;
  std::uint64_t raft_log_size = 0;
  std::uint64_t raft_alive_count = 0;
  long long raft_leader_id = -1;
  std::uint64_t raft_term = 0;
  // Columnar (columnar/store.h): TotalRows/NumSealedParts.
  std::uint64_t columnar_total_rows = 0;
  std::uint64_t columnar_sealed_parts = 0;
  // HNSW (vector/hnsw.h): size/dim/max_level.
  std::uint64_t hnsw_size = 0;
  long long hnsw_dim = 0;
  long long hnsw_max_level = 0;
  // KV (kv.h): Size/Sequence (Demo-Fuetterung).
  std::uint64_t kv_keys = 0;
  std::uint64_t kv_sequence = 0;
};

class MetricsServer {
 public:
  MetricsServer();
  ~MetricsServer();

  MetricsServer(const MetricsServer&) = delete;
  MetricsServer& operator=(const MetricsServer&) = delete;

  void set(Snapshot s);
  [[nodiscard]] Snapshot get() const;

  // Rendert aktuellen Snapshot bzw. gegebenes Snapshot als
  // Prometheus-Textformat ("dbengine_wal_durable_lsn 123\n"...).
  [[nodiscard]] std::string render() const;
  [[nodiscard]] static std::string render(const Snapshot& s);

  // Bindet 127.0.0.1:port (0 = ephemeral), listen, startet Accept-Thread.
  void start(int port = 0);
  void stop();

  [[nodiscard]] int port() const { return port_; }
  [[nodiscard]] bool running() const { return running_.load(); }

 private:
  void acceptLoop();
  void handleConn(int fd);

  mutable std::mutex mu_;
  Snapshot snapshot_;
  int listenFd_ = -1;
  int port_ = 0;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace dbengine::metrics
