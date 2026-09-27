// MetricsServer: Prometheus-Textformat ueber TCP (POSIX, 127.0.0.1).
// Muster nach src/server/pgserver.cpp: listen/ephemeral/sendAll/recv.
// Plus dbmetrics-main: --selfcheck (ephemeral + scrape + asserts) und
// --port <n> (Dauerbetrieb). STL/POSIX-only, C++20.

#include "dbengine/server/metrics.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// pg_stat-Quelle (QueryStat-Struct, session-lokal im Executor). metrics.h ist
// fixiert (kein Snapshot-Umbau): Top-5 leben in einem mutex-geschuetzten
// Prozess-Register hier (SetPgStats) und werden von render() angehaengt.
#include "dbengine/sql/executor.h"

// Echte Mini-Stores fuer Demo-Snapshot (nur im main-Teil).
#include "dbengine/columnar/store.h"
#include "dbengine/kv.h"
#include "dbengine/raft/shard.h"
#include "dbengine/storage/wal.h"
#include "dbengine/vector/hnsw.h"

namespace dbengine::metrics {
namespace {

bool sendAll(int fd, const char* data, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

bool sendAll(int fd, const std::string& s) {
  if (s.empty()) return true;
  return sendAll(fd, s.data(), s.size());
}

// Liest HTTP-Request bis "\r\n\r\n" oder Limit (64 KiB). false bei EOF/Fehler.
bool readHttpRequest(int fd, std::string& out) {
  out.clear();
  out.reserve(1024);
  char buf[1024];
  while (out.size() < 64 * 1024) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n == 0) return !out.empty();
    if (n < 0) {
      if (errno == EINTR) continue;
      return !out.empty();
    }
    out.append(buf, static_cast<std::size_t>(n));
    if (out.find("\r\n\r\n") != std::string::npos) return true;
  }
  return true;
}

bool isGetMetrics(const std::string& req) {
  // Erste Zeile: "GET /metrics ..." (auch "GET /metrics?x=1 ..." ok).
  std::size_t eol = req.find("\r\n");
  std::string line = eol == std::string::npos ? req : req.substr(0, eol);
  if (line.rfind("GET ", 0) != 0) return false;
  std::size_t path_beg = 4;
  std::size_t path_end = line.find(' ', path_beg);
  std::string target =
      path_end == std::string::npos ? line.substr(path_beg) : line.substr(path_beg, path_end - path_beg);
  return target == "/metrics" || target.rfind("/metrics?", 0) == 0;
}

}  // namespace

MetricsServer::MetricsServer() = default;
MetricsServer::~MetricsServer() { stop(); }

void MetricsServer::set(Snapshot s) {
  std::lock_guard<std::mutex> lk(mu_);
  snapshot_ = s;
}

Snapshot MetricsServer::get() const {
  std::lock_guard<std::mutex> lk(mu_);
  return snapshot_;
}

// ---- pg_stat_statements-light: Top-5-Render ---------------------------------
// Session-lokal, keine Persistenz (Register lebt nur im Prozess; Restart =
// leer). Wiring (Aufrufer, z.B. Server-Loop):
//   // Forward-Deklaration (metrics.h ist fixiert, keine neuen Deklarationen):
//   namespace dbengine::metrics {
//     void SetPgStats(std::vector<dbengine::sql::QueryStat> top);
//   }
//   ex.execute(sql); ...
//   dbengine::metrics::SetPgStats(ex.queryStats(5));
// Danach enthaelt jeder render()/scrape zusaetzlich (max. 5 Queries):
//   dbengine_pgstat_calls_total{query="<norm>"} <calls>
//   dbengine_pgstat_time_ms{query="<norm>"} <total_ms, 3 Nachkommastellen>
//   dbengine_pgstat_rows{query="<norm>"} <rows_out>
//   dbengine_pgstat_errors_total{query="<norm>"} <errors>
// Overhead: SetPgStats = eine Vektor-Kopie; render() = O(Top-5) String-Append,
// kein Einfluss auf den Execute-Pfad (der zahlt nur Map + chrono, s.
// executor.cpp). Leeres Register -> Byte-identische Ausgabe wie bisher.
namespace {
std::mutex g_pgstat_mu;
std::vector<dbengine::sql::QueryStat> g_pgstat_top;

std::string pgEscapeLabel(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    if (c == '\\')
      o += "\\\\";
    else if (c == '"')
      o += "\\\"";
    else if (c == '\n')
      o += "\\n";
    else if (c == '\r')
      o += "\\r";
    else
      o += c;
  }
  return o;
}
}  // namespace

