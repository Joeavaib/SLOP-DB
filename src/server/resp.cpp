// RespServer: RESP2-Protokoll ueber TCP (POSIX, 127.0.0.1).
// Muster nach src/server/pgserver.cpp: listen/ephemeral/send/recv.
// Plus dbresp-main: --selfcheck (ephemeral + Socket-Client + Asserts)
// und --port <n> (Dauerbetrieb). STL/POSIX-only, C++20.

#include "dbengine/server/resp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dbengine::resp {
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

std::string upperStr(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// ---- RESP2-Antwort-Encoder ------------------------------------------------

std::string respSimple(const std::string& s) { return "+" + s + "\r\n"; }
std::string respError(const std::string& s) { return "-" + s + "\r\n"; }
std::string respInt(long long v) { return ":" + std::to_string(v) + "\r\n"; }
std::string respBulk(const std::string& s) {
  return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}
std::string respNil() { return "$-1\r\n"; }

// ---- Eingabe-Parser (Inline + Multibulk) -----------------------------------
// tryParse: true + consumed>0 wenn genau ein Kommando komplett im Buffer.
//   args leer + true => leere Inline-Zeile (kein Reply, nur consumen).
//   false => unvollstaendig (mehr recv noetig).
//   protocolError => syntaktisch gueltig gelesen, aber Fehlformat
//     (Caller sendet -ERR Protocol error und consumt).

struct ParseOut {
  bool complete = false;
  bool protocolError = false;
  std::vector<std::string> args;
  std::size_t consumed = 0;
};

bool parseLongStrict(const std::string& s, long long& out) {
  if (s.empty()) return false;
  std::size_t i = 0;
  bool neg = false;
  if (s[0] == '-' || s[0] == '+') {
    neg = (s[0] == '-');
    i = 1;
    if (s.size() == 1) return false;
  }
  long long v = 0;
  for (; i < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    int d = s[i] - '0';
    // Einfacher Overflow-Guard (16 MiB-Limit macht echten Overflow unmoeglich).
    if (v > 1000000000LL) return false;
    v = v * 10 + d;
  }
  out = neg ? -v : v;
  return true;
}

std::vector<std::string> splitInline(const std::string& line) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size()) break;
    std::size_t j = i;
    while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
    out.push_back(line.substr(i, j - i));
    i = j;
  }
  return out;
}

ParseOut tryParseCommand(const std::string& buf) {
  ParseOut o;
  if (buf.empty()) return o;
  if (buf[0] == '*') {
    std::size_t eol = buf.find("\r\n");
    if (eol == std::string::npos) {
      // Toleranz: nacktes \n ohne \r (Tests/Clients).
      std::size_t lf = buf.find('\n');
      if (lf == std::string::npos) return o;
      eol = lf;
    }
    std::string countStr = buf.substr(1, eol - 1);
    if (!countStr.empty() && countStr.back() == '\r') countStr.pop_back();
    long long count = 0;
    if (!parseLongStrict(countStr, count)) {
      o.complete = true;
      o.protocolError = true;
      std::size_t lf = buf.find('\n');
      o.consumed = lf + 1;
      return o;
    }
    if (count <= 0 || count > 1024) {
      o.complete = true;
      o.protocolError = true;
      std::size_t lf = buf.find('\n');
      o.consumed = (lf == std::string::npos) ? buf.size() : lf + 1;
      return o;
    }
    std::size_t pos = buf.find('\n', 0) + 1;
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(count));
    for (long long i = 0; i < count; ++i) {
      if (pos >= buf.size()) return ParseOut{};
      if (buf[pos] != '$') {
        o.complete = true;
        o.protocolError = true;
        o.consumed = pos;
        return o;
      }
      std::size_t le = buf.find("\r\n", pos);
      bool bareLf = false;
      if (le == std::string::npos) {
        le = buf.find('\n', pos);
        if (le == std::string::npos) return ParseOut{};
        bareLf = true;
      }
      std::string lenStr = buf.substr(pos + 1, le - pos - 1);
      if (!lenStr.empty() && lenStr.back() == '\r') lenStr.pop_back();
      long long blen = 0;
      if (!parseLongStrict(lenStr, blen)) {
        o.complete = true;
        o.protocolError = true;
        o.consumed = le + (bareLf ? 1 : 2);
        return o;
      }
      std::size_t hdrEnd = le + (bareLf ? 1 : 2);
      if (blen == -1) {
        // Null-Bulk in Argv: als leerer String (kein WRONGTYPE/nil-Input).
        args.emplace_back("");
        pos = hdrEnd;
        continue;
      }
      if (blen < -1 || blen > 16 * 1024 * 1024) {
        o.complete = true;
        o.protocolError = true;
        o.consumed = hdrEnd;
        return o;
      }
      std::size_t need = hdrEnd + static_cast<std::size_t>(blen) + 2;
      if (buf.size() < need) {
        // Evtl. bare-LF Variante: +1 statt +2.
        if (buf.size() < hdrEnd + static_cast<std::size_t>(blen) + 1) return ParseOut{};
        if (buf[hdrEnd + static_cast<std::size_t>(blen)] != '\n') return ParseOut{};
        args.push_back(buf.substr(hdrEnd, static_cast<std::size_t>(blen)));
        pos = hdrEnd + static_cast<std::size_t>(blen) + 1;
        continue;
      }
      if (buf[hdrEnd + static_cast<std::size_t>(blen)] != '\r' ||
          buf[hdrEnd + static_cast<std::size_t>(blen) + 1] != '\n') {
        o.complete = true;
        o.protocolError = true;
        o.consumed = hdrEnd + static_cast<std::size_t>(blen);
        return o;
      }
      args.push_back(buf.substr(hdrEnd, static_cast<std::size_t>(blen)));
      pos = hdrEnd + static_cast<std::size_t>(blen) + 2;
    }
    o.complete = true;
    o.args = std::move(args);
    o.consumed = pos;
    return o;
  }
  // Inline: bis \n.
  std::size_t lf = buf.find('\n');
  if (lf == std::string::npos) return o;
  std::string line = buf.substr(0, lf);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  o.complete = true;
  o.consumed = lf + 1;
  o.args = splitInline(line);
  return o;
}

