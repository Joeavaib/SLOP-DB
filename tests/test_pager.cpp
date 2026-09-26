// Copyright 2026 dbengine contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Pager tests: write+read roundtrip + restart persistence (single-file).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include "dbengine/storage/pager.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (cond) {
    std::cout << "  ok: " << what << "\n";
  } else {
    std::cout << "  FAIL: " << what << "\n";
    ++failures;
  }
}

std::string tmp_path(const char* name) {
  auto p = std::filesystem::temp_directory_path() / name;
  std::remove(p.c_str());
  return p.string();
}

}  // namespace

int main() {
  using dbengine::storage::Pager;

  std::cout << "[pager] write+read roundtrip\n";
  {
    const std::string path = tmp_path("dbengine_pager_roundtrip.db");
    Pager pager(path, 4);  // tiny LRU to force evictions
    check(pager.open(), "open fresh file");

    // Roundtrip: 300 keys, mixed sizes incl. multi-page spill.
    for (std::uint64_t i = 0; i < 300; ++i) {
      std::string v = "value-" + std::to_string(i) + std::string(i % 17, 'x');
      if (!pager.insert(i, v)) {
        check(false, "insert roundtrip key");
        break;
      }
    }
    check(pager.entry_count() == 300, "entry count == 300");

    bool all_ok = true;
    for (std::uint64_t i = 0; i < 300; ++i) {
      std::string out;
      std::string want = "value-" + std::to_string(i) + std::string(i % 17, 'x');
      if (!pager.find(i, out) || out != want) {
        all_ok = false;
        break;
      }
    }
    check(all_ok, "read back all 300 keys");

    // Overwrite + erase.
    check(pager.insert(7, "seven-v2"), "overwrite key 7");
    std::string s;
    check(pager.find(7, s) && s == "seven-v2", "read overwritten key 7");
    check(pager.erase(8), "erase key 8");
    check(!pager.contains(8), "key 8 gone");
    check(pager.entry_count() == 299, "entry count == 299 after erase");

    // Zero-copy spot check: page 0 header magic via mmap (or fallback).
    auto span = pager.read_zero_copy(0);
    check(span.size() == dbengine::storage::kPageSize, "zero-copy page size 16KiB");
    check(span.size() >= 8 && span[0] == 'D' && span[1] == 'B', "zero-copy header magic");
    std::cout << "  info: uses_mmap=" << (pager.uses_mmap() ? "yes" : "no (pread fallback)") << "\n";

    // Raw page primitive smoke on tail page (separate pager file below
    // covers raw more; here just check invalid id fails).
    std::array<std::uint8_t, dbengine::storage::kPageSize> tmp{};
    check(!pager.read_page(1u << 30, tmp), "read_page invalid id fails");

    pager.close();
    std::remove(path.c_str());
  }

  std::cout << "[pager] restart persistence (single-file)\n";
  {
    const std::string path = tmp_path("dbengine_pager_restart.db");
    {
      Pager pager(path);
      check(pager.open(), "open (phase 1)");
      check(pager.insert(1, "one"), "insert 1");
      check(pager.insert(2, "two"), "insert 2");
      check(pager.insert(1000000, std::string(50000, 'a')), "insert 50KB value");
      check(pager.flush(), "flush");
      pager.close();
    }
    {
      // Reopen same single file: data must survive restart.
      Pager pager(path);
      check(pager.open(), "reopen (phase 2)");
      std::string a, b, c;
      check(pager.find(1, a) && a == "one", "persisted key 1");
      check(pager.find(2, b) && b == "two", "persisted key 2");
      check(pager.find(1000000, c) && c.size() == 50000, "persisted 50KB value");
      // Write after restart, then restart again.
      check(pager.insert(3, "three"), "insert after restart");
      pager.close();
    }
    {
      Pager pager(path);
      check(pager.open(), "reopen (phase 3)");
      std::string v;
      check(pager.find(3, v) && v == "three", "persisted post-restart write");
      check(pager.entry_count() == 4, "entry count == 4 after two restarts");
      pager.close();
    }
    std::remove(path.c_str());
  }

  std::cout << "[pager] raw page allocate/write/read\n";
  {
    const std::string path = tmp_path("dbengine_pager_raw.db");
    Pager pager(path, 2);
    check(pager.open(), "open raw file");
    auto id = pager.allocate_page();
    check(id >= 1, "allocate_page returns data page");
    std::array<std::uint8_t, dbengine::storage::kPageSize> w{};
    for (std::size_t i = 0; i < w.size(); ++i) w[i] = static_cast<std::uint8_t>(i & 0xff);
    check(pager.write_page(id, w), "write_page");
    check(pager.flush(), "flush raw");
    std::array<std::uint8_t, dbengine::storage::kPageSize> r{};
    check(pager.read_page(id, r) && r == w, "read_page roundtrip");
    pager.close();
    std::remove(path.c_str());
  }

  std::cout << "[pager] torn image detected (shorten + garbage)\n";
  {
    const std::string path = tmp_path("dbengine_pager_torn.db");
    {
      Pager pager(path);
      check(pager.open(), "open (torn setup)");
      check(pager.insert(1, "one"), "insert torn-1");
      check(pager.insert(2, "two"), "insert torn-2");
      for (std::uint64_t i = 3; i < 50; ++i) {
        if (!pager.insert(i, "value-" + std::to_string(i))) {
          check(false, "insert torn bulk");
          break;
        }
      }
      check(pager.flush(), "flush torn setup");
      pager.close();
    }
    {
      // Sanity: uncorrupted image reopens cleanly.
      Pager pager(path);
      check(pager.open(), "reopen clean before corruption");
      std::string v;
      check(pager.find(1, v) && v == "one", "clean image intact");
      pager.close();
    }
    {
      // Corrupt: shorten the data region, append garbage, pad back to a
      // page multiple so the size check passes and load_image() must judge.
      const std::size_t ps = dbengine::storage::kPageSize;
      int fd = ::open(path.c_str(), O_RDWR);
      check(fd >= 0, "open raw file for corruption");
      if (fd >= 0) {
        check(::ftruncate(fd, static_cast<off_t>(ps) + 5) == 0, "shorten data region");
        std::uint8_t garbage[64];
        std::memset(garbage, 0xAB, sizeof(garbage));
        ssize_t w = ::pwrite(fd, garbage, sizeof(garbage), static_cast<off_t>(ps) + 5);
        check(w == static_cast<ssize_t>(sizeof(garbage)), "append garbage");
        check(::ftruncate(fd, static_cast<off_t>(2 * ps)) == 0, "pad to page multiple");
        check(std::filesystem::file_size(path) % ps == 0, "corrupt size still page-aligned");
        ::fsync(fd);
        ::close(fd);
      }
    }
    {
      // Must report an error, not silently expose partial records.
      Pager pager(path);
      check(!pager.open(), "torn image: open fails instead of partial data");
      pager.close();
    }
    std::remove(path.c_str());
  }

  if (failures == 0) {
    std::cout << "PAGER TESTS PASSED\n";
    return 0;
  }
  std::cout << "PAGER TESTS FAILED (" << failures << ")\n";
  return 1;
}
