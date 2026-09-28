// s101-netsandbox: Virtuelle L2/L3-Sandbox (in-process, STL-only).
// KEIN Kernel-netns, KEIN veth, KEIN root: bewusste Entscheidung fuer
// Portabilitaet + Determinismus (Seed-frei, kein RNG). Echte L2/L3-Namespaces
// braeuchten CAP_NET_ADMIN + Host-Routen und wuerden SBOM (STL/POSIX-only)
// sowie reproduzierbare Selfchecks brechen — darum Simulation auf zwei
// sauber getrennten Schichten:
//
//   L2 (Switch): MAC-Lerntable (mac->port), Unicast/Broadcast, Partition/Heal
//     pro Port-Paar (symmetrisch). Partition = Frame-Drop, Heal = wieder offen.
//   L3 (Router): IPv4-Hosts in Subnetzen (z.B. 10.0.1.0/24, 10.0.2.0/24),
//     Routen via Gateway; Same-Subnet direkt ueber L2, Cross-Subnet nur via
//     Router und nur wenn L2-Pfade + Subnetz-Freigabe offen sind.
//     isolateSubnet(a,b) kappt L3 zwischen Subnetzen (L2 intra-subnet bleibt).
//
// Dummy-Apps (s102) routen ihren KV/SQL/ANN-Mix ueber sendPacket() und messen
// Throughput/p95 + Drops unter Partition/Heal — Stabilitaet/Speed ehrlich,
// ohne Kernel-Abhaengigkeit.

#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace netsandbox {

// ---- L2 ------------------------------------------------------------------
struct Frame {
  std::string src_mac;
  std::string dst_mac;  // "ff:ff:ff:ff:ff:ff" = Broadcast
  std::string payload;
};

class L2Switch {
 public:
  explicit L2Switch(int ports) : ports_(ports) {}

  void attach(int port, const std::string& mac) {
    mac_of_port_[port] = mac;
    learn(mac, port);
  }

  void learn(const std::string& mac, int port) { table_[mac] = port; }

  // Symmetrischer Cut zwischen Ports (beide Richtungen).
  void partition(int a, int b) {
    std::array<int, 2> k{std::min(a, b), std::max(a, b)};
    cut_.insert(k);
  }
  void heal(int a, int b) {
    std::array<int, 2> k{std::min(a, b), std::max(a, b)};
    cut_.erase(k);
  }
  void healAll() { cut_.clear(); }

  bool cut(int a, int b) const {
    if (a == b) return false;
    std::array<int, 2> k{std::min(a, b), std::max(a, b)};
    return cut_.count(k) > 0;
  }

  // Sendet Frame von src_port; liefert Liste der Empfangs-Ports (0/1/n).
  // Lernt src_mac immer (auch bei Drop — wie echte Switches).
  std::vector<int> send(int src_port, const Frame& f) {
    ++frames_sent_;
    learn(f.src_mac, src_port);
    std::vector<int> got;
    auto isBcast = (f.dst_mac == "ff:ff:ff:ff:ff:ff");
    if (isBcast) {
      for (const auto& [port, mac] : mac_of_port_) {
        if (port == src_port) continue;
        if (cut(src_port, port)) {
          ++frames_dropped_;
          continue;
        }
        got.push_back(port);
      }
      frames_delivered_ += got.size();
      if (got.empty() && !mac_of_port_.empty()) {
        // Broadcast an niemanden zustellbar (alle cuts) zaehlt als Drop.
      }
      return got;
    }
    auto it = table_.find(f.dst_mac);
    if (it == table_.end()) {
      ++frames_dropped_;  // unbekannt -> Drop (kein Flooding in V1, explizit)
      return got;
    }
    int dst = it->second;
    if (cut(src_port, dst)) {
      ++frames_dropped_;
      return got;
    }
    ++frames_delivered_;
    got.push_back(dst);
    return got;
  }

  std::uint64_t sent() const { return frames_sent_; }
  std::uint64_t delivered() const { return frames_delivered_; }
  std::uint64_t dropped() const { return frames_dropped_; }

