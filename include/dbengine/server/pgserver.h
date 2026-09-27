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
  //   SCRAM-SHA-256 (RFC 5802 Server-Seite, opt-in via setScram(true),
  //     Default false = exakt heutiges R3-Verhalten, byte-identisch):
  //     Wenn required + scramEnabled + User-Eintrag im SCRAM-Format, dann
  //     statt R(3): R(10 AuthenticationSASL mit 'SCRAM-SHA-256') ->
  //     SASLInitialResponse ('p' mit Mechanismus + Client-First
  //     "n,,n=user,r=nonce") -> R(11 SASLContinue mit Server-First
  //     "r=combined,s=salt-b64,i=iter") -> SASLResponse ('p' mit
  //     Client-Final "c=...,r=...,p=proof") -> bei OK R(12 SASLFinal
  //     "v=serverSig") + R(0) + Z, bei Fail E FATAL 28P01 + close.
  //     Salt-Format pro User in authUsers_: "scram:<salt-b64>:<iter>:
  //     <storedkey-b64>[:<serverkey-b64>]". salt-b64 = roher Salt (base64),
  //     iter = PBKDF2-Iterationen (Standard 4096), storedkey-b64 =
  //     base64(SHA256(ClientKey)) (32 Byte), optional 4. Feld serverkey-b64
  //     = base64(ServerKey) (32 Byte) fuer RFC-korrekte Server-Signatur.
  //     Ohne 4. Feld wird die Server-Signatur mit StoredKey als HMAC-Key
  //     berechnet (dokumentierte Abweichung; strikte Clients sollten die
  //     4-Feld-Form nutzen). Klartext-Eintrag (ohne "scram:"-Prefix) bei
  //     aktivem SCRAM = Legacy-R3-Pfad (R(3)-Cleartext wie bisher).
  //     Unbekannter User = Legacy-R3-Pfad (danach 28P01, keine
  //     User-Enumeration ueber R10/R3 hinaus wird nicht garantiert).
  //   KRYPTO: SHA256/HMAC-SHA256 ausschliesslich via OpenSSL (EVP/HMAC),
  //     nur wenn mit DBENGINE_WITH_TLS gebaut. Ohne das Define ist SCRAM
  //     explizit deaktiviert: setScram(true) wird gespeichert, aber eine
  //     SCRAM-Auth wird mit E FATAL 28P01 verweigert (kein R10, kein
  //     Fallback auf R3). Es gibt KEINE eigene Crypto-Implementierung.
  //     Base64 (Salt/Proof-Codec, kein Krypto) ist STL-only und immer aktiv.
  void setAuth(const std::map<std::string, std::string>& users);
  void setAuthRequired(bool required = false);
  void setScram(bool enabled = true);

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
  // scramEnabled_ (Default false): mit setScram(true) aktiviert; ohne
  // DBENGINE_WITH_TLS wird SCRAM zur Laufzeit verweigert (28P01).
  std::mutex authMu_;
  std::map<std::string, std::string> authUsers_;
  bool authRequired_ = false;
  bool scramEnabled_ = false;
  // TLS-Config (Pfade + Flag, Default aus). Wird pro SSLRequest unter
  // tlsMu_ kopiert (handleConn-Threads). Handshake-Kontext wird pro
  // Connection aufgebaut (kein geteilter SSL_CTX).
  mutable std::mutex tlsMu_;
  std::string tlsKeyPath_;
  std::string tlsCertPath_;
  bool tlsEnabled_ = false;
};

}  // namespace dbengine::pgserver