void SetPgStats(std::vector<dbengine::sql::QueryStat> top) {
  std::lock_guard<std::mutex> lk(g_pgstat_mu);
  if (top.size() > 5) top.resize(5);
  g_pgstat_top = std::move(top);
}

std::vector<dbengine::sql::QueryStat> GetPgStats() {
  std::lock_guard<std::mutex> lk(g_pgstat_mu);
  return g_pgstat_top;
}

std::string RenderPgStats(const std::vector<dbengine::sql::QueryStat>& top) {
  std::string out;
  std::size_t n = top.size();
  if (n > 5) n = 5;
  for (std::size_t i = 0; i < n; ++i) {
    const auto& e = top[i];
    const std::string q = pgEscapeLabel(e.query);
    out += "dbengine_pgstat_calls_total{query=\"" + q + "\"} ";
    out += std::to_string(e.calls);
    out += '\n';
    out += "dbengine_pgstat_time_ms{query=\"" + q + "\"} ";
    {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(3) << e.total_ms;
      out += oss.str();
    }
    out += '\n';
    out += "dbengine_pgstat_rows{query=\"" + q + "\"} ";
    out += std::to_string(e.rows_out);
    out += '\n';
    out += "dbengine_pgstat_errors_total{query=\"" + q + "\"} ";
    out += std::to_string(e.errors);
    out += '\n';
  }
  return out;
}

std::string MetricsServer::render(const Snapshot& s) {
  std::string out;
  out.reserve(1024);
  auto line = [&](const char* name, long long v) {
    out += name;
    out += ' ';
    out += std::to_string(v);
    out += '\n';
  };
  auto uline = [&](const char* name, unsigned long long v) {
    out += name;
    out += ' ';
    out += std::to_string(v);
    out += '\n';
  };
  uline("dbengine_wal_durable_lsn", s.wal_durable_lsn);
  uline("dbengine_wal_next_lsn", s.wal_next_lsn);
  uline("dbengine_wal_appends_total", s.wal_appends);
  uline("dbengine_wal_flushes_total", s.wal_flushes);
  uline("dbengine_raft_commit_index", s.raft_commit_index);
  uline("dbengine_raft_log_size", s.raft_log_size);
  uline("dbengine_raft_alive_count", s.raft_alive_count);
  line("dbengine_raft_leader_id", s.raft_leader_id);
  uline("dbengine_raft_term", s.raft_term);
  uline("dbengine_columnar_total_rows", s.columnar_total_rows);
  uline("dbengine_columnar_sealed_parts", s.columnar_sealed_parts);
  uline("dbengine_hnsw_size", s.hnsw_size);
  line("dbengine_hnsw_dim", s.hnsw_dim);
  line("dbengine_hnsw_max_level", s.hnsw_max_level);
  uline("dbengine_kv_keys", s.kv_keys);
  uline("dbengine_kv_sequence", s.kv_sequence);
  out += RenderPgStats(GetPgStats());  // leer -> Byte-identisch wie bisher
  return out;
}

std::string MetricsServer::render() const { return render(get()); }

void MetricsServer::start(int port) {
  if (running_.load()) return;
  listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd_ < 0) throw std::runtime_error("metrics: socket() failed");

  int one = 1;
  ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    throw std::runtime_error("metrics: bind() failed");
  }
  if (::listen(listenFd_, 16) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    throw std::runtime_error("metrics: listen() failed");
  }
  sockaddr_in bound{};
  socklen_t blen = sizeof(bound);
  if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    throw std::runtime_error("metrics: getsockname() failed");
  }
  port_ = ntohs(bound.sin_port);

  running_.store(true);
  thread_ = std::thread(&MetricsServer::acceptLoop, this);
}