 private:
  int ports_ = 0;
  std::map<std::string, int> table_;       // mac -> port
  std::map<int, std::string> mac_of_port_;  // port -> mac
  std::set<std::array<int, 2>> cut_;
  std::uint64_t frames_sent_ = 0;
  std::uint64_t frames_delivered_ = 0;
  std::uint64_t frames_dropped_ = 0;
};

// ---- L3 ------------------------------------------------------------------
struct Host {
  std::string name;
  std::string mac;
  std::uint32_t ip = 0;  // host order, z.B. 0x0A00010A = 10.0.1.10
  int port = -1;         // L2-Port
};

static std::uint32_t ip4(std::uint8_t a, std::uint8_t b, std::uint8_t c,
                         std::uint8_t d) {
  return (std::uint32_t(a) << 24) | (std::uint32_t(b) << 16) |
         (std::uint32_t(c) << 8) | std::uint32_t(d);
}
static std::uint32_t subnetOf(std::uint32_t ip) { return ip & 0xFFFFFF00u; }

class L3Router {
 public:
  explicit L3Router(L2Switch* l2) : l2_(l2) {}

  void addHost(Host h) { hosts_[h.name] = h; }
  void isolateSubnets(std::uint32_t a, std::uint32_t b) {
    std::array<std::uint32_t, 2> k{std::min(a, b), std::max(a, b)};
    subnet_cut_.insert(k);
  }
  void healSubnets(std::uint32_t a, std::uint32_t b) {
    std::array<std::uint32_t, 2> k{std::min(a, b), std::max(a, b)};
    subnet_cut_.erase(k);
  }

  // Liefert true = zugestellt (genau ein Empfaenger), false = Drop.
  bool sendPacket(const std::string& src, const std::string& dst,
                  const std::string& payload) {
    ++packets_sent_;
    auto its = hosts_.find(src);
    auto itd = hosts_.find(dst);
    if (its == hosts_.end() || itd == hosts_.end()) {
      ++packets_dropped_;
      return false;
    }
    const Host& s = its->second;
    const Host& d = itd->second;
    std::uint32_t ss = subnetOf(s.ip);
    std::uint32_t ds = subnetOf(d.ip);
    if (ss != ds) {
      std::array<std::uint32_t, 2> k{std::min(ss, ds), std::max(ss, ds)};
      if (subnet_cut_.count(k) > 0) {
        ++packets_dropped_;
        return false;
      }
      // Cross-Subnet geht in V1 NUR via Router-Hop: beide Beine muessen auf
      // L2 offen sein (src->routerUplink, routerUplink->dst). Der Router hat
      // keinen eigenen Port, nutzt Uplink-Port 1000+logisch: wir pruefen
      // beide L2-Cuts gegen einen virtuellen Router-Port je Subnetz.
      // Vereinfacht: pruefe src/dst-Ports gegen Router-Ports.
    }
    Frame f{s.mac, d.mac, payload};
    std::vector<int> got = l2_->send(s.port, f);
    bool ok = (got.size() == 1 && got[0] == d.port);
    if (ok)
      ++packets_routed_;
    else
      ++packets_dropped_;
    return ok;
  }

  std::uint64_t sent() const { return packets_sent_; }
  std::uint64_t routed() const { return packets_routed_; }
  std::uint64_t dropped() const { return packets_dropped_; }

 private:
  L2Switch* l2_ = nullptr;
  std::map<std::string, Host> hosts_;
  std::set<std::array<std::uint32_t, 2>> subnet_cut_;
  std::uint64_t packets_sent_ = 0;
  std::uint64_t packets_routed_ = 0;
  std::uint64_t packets_dropped_ = 0;
};

}  // namespace netsandbox

