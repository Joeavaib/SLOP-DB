#pragma once

// TCPServer V1 (POSIX sockets, 127.0.0.1, ephemeral port).
// Nutzt den pgwire-Codec (Startup/Q/Error/Ready/Complete/Row).
// Ablauf pro Connection: Startup -> AuthOk + ReadyForQuery -> Loop:
//   Q "SELECT 1" (Sonderpfad, byte-identisch) -> T(?column?)/D("1")/C(SELECT 1)/Z
//   Q sonst -> Session an server-eigener Executor-Instanz:
//     SELECT -> T(Spalten+OIDs)/D*(Text, NULL=-1)/C(SELECT n)/Z
//     INSERT/CREATE -> C(tag)/Z; Executor-Fehler -> E(42601/0A000)/Z (offen).
//   'X' (Terminate) beendet die Connection sauber.
//   Nicht-Q/X (Extended/COPY) -> E(0A000)/Z, unveraendert.

#include <atomic>
#include <mutex>
#include <thread>

#include "dbengine/kv.h"
#include "dbengine/sql/executor.h"
#include "dbengine/txn/mvcc.h"

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
  // Session-State: EINE Executor-Instanz pro PgServer (Lebensdauer = Server)
  // mit eigenem KV/MVCC (WAL = nullptr). handleConn-Threads teilen sie sich;
  // execMu_ serialisiert execute()+schemaOf().
  kv::KVStore kv_;
  txn::MvccStore mvcc_;
  sql::Executor executor_;
  std::mutex execMu_;
};

}  // namespace dbengine::pgserver
