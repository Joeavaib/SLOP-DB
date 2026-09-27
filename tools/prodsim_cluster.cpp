// Prodsim TCP-Harness: "Hosts" auf einer Maschine via 127.0.0.1 Loopback.
// Das ist KEIN zweites Raft und KEIN Multi-DC-Cluster. RaftGroup bleibt
// in-process (Partition-Matrix in shard.cpp ist die Consensus-Wahrheit).
// Hier: Wire-Codec + echtes TCP (ein Prozess, drei Links) plus fork()
// fuer einen 2-Prozess-Roundtrip. Partition = close(fd), Heal = reconnect.

#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dbengine/raft/shard.h"

using dbengine::raft::DecodeEntryWire;
using dbengine::raft::EncodeEntryWire;
using dbengine::raft::Entry;
using dbengine::raft::RecvWire;
using dbengine::raft::SendWire;
using dbengine::raft::TcpLoopbackPair;

namespace {

int g_failures = 0;

void Check(bool cond, const std::string& name) {
  if (cond) {
    std::cout << "PASS " << name << "\n";
  } else {
    std::cout << "FAIL " << name << "\n";
    ++g_failures;
  }
}

void CloseFd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

bool Exchange(int send_fd, int recv_fd, const Entry& e) {
  const std::string payload = EncodeEntryWire(e);
  if (!SendWire(send_fd, payload)) return false;
  std::string got;
  if (!RecvWire(recv_fd, got)) return false;
  Entry out;
  if (!DecodeEntryWire(got, out)) return false;
  return out.term == e.term && out.index == e.index && out.command == e.command;
}

}  // namespace

int main() {
  // Drei Loopback-Links (0-1, 1-2, 2-0) — emulierte Hosts, echte Sockets.
  int c01 = -1, s01 = -1, c12 = -1, s12 = -1, c20 = -1, s20 = -1;
  Check(TcpLoopbackPair(c01, s01), "net/pair-01");
  Check(TcpLoopbackPair(c12, s12), "net/pair-12");
  Check(TcpLoopbackPair(c20, s20), "net/pair-20");

  Entry e1{1, 1, "put a 1"};
  Entry e2{1, 2, "put b 2"};
  Entry e3{1, 3, "put c 3"};
  Check(Exchange(c01, s01, e1), "net/exchange-01");
  Check(Exchange(c12, s12, e2), "net/exchange-12");
  Check(Exchange(c20, s20, e3), "net/exchange-20");

  // Partition: Link 0-1 kappen. Send/Recv muessen fehlschlagen.
  CloseFd(c01);
  Check(!SendWire(c01, EncodeEntryWire(e1)), "net/partition-send-fails");
  std::string dumped;
  Check(!RecvWire(s01, dumped), "net/partition-recv-fails");
  CloseFd(s01);

  // Heal: neuen Loopback-Link, Traffic geht wieder.
  Check(TcpLoopbackPair(c01, s01), "net/reconnect-01");
  Check(Exchange(c01, s01, Entry{2, 4, "put d 4"}), "net/exchange-after-heal");

  CloseFd(c01);
  CloseFd(s01);
  CloseFd(c12);
  CloseFd(s12);
  CloseFd(c20);
  CloseFd(s20);

  // 2-Prozess-Roundtrip (echte separate Adressraeume, weiterhin Loopback).
  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  Check(listen_fd >= 0, "mp/listen-socket");
  int one = 1;
  if (listen_fd >= 0) {
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  bool bound = listen_fd >= 0 &&
               ::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
                   0 &&
               ::listen(listen_fd, 1) == 0;
  socklen_t alen = sizeof(addr);
  if (bound) {
    bound = ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen) ==
            0;
  }
  Check(bound, "mp/bind-listen");
  if (!bound) {
    CloseFd(listen_fd);
    std::cout << (g_failures ? std::to_string(g_failures) + " PRODSIM_NET FAILED\n"
                             : "ALL PRODSIM_NET PASSED\n");
    return g_failures ? 1 : 0;
  }

  pid_t pid = ::fork();
  if (pid == 0) {
    ::close(listen_fd);
    int cli = ::socket(AF_INET, SOCK_STREAM, 0);
    if (cli < 0) _exit(2);
    if (::connect(cli, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(cli);
      _exit(3);
    }
    Entry child{7, 9, "put child 1"};
    bool ok = SendWire(cli, EncodeEntryWire(child));
    ::close(cli);
    _exit(ok ? 0 : 4);
  }
  if (pid < 0) {
    Check(false, "mp/fork");
    CloseFd(listen_fd);
  } else {
    int srv = ::accept(listen_fd, nullptr, nullptr);
    CloseFd(listen_fd);
    Check(srv >= 0, "mp/accept");
    std::string payload;
    bool recvd = srv >= 0 && RecvWire(srv, payload);
    Entry out;
    bool decoded = recvd && DecodeEntryWire(payload, out) && out.term == 7 &&
                   out.index == 9 && out.command == "put child 1";
    Check(decoded, "mp/parent-recv-child");
    if (srv >= 0) ::close(srv);
    int st = 0;
    ::waitpid(pid, &st, 0);
    Check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "mp/child-exit");
  }

  if (g_failures == 0) {
    std::cout << "ALL PRODSIM_NET PASSED\n";
    return 0;
  }
  std::cout << g_failures << " PRODSIM_NET TEST(S) FAILED\n";
  return 1;
}
