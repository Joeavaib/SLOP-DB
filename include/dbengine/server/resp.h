#pragma once

// RESP2-Server (Redis-Protokoll, STL/POSIX-only).
// RespServer haelt einen KVStore (Strings only, kein WRONGTYPE):
//   Bindet 127.0.0.1:port (0 = ephemeral), listen, startet Accept-Thread.
//   Je Connection ein detached Thread; persistente Connection bis QUIT/close.
//   Eingabe: Inline ("SET foo bar\r\n") oder Multibulk
//     ("*3\r\n$3\r\nSET\r\n..."). Befehle case-insensitiv.
//   Befehle auf KVStore: GET/SET/DEL/EXISTS/KEYS(prefix)/PING/QUIT.
//   Antworten in RESP2 (+/-/:$/*, nil = $-1). Unbekannt -> -ERR unknown command.

#include <atomic>
#include <string>
#include <thread>

#include "dbengine/kv.h"

namespace dbengine::resp {

class RespServer {
 public:
  RespServer();
  ~RespServer();

  RespServer(const RespServer&) = delete;
  RespServer& operator=(const RespServer&) = delete;

  // Bindet 127.0.0.1:port (0 = ephemeral), listen, startet Accept-Thread.
  void start(int port = 0);
  void stop();

  [[nodiscard]] int port() const { return port_; }
  [[nodiscard]] bool running() const { return running_.load(); }

  [[nodiscard]] kv::KVStore& kv() { return kv_; }
  [[nodiscard]] const kv::KVStore& kv() const { return kv_; }

 private:
  void acceptLoop();
  void handleConn(int fd);

  kv::KVStore kv_;
  int listenFd_ = -1;
  int port_ = 0;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace dbengine::resp
