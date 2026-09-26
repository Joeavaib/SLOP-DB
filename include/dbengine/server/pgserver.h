#pragma once

// TCPServer V1 (POSIX sockets, 127.0.0.1, ephemeral port).
// Nutzt den pgwire-Codec (Startup/Q/Error/Ready/Complete/Row).
// Ablauf pro Connection: Startup -> AuthOk + ReadyForQuery -> Loop:
//   Q "SELECT 1" (Sonderpfad, byte-identisch) -> T(?column?)/D("1")/C(SELECT 1)/Z
//   Q sonst -> Session an server-eigener Executor-Instanz:
//     SELECT -> T(Spalten+OIDs)/D*(Text, NULL=-1)/C(SELECT n)/Z
//     INSERT/CREATE -> C(tag)/Z; Executor-Fehler -> E(42601/0A000)/Z (offen).
//   Extended minimal parameterlos (pro Connection: Statements+Portale):
//     P -> '1' (ParseComplete, Query gespeichert); D(S/P) -> T/n ohne Execute
//       (Projektions-Analyse: Spaltennamen+OIDs aus Schema/Parser);
//     B -> '2' (Portal angelegt, nur ohne Parameter); E -> T/D/C wie Q-Pfad;
//     S -> Z; C(S/P) -> '3'. Mit Parametern ($n/Bind-nParams>0) -> E 0A000+Z.
//   'X' (Terminate) beendet die Connection sauber.
//   Sonst (COPY/unbekannt) -> E(0A000)/Z, unveraendert.
//   SSLRequest (len 8, Code 80877103, kein Typ-Byte) PG-konform:
//     ohne TLS (Default, ohne DBENGINE_WITH_TLS oder ohne setTlsCert):
//       ein Byte 'N' -> Client faellt auf Klartext zurueck, danach normaler
//       Startup-Flow ueber denselben fd (statt FATAL wie frueher).
//     mit TLS (DBENGINE_WITH_TLS + setTlsCert(key, cert)):
//       ein Byte 'S' + SSL_accept auf gleichem fd, danach normaler
//       Startup/Auth-Flow ueber TLS (R0/R3/28P01 unveraendert).

#include <atomic>
#include <map>
#include <mutex>
#include <string>
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

  // Auth-Hook (opt-in, default: Trust-All wie bisher).
  //   setAuth({{user, pass}, ...}) hinterlegt Cleartext-Passwoerter.
  //   setAuthRequired(true) aktiviert den Check, false (Default) = Trust-All:
  //     Startup -> R(0 AuthOk) + Z (exakt heutiges Verhalten, keine 'p'-Runde).
  //   Wenn required: Startup -> R(3 Cleartext) -> lese PasswordMessage ('p')
  //     -> bei Match R(0) + weiter wie bisher, sonst E FATAL 28P01 + close.
  //   SICHERHEIT: Cleartext-Passwort ohne TLS ist mitlesbar (PG-Protokoll).
  //     Nur mit Sidecar-TLS (z.B. stunnel gegen 127.0.0.1-Bind) nutzen,
  //     nie direkt auf untrusted Netz binden. Kein Hashing/SASL in V1.
  void setAuth(const std::map<std::string, std::string>& users);
  void setAuthRequired(bool required = false);

  // TLS opt-in (PG-konform). setTlsCert(keyPath, certPath) aktiviert TLS:
  //   SSLRequest -> 'S' + Server-Handshake, danach Startup/Auth ueber TLS.
  //   Ohne Aufruf (Default): SSLRequest -> 'N' (Klartext-Fallback).
  //   OpenSSL-Code ist hinter DBENGINE_WITH_TLS; ohne Define ist setTlsCert
  //   ein No-Op (nur Pfade gespeichert) und es gilt immer der 'N'-Pfad,
  //   STL-only, keine OpenSSL-Abhaengigkeit. Aktivierung nur via CMake
  //   -DDBENGINE_WITH_TLS=ON mit gefundenem OpenSSL.
  void setTlsCert(const std::string& keyPath, const std::string& certPath);

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
  // Auth-Config (V1: Cleartext-Map + Flag, Default Trust-All).
  // Wird pro Connection unter authMu_ kopiert (handleConn-Threads).
  std::mutex authMu_;
  std::map<std::string, std::string> authUsers_;
  bool authRequired_ = false;
  // TLS-Config (Pfade + Flag, Default aus). Wird pro SSLRequest unter
  // tlsMu_ kopiert (handleConn-Threads). Handshake-Kontext wird pro
  // Connection aufgebaut (kein geteilter SSL_CTX).
  mutable std::mutex tlsMu_;
  std::string tlsKeyPath_;
  std::string tlsCertPath_;
  bool tlsEnabled_ = false;
};

}  // namespace dbengine::pgserver
