// s12-pgserver Tests: Thread-Server, Client-Socket sendet Startup+Q,
// erwartet AuthOk + ReadyForQuery + T/D/C/Z (SELECT 1). Kein gtest.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "dbengine/server/pgserver.h"
#include "dbengine/server/pgwire.h"

using dbengine::pgwire::getInt32BE;
using dbengine::pgwire::putInt32BE;

static int g_fail = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";                     \
      ++g_fail;                                                                \
    }                                                                          \
  } while (0)

namespace {

bool sendAll(int fd, const std::vector<uint8_t>& v) {
  std::size_t off = 0;
  while (off < v.size()) {
    ssize_t n = ::send(fd, v.data() + off, v.size() - off, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

bool recvAll(int fd, uint8_t* out, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    ssize_t n = ::recv(fd, out + off, len - off, 0);
    if (n == 0) return false;
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

// Eine typisierte Nachricht lesen: gibt (type, payload-ohne-header) zurueck.
bool readMsg(int fd, char& type, std::vector<uint8_t>& payload, int32_t& len) {
  uint8_t hdr[5];
  if (!recvAll(fd, hdr, 5)) return false;
  type = static_cast<char>(hdr[0]);
  len = getInt32BE(hdr + 1);
  if (len < 4) return false;
  payload.resize(static_cast<std::size_t>(len) - 4);
  if (!payload.empty() && !recvAll(fd, payload.data(), payload.size()))
    return false;
  return true;
}

int connectLoopback(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  if (::connect(fd, (sockaddr*)&a, sizeof(a)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::vector<uint8_t> encodeQ(const std::string& q) {
  std::vector<uint8_t> out;
  out.push_back('Q');
  putInt32BE(out, static_cast<int32_t>(q.size() + 1 + 4));
  out.insert(out.end(), q.begin(), q.end());
  out.push_back(0);
  return out;
}

}  // namespace

int main() {
  using dbengine::pgserver::PgServer;
  auto t0 = std::chrono::steady_clock::now();

  PgServer server;
  server.start();
  CHECK(server.running());
  CHECK(server.port() > 0);
  std::cerr << "pgserver listening on 127.0.0.1:" << server.port() << "\n";

  int fd = connectLoopback(server.port());
  CHECK(fd >= 0);
  if (fd < 0) {
    server.stop();
    return 1;
  }

  // 1) Startup senden.
  auto startup =
      dbengine::pgwire::encodeStartupRequest({{"user", "joe"}});
  CHECK(sendAll(fd, startup));

  // 2) AuthOk ('R') + ReadyForQuery ('Z') erwarten.
  char type = 0;
  std::vector<uint8_t> pay;
  int32_t len = 0;
  CHECK(readMsg(fd, type, pay, len));
  CHECK(type == 'R');
  CHECK(readMsg(fd, type, pay, len));
  CHECK(type == 'Z');
  CHECK(len == 5);
  CHECK(!pay.empty() && pay[0] == 'I');

  // 3) Q "SELECT 1" senden, T/D/C/Z erwarten.
  auto q0 = std::chrono::steady_clock::now();
  CHECK(sendAll(fd, encodeQ("SELECT 1")));

  CHECK(readMsg(fd, type, pay, len));
  CHECK(type == 'T');  // RowDescription
  CHECK(readMsg(fd, type, pay, len));
  CHECK(type == 'D');  // DataRow
  {
    std::string blob((char*)pay.data(), pay.size());
    CHECK(blob.find("1") != std::string::npos);
  }
  CHECK(readMsg(fd, type, pay, len));
  CHECK(type == 'C');  // CommandComplete
  {
    std::string blob((char*)pay.data(), pay.size());
    CHECK(blob.find("SELECT 1") != std::string::npos);
  }
  CHECK(readMsg(fd, type, pay, len));
  CHECK(type == 'Z');  // ReadyForQuery
  auto q1 = std::chrono::steady_clock::now();
  auto usec =
      std::chrono::duration_cast<std::chrono::microseconds>(q1 - q0).count();
  std::cerr << "SELECT 1 roundtrip latency: " << usec << " us\n";

  // 4) Zweite Query auf gleicher Connection (Loop-Nachweis).
  CHECK(sendAll(fd, encodeQ("SELECT 1;")));
  CHECK(readMsg(fd, type, pay, len));
  CHECK(type == 'T');
  CHECK(readMsg(fd, type, pay, len));
  CHECK(type == 'D');
  CHECK(readMsg(fd, type, pay, len));
  CHECK(type == 'C');
  CHECK(readMsg(fd, type, pay, len));
  CHECK(type == 'Z');

  // 5) Terminate + close.
  std::vector<uint8_t> term = {'X', 0, 0, 0, 4};
  CHECK(sendAll(fd, term));
  ::close(fd);

  server.stop();
  CHECK(!server.running());

  auto t1 = std::chrono::steady_clock::now();
  auto total =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  std::cerr << "pgserver test total: " << total << " ms\n";

  if (g_fail == 0) {
    std::cout << "pgserver tests: all passed\n";
    return 0;
  }
  std::cerr << "pgserver tests: " << g_fail << " FAILURES\n";
  return 1;
}