void MetricsServer::stop() {
  bool was = running_.exchange(false);
  (void)was;
  if (listenFd_ >= 0) {
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    listenFd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
  port_ = 0;
}

void MetricsServer::acceptLoop() {
  while (running_.load()) {
    int cfd = ::accept(listenFd_, nullptr, nullptr);
    if (cfd < 0) {
      if (!running_.load()) break;
      if (errno == EINTR) continue;
      if (errno == EINVAL || errno == EBADF) break;
      continue;
    }
    std::thread(&MetricsServer::handleConn, this, cfd).detach();
  }
}

void MetricsServer::handleConn(int fd) {
  std::string req;
  bool ok = readHttpRequest(fd, req);
  std::string body;
  std::string head;
  if (ok && isGetMetrics(req)) {
    body = render();
    head = "HTTP/1.0 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    sendAll(fd, head);
    sendAll(fd, body);
  } else {
    body = "not found\n";
    head = "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    sendAll(fd, head);
    sendAll(fd, body);
  }
  ::close(fd);
}

}  // namespace dbengine::metrics

// ---- dbmetrics-main (klein, im gleichen TU) ---------------------------------
namespace {

using dbengine::metrics::MetricsServer;
using dbengine::metrics::Snapshot;

// Baut Demo-Snapshot aus echten Mini-Stores (WAL+KV+Writes, Raft, Columnar, HNSW).
Snapshot BuildDemoSnapshot(const std::string& wal_path) {
  Snapshot s;

  // WAL: 3 appends + flush => durable_lsn=3, next_lsn=4.
  {
    ::unlink(wal_path.c_str());
    dbengine::storage::Wal wal(wal_path);
    wal.open();
    wal.append("m:put k1 v1");
    wal.append("m:put k2 v2");
    wal.append("m:put k3 v3");
    wal.flush();
    s.wal_durable_lsn = wal.durable_lsn();
    s.wal_next_lsn = wal.next_lsn();
    auto gs = wal.group_stats();
    s.wal_appends = gs.appends;
    s.wal_flushes = gs.flushes;
    wal.close();
    ::unlink(wal_path.c_str());
  }

  // KV: 3 Puts (WriteBatch-Pfad als "Writes").
  {
    dbengine::kv::KVStore kv;
    dbengine::kv::WriteBatch batch;
    batch.Put("m:k1", "v1");
    batch.Put("m:k2", "v2");
    batch.Put("m:k3", "v3");
    if (!kv.Write(batch)) throw std::runtime_error("demo: kv.Write failed");
    s.kv_keys = kv.Size();
    s.kv_sequence = kv.Sequence();
  }

  // Raft: Leader-Wahl + 2 Commands.
  {
    dbengine::raft::RaftGroup g(0, "", "");
    g.electLeader();
    g.append("put rk1 rv1");
    g.append("put rk2 rv2");
    s.raft_commit_index = g.commitIndex();
    s.raft_log_size = g.logSize();
    s.raft_alive_count = g.aliveCount();
    s.raft_leader_id = g.leaderId();
    s.raft_term = g.term();
  }

  // Columnar: 4 Rows, dann seal => total=4, sealed=1.
  {
    dbengine::columnar::ColumnarStore store;
    store.Append(1, "a");
    store.Append(2, "b");
    store.Append(3, "c");
    store.Append(4, "d");
    store.SealActive();
    s.columnar_total_rows = store.TotalRows();
    s.columnar_sealed_parts = store.NumSealedParts();
  }

  // HNSW: dim=4, 4 Vektoren, build.
  {
    dbengine::vector::HnswIndex idx(4);
    for (int i = 0; i < 4; ++i) {
      dbengine::vector::Vector v = {static_cast<float>(i), 0.0f, 0.0f, 1.0f};
      idx.add(v);
    }
    idx.build();
    s.hnsw_size = idx.size();
    s.hnsw_dim = idx.dim();
    s.hnsw_max_level = idx.max_level();
  }

  return s;
}

std::string ScrapeMetrics(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("scrape: socket() failed");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    throw std::runtime_error("scrape: connect() failed");
  }
  const std::string req = "GET /metrics HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
  std::size_t off = 0;
  while (off < req.size()) {
    ssize_t n = ::send(fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      ::close(fd);
      throw std::runtime_error("scrape: send() failed");
    }
    off += static_cast<std::size_t>(n);
  }
  std::string out;
  char buf[4096];
  while (true) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      throw std::runtime_error("scrape: recv() failed");
    }
    out.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return out;
}