// ---- Kommando-Ausfuehrung ---------------------------------------------------

struct ExecOut {
  std::string reply;
  bool quit = false;
};

ExecOut execCommand(const std::vector<std::string>& args, kv::KVStore& kv) {
  ExecOut o;
  if (args.empty()) {
    o.reply = respError("ERR empty command");
    return o;
  }
  std::string cmd = upperStr(args[0]);
  if (cmd == "PING") {
    if (args.size() == 1) {
      o.reply = respSimple("PONG");
    } else if (args.size() == 2) {
      o.reply = respBulk(args[1]);
    } else {
      o.reply = respError("ERR wrong number of arguments for 'ping'");
    }
    return o;
  }
  if (cmd == "SET") {
    if (args.size() != 3) {
      o.reply = respError("ERR wrong number of arguments for 'set'");
      return o;
    }
    kv.Put(args[1], args[2]);
    o.reply = respSimple("OK");
    return o;
  }
  if (cmd == "GET") {
    if (args.size() != 2) {
      o.reply = respError("ERR wrong number of arguments for 'get'");
      return o;
    }
    auto v = kv.Get(args[1]);
    if (!v.has_value()) {
      o.reply = respNil();
    } else {
      o.reply = respBulk(*v);
    }
    return o;
  }
  if (cmd == "DEL") {
    if (args.size() < 2) {
      o.reply = respError("ERR wrong number of arguments for 'del'");
      return o;
    }
    long long n = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (kv.Delete(args[i])) ++n;
    }
    o.reply = respInt(n);
    return o;
  }
  if (cmd == "EXISTS") {
    if (args.size() < 2) {
      o.reply = respError("ERR wrong number of arguments for 'exists'");
      return o;
    }
    long long n = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (kv.Get(args[i]).has_value()) ++n;
    }
    o.reply = respInt(n);
    return o;
  }
  if (cmd == "KEYS") {
    if (args.size() > 2) {
      o.reply = respError("ERR wrong number of arguments for 'keys'");
      return o;
    }
    std::string prefix = (args.size() == 2) ? args[1] : "";
    auto pairs = kv.Scan(prefix);
    std::string r = "*" + std::to_string(pairs.size()) + "\r\n";
    for (const auto& kvp : pairs) r += respBulk(kvp.first);
    o.reply = std::move(r);
    return o;
  }
  if (cmd == "QUIT") {
    o.reply = respSimple("OK");
    o.quit = true;
    return o;
  }
  o.reply = respError("ERR unknown command");
  return o;
}

}  // namespace

RespServer::RespServer() = default;
RespServer::~RespServer() { stop(); }