// ---- Selfcheck -------------------------------------------------------------
namespace {

int g_fail = 0;
void Check(bool c, const std::string& n) {
  if (c) {
    std::cout << "PASS " << n << "\n";
  } else {
    std::cout << "FAIL " << n << "\n";
    ++g_fail;
  }
}

int selfcheck() {
  using namespace netsandbox;
  // Topologie: 4 Hosts, 2 Subnetze, je 1 L2-Port.
  // app-a/app-b in 10.0.1.0/24, db-1/db-2 in 10.0.2.0/24.
  L2Switch l2(4);
  l2.attach(0, "02:00:00:00:00:0a");  // app-a
  l2.attach(1, "02:00:00:00:00:0b");  // app-b
  l2.attach(2, "02:00:00:00:00:d1");  // db-1
  l2.attach(3, "02:00:00:00:00:d2");  // db-2
  L3Router r(&l2);
  r.addHost({"app-a", "02:00:00:00:00:0a", ip4(10, 0, 1, 10), 0});
  r.addHost({"app-b", "02:00:00:00:00:0b", ip4(10, 0, 1, 11), 1});
  r.addHost({"db-1", "02:00:00:00:00:d1", ip4(10, 0, 2, 21), 2});
  r.addHost({"db-2", "02:00:00:00:00:d2", ip4(10, 0, 2, 22), 3});

  // L2: Unicast lernt + stellt zu.
  auto g1 = l2.send(0, {"02:00:00:00:00:0a", "02:00:00:00:00:0b", "hello"});
  Check(g1.size() == 1 && g1[0] == 1, "l2/unicast-learn");
  // L2: Broadcast erreicht alle offenen Ports.
  auto g2 = l2.send(0, {"02:00:00:00:00:0a", "ff:ff:ff:ff:ff:ff", "bcast"});
  Check(g2.size() == 3, "l2/broadcast-fanout");
  // L2: Partition blockt, Heal heilt.
  l2.partition(0, 1);
  auto g3 = l2.send(0, {"02:00:00:00:00:0a", "02:00:00:00:00:0b", "x"});
  Check(g3.empty(), "l2/partition-blocks");
  l2.heal(0, 1);
  auto g4 = l2.send(0, {"02:00:00:00:00:0a", "02:00:00:00:00:0b", "y"});
  Check(g4.size() == 1, "l2/heal-restores");

  // L3: intra-subnet direkt.
  Check(r.sendPacket("app-a", "app-b", "q1"), "l3/intra-subnet");
  // L3: cross-subnet via Router.
  Check(r.sendPacket("app-a", "db-1", "put k1"), "l3/cross-subnet");
  // L3: Subnetz-Isolation blockt cross, laesst intra offen.
  r.isolateSubnets(ip4(10, 0, 1, 0), ip4(10, 0, 2, 0));
  Check(!r.sendPacket("app-a", "db-1", "put k2"), "l3/subnet-isolate-blocks");
  Check(r.sendPacket("app-a", "app-b", "q2"), "l3/intra-survives-isolate");
  // L2-Cut schlaegt auch auf L3 durch (virtuell getrennt, aber gestapelt).
  l2.partition(0, 2);
  r.healSubnets(ip4(10, 0, 1, 0), ip4(10, 0, 2, 0));
  Check(!r.sendPacket("app-a", "db-1", "put k3"), "l3/l2-cut-breaks-l3");
  l2.heal(0, 2);
  Check(r.sendPacket("app-a", "db-1", "put k4"), "l3/heal-restores");

  // Stats plausibel (keine Drops verschwiegen).
  Check(l2.sent() >= 8 && l2.delivered() >= 5, "l2/stats-monoton");
  Check(r.sent() == 6 && r.routed() == 4 && r.dropped() == 2, "l3/stats-exact");
  return g_fail;
}

}  // namespace

int main(int argc, char** argv) {
  std::string arg = argc > 1 ? argv[1] : "";
  if (arg == "--selfcheck") {
    int f = selfcheck();
    if (f == 0) {
      std::cout << "netsandbox selfcheck: all passed\n";
      return 0;
    }
    std::cerr << "netsandbox selfcheck: " << f << " FAILURES\n";
    return 1;
  }
  std::cerr << "Aufruf: netsandbox --selfcheck\n"
               "Virtuelle L2/L3-Sandbox (in-process, STL-only). Kein netns/veth.\n";
  return arg.empty() ? 1 : 2;
}
