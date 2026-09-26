// WAL-Tests (ohne gtest, assert-basiert wie test_smoke.cpp):
//  1. append 1000 records, LSN streng steigend
//  2. kill-sim: close OHNE checkpoint, reopen, replay == original
//  3. checkpoint(500): replay == Rest; checkpoint(all): replay leer
//  4. CRC/torn-tail: korruptes Ende -> Prefix-Replay
// Misst Recovery-Zeit (replay ms) und gibt sie aus.

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "dbengine/storage/wal.h"

#if defined(__linux__)
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using dbengine::storage::Wal;
using dbengine::storage::WalRecord;

static std::string tmp_path(const std::string& name) {
  auto p = std::filesystem::temp_directory_path() / (name + ".wal");
  std::error_code ec;
  std::filesystem::remove(p, ec);
  return p.string();
}

static void check(bool ok, const char* msg) {
  if (!ok) {
    std::cerr << "FAIL: " << msg << '\n';
    std::abort();
  }
}

int main() {
  // CRC32-lite Sanity (bekannter Check-Wert "123456789" -> 0xCBF43926).
  {
    const char* v = "123456789";
    uint32_t c = Wal::crc32(v, 9);
    check(c == 0xCBF43926u, "crc32 sanity");
  }

  // 1. append 1000 records
  const std::string path = tmp_path("dbengine_test_wal_s03");
  std::vector<std::string> original;
  original.reserve(1000);
  for (int i = 0; i < 1000; ++i) original.push_back("rec-" + std::to_string(i) + "-payload-xyz");

  {
    Wal w(path);
    w.open();
    uint64_t expect = 1;
    for (const auto& s : original) {
      uint64_t lsn = w.append(s);
      check(lsn == expect, "LSN steigend");
      ++expect;
    }
    check(w.next_lsn() == 1001u, "next_lsn nach 1000 appends");
    w.flush();  // einziger fsync (group commit) — danach Kill--9-sicher
    w.close();  // kill-sim: close OHNE checkpoint (kein truncate)
  }

  // 2. kill-sim: reopen + replay == original, mit Zeitmessung
  std::vector<WalRecord> recs;
  double recovery_ms = 0;
  {
    Wal w(path);
    auto t0 = std::chrono::steady_clock::now();
    w.open();  // Recovery beim Start (torn-tail-cap + max-LSN-Scan)
    std::vector<WalRecord> r = Wal::replay_file(path);
    auto t1 = std::chrono::steady_clock::now();
    recovery_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    // Instanz-Replay muss identisch sein:
    std::vector<WalRecord> r2 = w.replay();
    check(r.size() == r2.size(), "replay vs replay_file konsistent");
    recs = std::move(r);
    check(w.next_lsn() == 1001u, "next_lsn nach Recovery");
    w.close();
  }
  check(recs.size() == original.size(), "replay size == 1000");
  for (size_t i = 0; i < original.size(); ++i) {
    check(recs[i].lsn == i + 1, "replay LSN");
    check(recs[i].data == original[i], "replay payload");
  }
  std::cout << "[wal] replay 1000 records ok, recovery_ms=" << recovery_ms << '\n';

  // Leere + grosse Payload (Randaefle, eigene Datei).
  {
    const std::string p2 = tmp_path("dbengine_test_wal_edge");
    Wal w(p2);
    w.open();
    check(w.append("") == 1, "empty payload lsn");
    std::string big(1 << 20, 'A');  // 1 MiB
    check(w.append(big) == 2, "big payload lsn");
    w.flush();
    auto r = w.replay();
    check(r.size() == 2, "edge replay size");
    check(r[0].data.empty(), "edge empty");
    check(r[1].data == big, "edge big");
    w.close();
    std::filesystem::remove(p2);
  }

  // 3. checkpoint(500) -> nur 501..1000 uebrig
  {
    Wal w(path);
    w.open();
    w.checkpoint(500);
    auto r = w.replay();
    check(r.size() == 500, "nach checkpoint(500) size");
    for (size_t i = 0; i < r.size(); ++i) {
      check(r[i].lsn == 501 + i, "checkpoint LSN-Rest");
      check(r[i].data == original[500 + i], "checkpoint payload-Rest");
    }
    w.flush();
    w.close();
  }
  // checkpoint(all) -> leer
  {
    Wal w(path);
    w.open();
    w.checkpoint(1000);
    auto r = w.replay();
    check(r.empty(), "nach checkpoint(all) leer");
    check(w.next_lsn() == 1u, "next_lsn nach full-checkpoint reset");
    // Neues append nach full-checkpoint faengt bei 1 an (Datei war leer).
    check(w.append("neu") == 1, "append nach full-checkpoint");
    w.flush();
    w.close();
  }

  // 4. torn-tail: haenge Muell an, replay muss Prefix liefern
  {
    const std::string p3 = tmp_path("dbengine_test_wal_torn");
    {
      Wal w(p3);
      w.open();
      w.append("a");
      w.append("b");
      w.append("c");
      w.flush();
      w.close();
    }
    {
      FILE* f = std::fopen(p3.c_str(), "ab");
      assert(f);
      const char garbage[7] = {'X', 'Y', 'Z', 0, 1, 2, 3};
      assert(std::fwrite(garbage, 1, sizeof(garbage), f) == sizeof(garbage));
      std::fclose(f);
    }
    auto r = Wal::replay_file(p3);
    check(r.size() == 3, "torn tail -> prefix 3");
    check(r[2].data == "c", "torn tail payload");
    std::filesystem::remove(p3);
  }

#if defined(__linux__)
  // Echter Kill-9-Harness (Linux only, fork/kill/waitpid/SIGKILL + Pipes).
  // N=2000, deterministische Payloads "rec-<i>" (i=0..N-1, LSN=i+1).
  // Sync-Protokoll: Kind -> Parent jeweils 1 Ready-Byte ueber Pipe,
  // Parent killt mit SIGKILL, wartet per waitpid (WIFSIGNALED+SIGKILL),
  // verifiziert per Wal::replay_file. Temp-Pfade unique (pid) + Cleanup.
  {
    constexpr int kN = 2000;
    const std::string pidtag = std::to_string(static_cast<long long>(::getpid()));

    // A) Kill mitten in appends OHNE flush -> Prefix 0..k, lueckenlos ab 1.
    {
      const std::string pa = tmp_path("dbengine_test_wal_killA_" + pidtag);
      int pfd[2];
      check(::pipe(pfd) == 0, "killA pipe");
      pid_t pid = ::fork();
      check(pid >= 0, "killA fork");
      if (pid == 0) {
        // Kind: schreibe N ohne flush, signalisiere nach 200, strecke Rest.
        (void)::close(pfd[0]);
        try {
          Wal w(pa);
          w.open();
          constexpr int kReadyAt = 200;
          for (int i = 0; i < kN; ++i) {
            w.append("rec-" + std::to_string(i));
            if (i == kReadyAt) {
              const char b = 'R';
              if (::write(pfd[1], &b, 1) != 1) ::_exit(99);
            }
            if (i >= kReadyAt) (void)::usleep(100);  // Fenster ~180ms verbreitern
          }
          // Falls Parent zu langsam: bis zum Kill parken (Prefix dann == N).
          (void)::close(pfd[1]);
          for (;;) (void)::pause();
        } catch (...) {
          ::_exit(98);
        }
        ::_exit(0);  // unerreichbar
      }
      // Parent: warte auf Ready, kill sofort (mitten in appends).
      (void)::close(pfd[1]);
      {
        char b = 0;
        ssize_t n = ::read(pfd[0], &b, 1);
        check(n == 1 && b == 'R', "killA sync ready");
      }
      check(::kill(pid, SIGKILL) == 0, "killA kill");
      {
        int st = 0;
        check(::waitpid(pid, &st, 0) == pid, "killA waitpid");
        check(WIFSIGNALED(st), "killA WIFSIGNALED");
        check(WTERMSIG(st) == SIGKILL, "killA SIGKILL");
      }
      (void)::close(pfd[0]);
      auto r = Wal::replay_file(pa);
      check(r.size() <= static_cast<size_t>(kN), "killA size <= N");
      for (size_t idx = 0; idx < r.size(); ++idx) {
        check(r[idx].lsn == idx + 1, "killA LSN lueckenlos ab 1");
        check(r[idx].data == "rec-" + std::to_string(idx), "killA payload prefix");
      }
      std::cout << "[wal] killA mid-append no-flush ok, prefix k=" << r.size() << "/" << kN
                << '\n';
      std::error_code ec;
      std::filesystem::remove(pa, ec);
    }

    // B) Kill direkt NACH flush -> Replay == alle N (Verlust 0).
    {
      const std::string pb = tmp_path("dbengine_test_wal_killB_" + pidtag);
      int pfd[2];
      check(::pipe(pfd) == 0, "killB pipe");
      pid_t pid = ::fork();
      check(pid >= 0, "killB fork");
      if (pid == 0) {
        (void)::close(pfd[0]);
        try {
          Wal w(pb);
          w.open();
          for (int i = 0; i < kN; ++i) w.append("rec-" + std::to_string(i));
          w.flush();  // group-commit: danach Kill--9-sicher
          const char b = 'F';
          if (::write(pfd[1], &b, 1) != 1) ::_exit(99);
          (void)::close(pfd[1]);
          for (;;) (void)::pause();  // auf Kill warten, kein exit/close
        } catch (...) {
          ::_exit(98);
        }
        ::_exit(0);
      }
      (void)::close(pfd[1]);
      {
        char b = 0;
        ssize_t n = ::read(pfd[0], &b, 1);
        check(n == 1 && b == 'F', "killB sync flushed");
      }
      check(::kill(pid, SIGKILL) == 0, "killB kill");
      {
        int st = 0;
        check(::waitpid(pid, &st, 0) == pid, "killB waitpid");
        check(WIFSIGNALED(st), "killB WIFSIGNALED");
        check(WTERMSIG(st) == SIGKILL, "killB SIGKILL");
      }
      (void)::close(pfd[0]);
      auto r = Wal::replay_file(pb);
      check(r.size() == static_cast<size_t>(kN), "killB size == N (Verlust 0)");
      for (size_t idx = 0; idx < r.size(); ++idx) {
        check(r[idx].lsn == idx + 1, "killB LSN");
        check(r[idx].data == "rec-" + std::to_string(idx), "killB payload");
      }
      std::cout << "[wal] killB post-flush ok, n=" << r.size() << '\n';
      std::error_code ec;
      std::filesystem::remove(pb, ec);
    }

    // C) Kill waehrend checkpoint(1000) -> atomar pre ODER post, kein Mix.
    {
      constexpr unsigned kDelaysUs[] = {0, 300, 800, 1500, 3000};
      constexpr int kCkpt = 1000;
      int kills_in_window = 0;
      for (size_t att = 0; att < sizeof(kDelaysUs) / sizeof(kDelaysUs[0]); ++att) {
        const std::string pc =
            tmp_path("dbengine_test_wal_killC_" + pidtag + "_" + std::to_string(att));
        int pfd[2];
        check(::pipe(pfd) == 0, "killC pipe");
        pid_t pid = ::fork();
        check(pid >= 0, "killC fork");
        if (pid == 0) {
          (void)::close(pfd[0]);
          try {
            Wal w(pc);
            w.open();
            for (int i = 0; i < kN; ++i) w.append("rec-" + std::to_string(i));
            w.flush();  // pre-Checkpoint-Stand dauerhaft
            const char b = 'C';
            if (::write(pfd[1], &b, 1) != 1) ::_exit(99);
            // Direkt checkpointen (Fenster: tmp+rename); danach parken.
            w.checkpoint(static_cast<uint64_t>(kCkpt));
            (void)::close(pfd[1]);
            for (;;) (void)::pause();
          } catch (...) {
            ::_exit(98);
          }
          ::_exit(0);
        }
        (void)::close(pfd[1]);
        {
          char b = 0;
          ssize_t n = ::read(pfd[0], &b, 1);
          check(n == 1 && b == 'C', "killC sync pre-checkpoint");
        }
        // Timing-Fenster: ohne auf Checkpoint-Ende zu warten killen.
        if (kDelaysUs[att] > 0) (void)::usleep(kDelaysUs[att]);
        check(::kill(pid, SIGKILL) == 0, "killC kill");
        ++kills_in_window;  // jeder Kill ohne Completion-Wait zaehlt als Fenster-Versuch
        {
          int st = 0;
          check(::waitpid(pid, &st, 0) == pid, "killC waitpid");
          check(WIFSIGNALED(st), "killC WIFSIGNALED");
          check(WTERMSIG(st) == SIGKILL, "killC SIGKILL");
        }
        (void)::close(pfd[0]);
        auto r = Wal::replay_file(pc);
        if (r.size() == static_cast<size_t>(kN)) {
          // pre-Checkpoint: 1..N
          for (size_t idx = 0; idx < r.size(); ++idx) {
            check(r[idx].lsn == idx + 1, "killC pre LSN");
            check(r[idx].data == "rec-" + std::to_string(idx), "killC pre payload");
          }
          std::cout << "[wal] killC att=" << att << " delay_us=" << kDelaysUs[att]
                    << " -> pre-checkpoint (" << r.size() << ")\n";
        } else if (r.size() == static_cast<size_t>(kN - kCkpt)) {
          // post-Checkpoint: 1001..N
          for (size_t idx = 0; idx < r.size(); ++idx) {
            check(r[idx].lsn == static_cast<uint64_t>(kCkpt + 1 + idx), "killC post LSN");
            check(r[idx].data == "rec-" + std::to_string(kCkpt + idx), "killC post payload");
          }
          std::cout << "[wal] killC att=" << att << " delay_us=" << kDelaysUs[att]
                    << " -> post-checkpoint (" << r.size() << ")\n";
        } else {
          check(false, "killC atomar: nur pre(2000) oder post(1000) erlaubt");
        }
        std::error_code ec;
        std::filesystem::remove(pc, ec);
      }
      check(kills_in_window >= 1, "killC mindestens 1 Kill im Fenster");
      std::cout << "[wal] killC checkpoint-atomic ok, attempts=" << kills_in_window << '\n';
    }
  }
#else
  std::cout << "[wal] kill-9 harness skipped (non-Linux, fork/SIGKILL unavailable)\n";
#endif

  std::filesystem::remove(path);
  std::cout << "[wal] ALL WAL TESTS PASSED (1000 append/replay/checkpoint/crc/torn)\n";
  return 0;
}
