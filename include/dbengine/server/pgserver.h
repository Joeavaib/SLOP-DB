#pragma once

// TCPServer V1 (POSIX sockets, 127.0.0.1, ephemeral port).
// Nutzt den pgwire-Codec (Startup/Q/Error/Ready/Complete/Row).
// Ablauf pro Connection: Startup -> AuthOk + ReadyForQuery -> Loop:
//   Q -> RowDescription + DataRow + CommandComplete + ReadyForQuery
//   (SELECT 1) bzw. CommandComplete + ReadyForQuery (sonst).
//   'X' (Terminate) beendet die Connection sauber.

#include <atomic>
#include <thread>

namespace dbengine::pgserver {

class PgServer {
 public:
  PgServer();
  ~PgServer();

  PgServer(const PgServer&) = delete;
  PgServer& operator=(const PgServer&) = delete;

  // Bindet 127.0.0.1:0, listen, startet Accept-Thread.
  void start();
  // Stoppt Accept-Loop, schliesst Listen-Socket, joint Thread.
  void stop();

  int port() const { return port_; }
  bool running() const;

 private:
  void acceptLoop();
  void handleConn(int fd);

  int listenFd_ = -1;
  int port_ = 0;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace dbengine::pgserver