int SelfCheck() {
  try {
    std::string wal_path =
        "/tmp/dbmetrics_selfcheck_" + std::to_string(static_cast<long long>(::getpid())) + ".wal";
    Snapshot snap = BuildDemoSnapshot(wal_path);
    MetricsServer srv;
    srv.set(snap);
    srv.start(0);  // ephemeral
    std::string resp = ScrapeMetrics(srv.port());
    srv.stop();
    const char* want[] = {
        "wal_durable_lsn",
        "raft_commit_index",
        "columnar_total_rows",
        "hnsw_size",
    };
    for (const char* k : want) {
      if (resp.find(k) == std::string::npos) {
        std::fprintf(stderr, "dbmetrics selfcheck: FAIL keyword '%s' missing\n", k);
        return 1;
      }
    }
    std::cout << "OK\n";
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "dbmetrics selfcheck: FAIL %s\n", e.what());
    return 1;
  }
}

int ServeForever(int port) {
  std::string wal_path =
      "/tmp/dbmetrics_demo_" + std::to_string(static_cast<long long>(::getpid())) + ".wal";
  Snapshot snap = BuildDemoSnapshot(wal_path);
  MetricsServer srv;
  srv.set(snap);
  srv.start(port);
  std::cout << "dbmetrics listening on 127.0.0.1:" << srv.port() << " (/metrics)\n" << std::flush;
  while (true) std::this_thread::sleep_for(std::chrono::seconds(3600));
  return 0;  // unreachable
}

}  // namespace

int main(int argc, char** argv) {
  bool selfcheck = false;
  int port = -1;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--selfcheck") {
      selfcheck = true;
    } else if (a == "--port") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "usage: %s [--selfcheck] [--port <n>]\n", argv[0]);
        return 2;
      }
      try {
        port = std::stoi(argv[++i]);
      } catch (...) {
        std::fprintf(stderr, "dbmetrics: invalid --port value\n");
        return 2;
      }
      if (port < 0 || port > 65535) {
        std::fprintf(stderr, "dbmetrics: --port out of range 0..65535\n");
        return 2;
      }
    } else {
      std::fprintf(stderr, "usage: %s [--selfcheck] [--port <n>]\n", argv[0]);
      return 2;
    }
  }
  if (selfcheck) {
    if (port >= 0) {
      // --selfcheck --port N: fester Port statt ephemeral.
      try {
        std::string wal_path =
            "/tmp/dbmetrics_selfcheck_" + std::to_string(static_cast<long long>(::getpid())) + ".wal";
        Snapshot snap = BuildDemoSnapshot(wal_path);
        MetricsServer srv;
        srv.set(snap);
        srv.start(port);
        std::string resp = ScrapeMetrics(srv.port());
        srv.stop();
        const char* want[] = {
            "wal_durable_lsn",
            "raft_commit_index",
            "columnar_total_rows",
            "hnsw_size",
        };
        for (const char* k : want) {
          if (resp.find(k) == std::string::npos) {
            std::fprintf(stderr, "dbmetrics selfcheck: FAIL keyword '%s' missing\n", k);
            return 1;
          }
        }
        std::cout << "OK\n";
        return 0;
      } catch (const std::exception& e) {
        std::fprintf(stderr, "dbmetrics selfcheck: FAIL %s\n", e.what());
        return 1;
      }
    }
    return SelfCheck();
  }
  if (port >= 0) return ServeForever(port);
  std::fprintf(stderr, "usage: %s [--selfcheck] [--port <n>]\n", argv[0]);
  return 2;
}
