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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dbengine::storage {

// 16 KiB pages (F2.1: Row-Store, CoW B+Tree, Zero-Copy mmap reads).
inline constexpr std::size_t kPageSize = 16384;
inline constexpr std::uint32_t kPagerFormatVersion = 1;
inline constexpr std::size_t kDefaultCacheCapacity = 16;
inline constexpr std::size_t kMaxValueBytes = 1u << 20;  // 1 MiB per value (MVP cap)

using PageId = std::uint32_t;
using PageBuffer = std::array<std::uint8_t, kPageSize>;

// Single-file pager with:
//  - open/read/write page primitives on one backing file,
//  - mmap fast path for reads (pread fallback when mmap unavailable),
//  - small LRU page cache for raw pages + zero-copy views,
//  - plump clustered B+Tree hull: sorted std::map serialized across
//    data pages (page 0 = header, pages 1..N = record stream).
//
// File layout (little-endian):
//   Page 0 (header, 16 KiB, first 24 bytes used):
//     magic[8] = "DBENPG01", version u32, page_count u32, entry_count u32
//   Pages 1..N: concatenated records, each
//     key u64 (8B) | value_len u32 (4B) | value bytes
//   Records are stored in key-sorted (clustered) order.
//
// Zero-copy approach (documented):
//   - On open/flush the whole file is mmapped read-only (MAP_SHARED).
//   - read_zero_copy(page) returns a span directly into the mapping, i.e.
//     no memcpy, no userspace cache copy on the read path.
//   - If mmap is unavailable (e.g. unsupported FS), the call falls back to
//     the LRU cache copy. Callers can probe uses_mmap().
//   - Returned spans are valid until the next mutating call
//     (insert/erase/write_page/allocate_page/flush/close); remap may move
//     the base pointer, so do not retain them across writes.
//
// Concurrency: all public methods are mutex-guarded. Not fork-safe.
class Pager {
 public:
  explicit Pager(std::string path, std::size_t cache_capacity = kDefaultCacheCapacity);
  ~Pager();

  Pager(const Pager&) = delete;
  Pager& operator=(const Pager&) = delete;
  Pager(Pager&&) noexcept = default;
  Pager& operator=(Pager&&) noexcept = default;

  // Open backing file (creates + formats if missing/empty). Returns false
  // on I/O error or magic/version mismatch.
  [[nodiscard]] bool open();
  void close();
  [[nodiscard]] bool is_open() const;
  // True when the mmap read path is active. False => pread fallback.
  [[nodiscard]] bool uses_mmap() const;

  // --- Raw page primitives -------------------------------------------
  [[nodiscard]] PageId page_count() const;
  // Read page into `out` (via cache; source is mmap when available).
  [[nodiscard]] bool read_page(PageId id, std::span<std::uint8_t, kPageSize> out);
  // Stage a full-page write in the LRU cache (durable after flush()).
  [[nodiscard]] bool write_page(PageId id, std::span<const std::uint8_t, kPageSize> data);
  // Append a zeroed page, return its id. Durable after flush().
  [[nodiscard]] PageId allocate_page();
  // Zero-copy read: span into mmap (or LRU entry on fallback).
  [[nodiscard]] std::span<const std::uint8_t> read_zero_copy(PageId id);
  // Persist staged pages + KV image, fsync, refresh mmap.
  [[nodiscard]] bool flush();

  // --- Clustered B+Tree hull (sorted map backed by pages) -------------
  [[nodiscard]] bool insert(std::uint64_t key, std::string_view value);
  [[nodiscard]] bool insert(std::uint64_t key, std::span<const std::uint8_t> value);
  // Returns false when key is missing.
  [[nodiscard]] bool find(std::uint64_t key, std::string& out) const;
  [[nodiscard]] bool find(std::uint64_t key, std::vector<std::uint8_t>& out) const;
  [[nodiscard]] bool erase(std::uint64_t key);
  [[nodiscard]] std::size_t entry_count() const;
  [[nodiscard]] bool contains(std::uint64_t key) const;

  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  struct CachedPage {
    PageBuffer data{};
    bool dirty = false;
  };

  // Strict loader, caller holds mutex_.
  // Validates page-0 magic/version and the record stream (pages 1..N).
  // Returns true only for a fully valid image (trailing bytes must be
  // zero padding). Returns false on corrupt/torn input (bad magic or
  // version, header page_count exceeding the file, truncated record
  // header/payload, impossible value length, trailing garbage, or I/O
  // error). On false, entries_ is left empty: never silently exposes
  // partial data; callers (open) treat false as open failure.
  bool load_image();
  // Atomically persists the KV image (crash-safe):
  // serializes into path+".tmp", fsyncs the tmp file, renames it over
  // path_, then dir-fsyncs the parent directory (pattern from wal.cpp).
  // After a crash the file is either the old or the new complete image,
  // never a torn mix. Returns false on I/O error (old image untouched
  // until a successful rename).
  bool store_image();
  bool ensure_mmap();
  void drop_mmap();
  bool file_size(std::int64_t& out) const;
  bool read_exact(std::int64_t offset, void* buf, std::size_t n) const;
  bool write_exact(std::int64_t offset, const void* buf, std::size_t n);

  void cache_put(PageId id, const PageBuffer& data, bool dirty);
  bool cache_get(PageId id, PageBuffer& out);
  bool evict_if_needed();
  bool flush_raw_pages();

  std::string path_;
  std::size_t cache_capacity_;
  int fd_ = -1;
  bool open_ = false;
  PageId page_count_ = 0;

  // mmap read path.
  void* mmap_base_ = nullptr;
  std::size_t mmap_len_ = 0;
  bool use_mmap_ = false;

  // Clustered index image (sorted => "clustered" order on pages).
  std::map<std::uint64_t, std::vector<std::uint8_t>> entries_;
  bool image_dirty_ = false;

  // Raw-page LRU (front = MRU).
  mutable std::list<PageId> lru_;
  mutable std::unordered_map<PageId, std::pair<typename std::list<PageId>::iterator, CachedPage>> cache_;

  mutable std::mutex mutex_;
};

}  // namespace dbengine::storage
