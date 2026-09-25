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

  std::filesystem::remove(path);
  std::cout << "[wal] ALL WAL TESTS PASSED (1000 append/replay/checkpoint/crc/torn)\n";
  return 0;
}