void RespServer::start(int port) {
  if (running_.load()) return;
  listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd_ < 0) throw std::runtime_error("resp: socket() failed");

  int one = 1;
  ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    throw std::runtime_error("resp: bind() failed");
  }
  if (::listen(listenFd_, 16) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    throw std::runtime_error("resp: listen() failed");
  }
  sockaddr_in bound{};
  socklen_t blen = sizeof(bound);
  if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    throw std::runtime_error("resp: getsockname() failed");
  }
  port_ = ntohs(bound.sin_port);

  running_.store(true);
  thread_ = std::thread(&RespServer::acceptLoop, this);
}

void RespServer::stop() {
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

void RespServer::acceptLoop() {
  while (running_.load()) {
    int cfd = ::accept(listenFd_, nullptr, nullptr);
    if (cfd < 0) {
      if (!running_.load()) break;
      if (errno == EINTR) continue;
      if (errno == EINVAL || errno == EBADF) break;
      continue;
    }
    std::thread(&RespServer::handleConn, this, cfd).detach();
  }
}

void RespServer::handleConn(int fd) {
  std::string buf;
  buf.reserve(4096);
  char tmp[4096];
  bool open = true;
  while (open && running_.load()) {
    // Versuche erst gepuffertes Kommando (Pipelining: mehrere pro recv).
    while (open) {
      ParseOut p = tryParseCommand(buf);
      if (!p.complete) break;
      buf.erase(0, p.consumed);
      if (p.protocolError) {
        if (!sendAll(fd, respError("ERR Protocol error"))) {
          open = false;
          break;
        }
        continue;
      }
      if (p.args.empty()) continue;  // leere Inline-Zeile: kein Reply
      ExecOut e = execCommand(p.args, kv_);
      if (!sendAll(fd, e.reply)) {
        open = false;
        break;
      }
      if (e.quit) {
        open = false;
        break;
      }
    }
    if (!open) break;
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    buf.append(tmp, static_cast<std::size_t>(n));
    if (buf.size() > 17 * 1024 * 1024) {
      (void)sendAll(fd, respError("ERR Protocol error"));
      break;
    }
  }
  ::close(fd);
}

}  // namespace dbengine::resp

// ---- dbresp-main (klein, im gleichen TU) ------------------------------------
namespace {

using dbengine::resp::RespServer;

int connectTcp(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("selfcheck: socket() failed");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    throw std::runtime_error("selfcheck: connect() failed");
  }
  return fd;
}

bool clientSend(int fd, const std::string& s) {
  std::size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

// Liest genau ein RESP2-Reply (rekursiv fuer *; +-$: ein Frame).
// Gibt Roh-Reply (inkl. CRLF) zurueck.
std::string readReply(int fd) {
  std::string buf;
  char tmp[1024];
  auto needMore = [&](const std::string& b, int depth) -> bool {
    // Prueft ob b genau einen kompletten Top-Level-Frame enthaelt.
    // depth ist nur Rekursionsschutz beim Parsen (kein Netzzustand).
    (void)depth;
    if (b.empty()) return true;
    char t = b[0];
    std::size_t lf = b.find("\r\n");
    if (lf == std::string::npos) return true;
    if (t == '+' || t == '-' || t == ':') return false;
    if (t == '$') {
      std::string ls = b.substr(1, lf - 1);
      long long n = 0;
      try {
        n = std::stoll(ls);
      } catch (...) {
        return false;
      }
      if (n == -1) return false;
      return b.size() < lf + 2 + static_cast<std::size_t>(n) + 2;
    }
    if (t == '*') {
      long long cnt = 0;
      try {
        cnt = std::stoll(b.substr(1, lf - 1));
      } catch (...) {
        return false;
      }
      if (cnt < 0) return false;
      if (cnt == 0) return false;
      // Zaehle cnt Bulk-Frames ab lf+2.
      std::size_t pos = lf + 2;
      for (long long i = 0; i < cnt; ++i) {
        if (pos >= b.size()) return true;
        if (b[pos] != '$') return false;  // KEYS liefert nur Bulks
        std::size_t le = b.find("\r\n", pos);
        if (le == std::string::npos) return true;
        long long bl = 0;
        try {
          bl = std::stoll(b.substr(pos + 1, le - pos - 1));
        } catch (...) {
          return false;
        }
        if (bl < 0) {
          pos = le + 2;
          continue;
        }
        if (b.size() < le + 2 + static_cast<std::size_t>(bl) + 2) return true;
        pos = le + 2 + static_cast<std::size_t>(bl) + 2;
      }
      return false;
    }
    return false;
  };
  while (needMore(buf, 0)) {
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("selfcheck: recv() failed");
    }
    buf.append(tmp, static_cast<std::size_t>(n));
    if (buf.size() > 17 * 1024 * 1024) throw std::runtime_error("selfcheck: reply too large");
  }
  return buf;
}

