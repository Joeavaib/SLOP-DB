// MVCC-Tests (s04): dirty-read verhindert, repeatable-read ok,
// write-write serialisiert. Ohne GTest (keine Dep), via CTest.
// Zusaetzlich: read-own-writes, abort, delete-tombstone, purge/Undo-GC.

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dbengine/txn/mvcc.h"

using dbengine::txn::Isolation;
using dbengine::txn::MvccStore;

static int g_pass = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __func__ << ":" << __LINE__ << ": " #cond "\n"; \
      return false;                                                          \
    }                                                                        \
  } while (0)

static bool t_visible_helper() {
  dbengine::txn::Version v;
  v.trx_begin = 5;
  v.trx_end = 10;
  CHECK(dbengine::txn::IsVisible(v, 5));
  CHECK(dbengine::txn::IsVisible(v, 9));
  CHECK(!dbengine::txn::IsVisible(v, 4));
  CHECK(!dbengine::txn::IsVisible(v, 10));
  return true;
}

static bool t_dirty_read_blocked() {
  MvccStore s;
  // Seed: k=v0 committed.
  {
    auto w = s.TryBeginWrite();
    CHECK(w.has_value());
    CHECK(s.Write(*w, "k", "v0"));
    CHECK(s.Commit(*w));
  }
  // W1 schreibt uncommitted v1.
  auto w1 = s.TryBeginWrite();
  CHECK(w1.has_value());
  CHECK(s.Write(*w1, "k", "v1"));
  // R2 darf v1 NICHT sehen (kein Dirty-Read), sondern v0.
  {
    auto r2 = s.BeginRead();
    auto v = s.Read(r2, "k");
    CHECK(v.has_value() && *v == "v0");
    CHECK(s.Commit(r2));
  }
  // Nach Commit sieht neue Txn v1.
  CHECK(s.Commit(*w1));
  {
    auto r3 = s.BeginRead();
    auto v = s.Read(r3, "k");
    CHECK(v.has_value() && *v == "v1");
    CHECK(s.Commit(r3));
  }
  return true;
}

static bool t_repeatable_read() {
  MvccStore s;
  {
    auto w = s.TryBeginWrite();
    CHECK(w.has_value());
    CHECK(s.Write(*w, "k", "v0"));
    CHECK(s.Commit(*w));
  }
  auto r1 = s.BeginRead();  // Snapshot fixiert v0
  CHECK(s.Read(r1, "k") == std::optional<std::string>("v0"));
  // Konkurrierender Commit v1.
  {
    auto w2 = s.TryBeginWrite();
    CHECK(w2.has_value());
    CHECK(s.Write(*w2, "k", "v1"));
    CHECK(s.Commit(*w2));
  }
  // R1 muss weiterhin v0 sehen (repeatable), neue Txn sieht v1.
  CHECK(s.Read(r1, "k") == std::optional<std::string>("v0"));
  CHECK(s.Commit(r1));
  {
    auto r2 = s.BeginRead();
    CHECK(s.Read(r2, "k") == std::optional<std::string>("v1"));
    CHECK(s.Commit(r2));
  }
  return true;
}

static bool t_write_write_serialized() {
  MvccStore s;
  auto w1 = s.TryBeginWrite();
  CHECK(w1.has_value());
  // Zweiter Writer muss scheitern solange w1 aktiv (Single-Writer-Lock).
  auto w2 = s.TryBeginWrite();
  CHECK(!w2.has_value());
  CHECK(s.Write(*w1, "k", "a"));
  CHECK(s.Commit(*w1));
  // Nach Commit ist Lock frei.
  auto w3 = s.TryBeginWrite();
  CHECK(w3.has_value());
  CHECK(s.Write(*w3, "k", "b"));
  CHECK(s.Commit(*w3));
  auto r = s.BeginRead();
  CHECK(s.Read(r, "k") == std::optional<std::string>("b"));
  CHECK(s.Commit(r));
  return true;
}

static bool t_write_write_threads() {
  MvccStore s;
  std::atomic<int> wins{0};
  std::atomic<bool> ok{true};
  auto w2 = [&](const char* val) {
    for (int i = 0; i < 200; ++i) {
      auto w = s.TryBeginWrite();
      if (w.has_value()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!s.Write(*w, "k", val)) {
          s.Abort(*w);
          ok = false;
          return;
        }
        if (!s.Commit(*w)) {
          ok = false;
          return;
        }
        wins.fetch_add(1);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ok = false;  // nie Lock bekommen -> Fehler
  };
  std::thread t1(w2, "t1");
  std::thread t2(w2, "t2");
  t1.join();
  t2.join();
  CHECK(ok.load());
  CHECK(wins.load() == 2);  // serialisiert nacheinander, beide committed
  auto r = s.BeginRead();
  auto v = s.Read(r, "k");
  CHECK(v.has_value() && (*v == "t1" || *v == "t2"));
  CHECK(s.Commit(r));
  CHECK(s.VersionCount("k") == 2);
  return true;
}

static bool t_read_own_writes_and_abort() {
  MvccStore s;
  auto w = s.TryBeginWrite();
  CHECK(w.has_value());
  CHECK(s.Write(*w, "k", "tmp"));
  // Eigene Writes sichtbar.
  CHECK(s.Read(*w, "k") == std::optional<std::string>("tmp"));
  s.Abort(*w);
  // Nach Abort nichts sichtbar.
  auto r = s.BeginRead();
  CHECK(!s.Read(r, "k").has_value());
  CHECK(s.Commit(r));
  return true;
}

static bool t_delete_and_purge() {
  MvccStore s;
  for (const char* v : {"v0", "v1", "v2"}) {
    auto w = s.TryBeginWrite();
    CHECK(w.has_value());
    CHECK(s.Write(*w, "k", v));
    CHECK(s.Commit(*w));
  }
  CHECK(s.VersionCount("k") == 3);
  // Delete -> Tombstone.
  {
    auto w = s.TryBeginWrite();
    CHECK(w.has_value());
    CHECK(s.Erase(*w, "k"));
    CHECK(s.Commit(*w));
  }
  {
    auto r = s.BeginRead();
    CHECK(!s.Read(r, "k").has_value());
    CHECK(s.Commit(r));
  }
  CHECK(s.VersionCount("k") == 4);
  // Kein aktiver Snapshot -> Purge darf bis auf neueste alles raeumen.
  std::size_t freed = s.Purge();
  CHECK(freed == 3);
  CHECK(s.VersionCount("k") == 1);
  return true;
}

static bool t_read_only_needs_no_writer_lock() {
  MvccStore s;
  auto w = s.TryBeginWrite();
  CHECK(w.has_value());
  // Reads parallel zum aktiven Writer muessen gehen.
  auto r = s.BeginRead();
  CHECK(s.Write(*w, "x", "1"));
  CHECK(!s.Read(r, "x").has_value());  // Snapshot vor Commit
  CHECK(s.Commit(*w));
  CHECK(s.Commit(r));
  return true;
}

int main() {
  using Fn = bool (*)();
  std::vector<std::pair<std::string, Fn>> cases = {
      {"visible_helper", t_visible_helper},
      {"dirty_read_blocked", t_dirty_read_blocked},
      {"repeatable_read", t_repeatable_read},
      {"write_write_serialized", t_write_write_serialized},
      {"write_write_threads", t_write_write_threads},
      {"read_own_writes_and_abort", t_read_own_writes_and_abort},
      {"delete_and_purge", t_delete_and_purge},
      {"read_only_parallel", t_read_only_needs_no_writer_lock},
  };
  int fail = 0;
  for (auto& [name, fn] : cases) {
    bool ok = false;
    try {
      ok = fn();
    } catch (const std::exception& e) {
      std::cerr << "EXC " << name << ": " << e.what() << "\n";
      ok = false;
    } catch (...) {
      std::cerr << "EXC " << name << ": unknown\n";
      ok = false;
    }
    std::cout << (ok ? "PASS " : "FAIL ") << name << "\n";
    if (ok)
      ++g_pass;
    else
      ++fail;
  }
  std::cout << g_pass << "/" << cases.size() << " mvcc tests passed\n";
  return fail == 0 ? 0 : 1;
}