std::string cmdMultibulk(const std::vector<std::string>& args) {
  std::string s = "*" + std::to_string(args.size()) + "\r\n";
  for (const auto& a : args) s += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
  return s;
}

int SelfCheck() {
  try {
    RespServer srv;
    srv.start(0);  // ephemeral
    int fd = connectTcp(srv.port());

    auto expect = [&](const std::string& send, const std::string& want,
                      const char* name) -> bool {
      if (!clientSend(fd, send)) {
        std::fprintf(stderr, "dbresp selfcheck: FAIL %s (send)\n", name);
        return false;
      }
      std::string got = readReply(fd);
      if (got != want) {
        std::fprintf(stderr, "dbresp selfcheck: FAIL %s: got '%s' want '%s'\n", name,
                     got.c_str(), want.c_str());
        return false;
      }
      return true;
    };

    bool ok = true;
    // Multibulk-Sequenz: SET/GET/EXISTS/KEYS/DEL + nil-Antwort.
    if (ok) ok = expect(cmdMultibulk({"SET", "sc:key1", "v1"}), "+OK\r\n", "SET");
    if (ok) ok = expect(cmdMultibulk({"GET", "sc:key1"}), "$2\r\nv1\r\n", "GET");
    if (ok) ok = expect(cmdMultibulk({"EXISTS", "sc:key1"}), ":1\r\n", "EXISTS-hit");
    if (ok) ok = expect(cmdMultibulk({"EXISTS", "sc:missing"}), ":0\r\n", "EXISTS-miss");
    if (ok) {
      // KEYS mit Prefix "sc:": muss sc:key1 enthalten.
      if (!clientSend(fd, cmdMultibulk({"KEYS", "sc:"}))) {
        std::fprintf(stderr, "dbresp selfcheck: FAIL KEYS (send)\n");
        ok = false;
      } else {
        std::string got = readReply(fd);
        if (got.find("sc:key1") == std::string::npos || got.rfind("*", 0) != 0) {
          std::fprintf(stderr, "dbresp selfcheck: FAIL KEYS: got '%s'\n", got.c_str());
          ok = false;
        }
      }
    }
    if (ok) ok = expect(cmdMultibulk({"DEL", "sc:key1"}), ":1\r\n", "DEL");
    if (ok) ok = expect(cmdMultibulk({"GET", "sc:key1"}), "$-1\r\n", "GET-nil");
    if (ok) ok = expect(cmdMultibulk({"EXISTS", "sc:key1"}), ":0\r\n", "EXISTS-after-del");
    // Inline-Eingabe: PING + unbekanntes Kommando.
    if (ok) ok = expect("PING\r\n", "+PONG\r\n", "PING-inline");
    if (ok) {
      if (!clientSend(fd, "FOOBAR\r\n")) {
        std::fprintf(stderr, "dbresp selfcheck: FAIL unknown (send)\n");
        ok = false;
      } else {
        std::string got = readReply(fd);
        if (got.rfind("-ERR unknown command", 0) != 0) {
          std::fprintf(stderr, "dbresp selfcheck: FAIL unknown: got '%s'\n", got.c_str());
          ok = false;
        }
      }
    }
    if (ok) ok = expect(cmdMultibulk({"QUIT"}), "+OK\r\n", "QUIT");
    ::close(fd);
    srv.stop();
    if (!ok) return 1;
    std::cout << "OK\n";
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "dbresp selfcheck: FAIL %s\n", e.what());
    return 1;
  }
}

int ServeForever(int port) {
  RespServer srv;
  srv.start(port);
  std::cout << "dbresp listening on 127.0.0.1:" << srv.port() << "\n" << std::flush;
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
        std::fprintf(stderr, "dbresp: invalid --port value\n");
        return 2;
      }
      if (port < 0 || port > 65535) {
        std::fprintf(stderr, "dbresp: --port out of range 0..65535\n");
        return 2;
      }
    } else {
      std::fprintf(stderr, "usage: %s [--selfcheck] [--port <n>]\n", argv[0]);
      return 2;
    }
  }
  if (selfcheck) return SelfCheck();
  if (port >= 0) return ServeForever(port);
  std::fprintf(stderr, "usage: %s [--selfcheck] [--port <n>]\n", argv[0]);
  return 2;
}
