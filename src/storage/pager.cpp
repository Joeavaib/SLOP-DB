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

#include "dbengine/storage/pager.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace dbengine::storage {
namespace {

constexpr char kMagic[8] = {'D', 'B', 'E', 'N', 'P', 'G', '0', '1'};
constexpr std::size_t kHeaderSize = 24;  // magic(8) + version(4) + page_count(4) + entries(4) + reserved(4)

void encode_u32_le(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

void encode_u64_le(std::uint8_t* p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

std::uint32_t decode_u32_le(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t decode_u64_le(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  return v;
}

bool is_all_zero(const std::uint8_t* p, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    if (p[i] != 0) return false;
  }
  return true;
}

bool fsync_fd(int fd) {
#ifdef __linux__
  if (::fdatasync(fd) == 0) return true;
#endif
  return ::fsync(fd) == 0;
}

void fsync_dir_of(const std::string& path) {
  // After rename(): dir-fsync so the rename itself is Kill -9 safe.
  auto slash = path.find_last_of('/');
  std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
  if (dir.empty()) dir = ".";
  int dfd = ::open(dir.c_str(), O_RDONLY
#ifdef O_DIRECTORY
                                     | O_DIRECTORY
#endif
  );
  if (dfd < 0) return;  // best effort
  (void)::fsync(dfd);
  ::close(dfd);
}

bool pwrite_full(int fd, std::int64_t offset, const void* buf, std::size_t n) {
  const auto* p = static_cast<const std::uint8_t*>(buf);
  std::size_t done = 0;
  while (done < n) {
    ssize_t w =
        ::pwrite(fd, p + done, n - done, static_cast<off_t>(offset + static_cast<std::int64_t>(done)));
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    done += static_cast<std::size_t>(w);
  }
  return true;
}

}  // namespace

Pager::Pager(std::string path, std::size_t cache_capacity)
    : path_(std::move(path)), cache_capacity_(cache_capacity == 0 ? 1 : cache_capacity) {}

Pager::~Pager() { close(); }

bool Pager::open() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (open_) return true;

  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) return false;

  struct stat st {};
  if (::fstat(fd_, &st) != 0) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  if (st.st_size == 0) {
    // Format fresh single-file image: exactly one header page.
    page_count_ = 1;
    PageBuffer hdr{};
    std::memcpy(hdr.data(), kMagic, 8);
    encode_u32_le(hdr.data() + 8, kPagerFormatVersion);
    encode_u32_le(hdr.data() + 12, page_count_);
    encode_u32_le(hdr.data() + 16, 0);
    encode_u32_le(hdr.data() + 20, 0);
    if (::ftruncate(fd_, static_cast<off_t>(kPageSize)) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    if (!write_exact(0, hdr.data(), kPageSize)) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    ::fsync(fd_);
    entries_.clear();
    image_dirty_ = false;
    cache_.clear();
    lru_.clear();
    open_ = true;
    ensure_mmap();
    return true;
  }

  if (st.st_size % static_cast<off_t>(kPageSize) != 0 || st.st_size < static_cast<off_t>(kPageSize)) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  page_count_ = static_cast<PageId>(st.st_size / static_cast<off_t>(kPageSize));
  cache_.clear();
  lru_.clear();
  open_ = true;

  // Validate header + load KV image while holding the lock.
  PageBuffer hdr{};
  if (!read_exact(0, hdr.data(), kPageSize)) {
    open_ = false;
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  if (std::memcmp(hdr.data(), kMagic, 8) != 0 || decode_u32_le(hdr.data() + 8) != kPagerFormatVersion) {
    open_ = false;
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  const std::uint32_t header_pages = decode_u32_le(hdr.data() + 12);
  if (header_pages != 0 && header_pages != page_count_) {
    // Tolerate trailing preallocated pages: trust actual file size.
    page_count_ = static_cast<PageId>(st.st_size / static_cast<off_t>(kPageSize));
  }
  if (!load_image()) {
    open_ = false;
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  ensure_mmap();
  return true;
}

void Pager::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return;
  // Best-effort persist; ignore errors on destructor path.
  store_image();
  flush_raw_pages();
  drop_mmap();
  ::close(fd_);
  fd_ = -1;
  open_ = false;
}

bool Pager::is_open() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return open_;
}

bool Pager::uses_mmap() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return use_mmap_;
}

PageId Pager::page_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return page_count_;
}

bool Pager::read_page(PageId id, std::span<std::uint8_t, kPageSize> out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_ || id >= page_count_) return false;
  PageBuffer buf{};
  if (cache_get(id, buf)) {
    std::memcpy(out.data(), buf.data(), kPageSize);
    return true;
  }
  if (use_mmap_ && mmap_base_ != nullptr) {
    const auto* base = static_cast<const std::uint8_t*>(mmap_base_);
    std::memcpy(out.data(), base + static_cast<std::size_t>(id) * kPageSize, kPageSize);
  } else {
    if (!read_exact(static_cast<std::int64_t>(id) * static_cast<std::int64_t>(kPageSize), buf.data(),
                    kPageSize)) {
      return false;
    }
    std::memcpy(out.data(), buf.data(), kPageSize);
  }
  cache_put(id, buf, false);
  // cache_put stored a copy; serve from caller's copy already done.
  // Re-fetch to promote LRU consistently (cache_get above missed).
  return true;
}

bool Pager::write_page(PageId id, std::span<const std::uint8_t, kPageSize> data) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_ || id >= page_count_) return false;
  PageBuffer buf{};
  std::memcpy(buf.data(), data.data(), kPageSize);
  // Staged write invalidates mmap view lazily on flush; drop now so
  // concurrent zero-copy readers never see torn old+mmap mix.
  drop_mmap();
  image_dirty_ = true;  // raw + KV share page_count; reconcile on flush
  cache_put(id, buf, true);
  return true;
}

PageId Pager::allocate_page() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return 0;
  const PageId id = page_count_++;
  PageBuffer zero{};
  zero.fill(0);
  drop_mmap();
  image_dirty_ = true;
  cache_put(id, zero, true);
  return id;
}

std::span<const std::uint8_t> Pager::read_zero_copy(PageId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_ || id >= page_count_) return {};
  if (use_mmap_ && mmap_base_ != nullptr) {
    const auto* base = static_cast<const std::uint8_t*>(mmap_base_);
    return {base + static_cast<std::size_t>(id) * kPageSize, kPageSize};
  }
  // Fallback: serve from LRU entry (single copy from pread at most).
  PageBuffer buf{};
  if (cache_get(id, buf)) {
    auto it = cache_.find(id);
    return {it->second.second.data.data(), kPageSize};
  }
  if (!read_exact(static_cast<std::int64_t>(id) * static_cast<std::int64_t>(kPageSize), buf.data(),
                  kPageSize)) {
    return {};
  }
  cache_put(id, buf, false);
  auto it = cache_.find(id);
  if (it == cache_.end()) return {};
  return {it->second.second.data.data(), kPageSize};
}

bool Pager::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return false;
  if (!store_image()) return false;
  if (!flush_raw_pages()) return false;
  ::fsync(fd_);
  ensure_mmap();
  return true;
}

bool Pager::insert(std::uint64_t key, std::string_view value) {
  return insert(key, std::span<const std::uint8_t>(
                         reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

bool Pager::insert(std::uint64_t key, std::span<const std::uint8_t> value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_ || value.size() > kMaxValueBytes) return false;
  entries_[key] = std::vector<std::uint8_t>(value.begin(), value.end());
  image_dirty_ = true;
  // Write-through for MVP durability: every insert is flushed so a
  // restart (or Kill -9 after return) sees it. Later steps (WAL) will
  // replace this with group-commit.
  return store_image() && flush_raw_pages() && ([this] { ::fsync(fd_); return ensure_mmap(); }());
}

bool Pager::insert_batch(
    const std::vector<std::pair<std::uint64_t, std::string>>& kvs) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return false;
  // s110: All-or-Nothing — erst VOLL validieren, dann mutieren. Vorher blieb
  // bei Oversize mitten im Batch eine Teilmutation in entries_ zurueck.
  for (const auto& [key, value] : kvs) {
    (void)key;
    if (value.size() > kMaxValueBytes) return false;
  }
  for (const auto& [key, value] : kvs) {
    entries_[key] =
        std::vector<std::uint8_t>(value.begin(), value.end());
  }
  image_dirty_ = true;
  // Ein Image-Rewrite + fsync fuer den ganzen Batch (statt einem pro Key).
  return store_image() && flush_raw_pages() && ([this] { ::fsync(fd_); return ensure_mmap(); }());
}

bool Pager::find(std::uint64_t key, std::string& out) const {
  std::vector<std::uint8_t> raw;
  if (!find(key, raw)) return false;
  out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
  return true;
}

bool Pager::find(std::uint64_t key, std::vector<std::uint8_t>& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return false;
  auto it = entries_.find(key);
  if (it == entries_.end()) return false;
  out = it->second;
  return true;
}

bool Pager::erase(std::uint64_t key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return false;
  auto it = entries_.find(key);
  if (it == entries_.end()) return false;
  entries_.erase(it);
  image_dirty_ = true;
  return store_image() && flush_raw_pages() && ([this] { ::fsync(fd_); return ensure_mmap(); }());
}

std::size_t Pager::entry_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

bool Pager::contains(std::uint64_t key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return open_ && entries_.find(key) != entries_.end();
}

// --- internals (caller holds mutex_) ---------------------------------

bool Pager::load_image() {
  entries_.clear();
  image_dirty_ = false;
  if (page_count_ < 1) return false;
  // Validate header (magic/version/page_count); open() checked these too,
  // but load_image must reject corrupt images on its own.
  PageBuffer hdr{};
  if (!read_exact(0, hdr.data(), kPageSize)) return false;
  if (std::memcmp(hdr.data(), kMagic, 8) != 0) return false;
  if (decode_u32_le(hdr.data() + 8) != kPagerFormatVersion) return false;
  const std::uint32_t header_pages = decode_u32_le(hdr.data() + 12);
  if (header_pages != 0 && header_pages > page_count_) {
    return false;  // file shorter than header claims -> torn/truncated
  }
  // s113: exakte Record-Zahl aus dem Header (Offset 16, seit Welle 1 bei
  // jedem store_image geschrieben) statt (0,0)-Sentinel. Der Sentinel machte
  // den gueltigen Datensatz (key=0, value="") als LETZTEN Record vom
  // Zero-Padding ununterscheidbar -> stiller Restart-Datenverlust.
  const std::uint32_t want_records = decode_u32_le(hdr.data() + 16);
  // s117: Inkrementelles Parsen pro Page mit Carry statt Riesen-Stream.
  // Vorher lag die komplette Datei (N×16 KiB) ZUSAETZLICH zu entries_ im
  // Speicher (Datei/mmap + Stream + entries_ + Value-Kopien). Jetzt: max
  // 1 Page + 1 Record Carry (<= kPageSize + kMaxValueBytes + 12) + entries_.
  // Fehlerfaelle exakt wie vorher (Count-Modell s113): zu wenige Records,
  // korrupte Laengen, torn Payload, non-zero Rest nach allen Records.
  std::vector<std::uint8_t> buf;
  buf.reserve(static_cast<std::size_t>(kPageSize) + kMaxValueBytes + 12);
  std::size_t head = 0;  // Parse-Offset in buf (kompaktiert ab 1 Page)
  std::uint64_t got_records = 0;
  PageBuffer page{};
  auto compact = [&]() {
    if (head > kPageSize) {
      buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(head));
      head = 0;
    }
  };
  // Parst alle VOLLSTAENDIGEN Records in buf[head..]. last=true: strikte
  // Endpruefung (keine weiteren Pages). Rueckgabe false = korrupt.
  auto parse_avail = [&](bool last) -> bool {
    for (;;) {
      if (got_records >= want_records) break;
      const std::size_t avail = buf.size() - head;
      if (avail < 12) {
        if (last) {
          if (avail == 0) break;  // exakt aufgebraucht
          entries_.clear();
          return false;  // truncated header vor Count
        }
        break;  // Carry: naechste Page abwarten
      }
      const std::uint64_t key = decode_u64_le(buf.data() + head);
      const std::uint32_t len = decode_u32_le(buf.data() + head + 8);
      if (len > kMaxValueBytes) {
        entries_.clear();
        return false;  // korrupt (Padding nur NACH allen Records legal)
      }
      if (avail < 12 + len) {
        if (last) {
          entries_.clear();
          return false;  // torn payload: Record laenger als Datei-Rest
        }
        break;  // Carry
      }
      entries_[key] = std::vector<std::uint8_t>(
          buf.begin() + static_cast<std::ptrdiff_t>(head + 12),
          buf.begin() + static_cast<std::ptrdiff_t>(head + 12 + len));
      head += 12 + len;
      ++got_records;
    }
    return true;
  };
  for (PageId p = 1; p < page_count_; ++p) {
    if (!read_exact(static_cast<std::int64_t>(p) * static_cast<std::int64_t>(kPageSize), page.data(),
                    kPageSize)) {
      entries_.clear();
      return false;
    }
    buf.insert(buf.end(), page.begin(), page.end());
    if (!parse_avail(false)) return false;
    compact();
  }
  if (!parse_avail(true)) return false;
  if (got_records != want_records) {
    entries_.clear();
    return false;  // fewer records than header count -> torn/truncated
  }
  // Nach allen Records darf nur noch Zero-Padding folgen.
  if (head != buf.size() && !is_all_zero(buf.data() + head, buf.size() - head)) {
    entries_.clear();
    return false;  // trailing garbage after last record -> corrupt, not EOF
  }
  image_dirty_ = false;
  return true;
}

bool Pager::store_image() {
  // Atomic persist (Muster wal.cpp checkpoint): full new image into
  // path_+".tmp", fsync, rename, dir-fsync, then fd_ reopen. A crash
  // leaves the old or the new complete image, never a torn mix.
  // insert/erase/flush call this followed by flush_raw_pages()+fsync(),
  // so staged raw tail pages are written right after the rename.
  auto drop_cached = [this](PageId id) {
    cache_.erase(id);
    for (auto it = lru_.begin(); it != lru_.end();) {
      if (*it == id)
        it = lru_.erase(it);
      else
        ++it;
    }
  };

  if (!image_dirty_) {
    // Reconcile header page_count with raw allocations. Fast path: header
    // already current -> nothing to do (no observable rewrite).
    PageBuffer hdr{};
    if (!read_exact(0, hdr.data(), kPageSize)) return false;
    if (std::memcmp(hdr.data(), kMagic, 8) != 0) return false;
    const std::uint32_t hp = decode_u32_le(hdr.data() + 12);
    const std::uint32_t he = decode_u32_le(hdr.data() + 16);
    if (hp == page_count_ && he == static_cast<std::uint32_t>(entries_.size())) return true;
    encode_u32_le(hdr.data() + 12, page_count_);
    encode_u32_le(hdr.data() + 16, static_cast<std::uint32_t>(entries_.size()));

    // Copy current file verbatim (patched header page 0) through tmp.
    const std::string tmp = path_ + ".tmp";
    int tfd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tfd < 0) return false;
    bool ok = pwrite_full(tfd, 0, hdr.data(), kPageSize);
    PageBuffer pg{};
    for (PageId p = 1; ok && p < page_count_; ++p) {
      if (!read_exact(static_cast<std::int64_t>(p) * static_cast<std::int64_t>(kPageSize), pg.data(),
                      kPageSize)) {
        ok = false;
        break;
      }
      ok = pwrite_full(tfd, static_cast<std::int64_t>(p) * static_cast<std::int64_t>(kPageSize),
                       pg.data(), kPageSize);
    }
    if (ok) ok = fsync_fd(tfd);
    ::close(tfd);
    if (!ok) {
      ::unlink(tmp.c_str());
      return false;
    }
    if (::rename(tmp.c_str(), path_.c_str()) != 0) {
      ::unlink(tmp.c_str());
      return false;
    }
    fsync_dir_of(path_);
    drop_mmap();
    ::close(fd_);
    fd_ = ::open(path_.c_str(), O_RDWR);
    if (fd_ < 0) return false;
    drop_cached(0);  // header is authoritative
    return true;
  }
  // Serialize sorted entries (std::map iteration is sorted => clustered).
  std::size_t need = 0;
  for (const auto& [k, v] : entries_) need += 12 + v.size();

  PageId data_pages = static_cast<PageId>((need + kPageSize - 1) / kPageSize);
  PageId total = data_pages + 1;  // + header
  if (total < page_count_) {
    // Keep raw-allocated tail pages (mixed use); never shrink below
    // current count, zero the freed region instead.
    total = page_count_;
  }

  std::vector<std::uint8_t> stream(static_cast<std::size_t>(data_pages) * kPageSize, 0);
  std::size_t off = 0;
  for (const auto& [k, v] : entries_) {
    encode_u64_le(stream.data() + off, k);
    encode_u32_le(stream.data() + off + 8, static_cast<std::uint32_t>(v.size()));
    off += 12;
    if (!v.empty()) {
      std::memcpy(stream.data() + off, v.data(), v.size());
      off += v.size();
    }
  }

  PageBuffer hdr{};
  std::memcpy(hdr.data(), kMagic, 8);
  encode_u32_le(hdr.data() + 8, kPagerFormatVersion);
  encode_u32_le(hdr.data() + 12, total);
  encode_u32_le(hdr.data() + 16, static_cast<std::uint32_t>(entries_.size()));
  encode_u32_le(hdr.data() + 20, 0);

  const std::string tmp = path_ + ".tmp";
  int tfd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (tfd < 0) return false;
  bool ok = pwrite_full(tfd, 0, hdr.data(), kPageSize);
  for (PageId p = 1; ok && p <= data_pages; ++p) {
    ok = pwrite_full(tfd, static_cast<std::int64_t>(p) * static_cast<std::int64_t>(kPageSize),
                     stream.data() + static_cast<std::size_t>(p - 1) * kPageSize, kPageSize);
  }
  if (ok && total > data_pages + 1) {
    // Zero tail pages beyond the image so a later load stops cleanly.
    // Dirty staged raw tail pages get zeros here; flush_raw_pages()
    // (caller, right after rename) writes their real content.
    PageBuffer zero{};
    zero.fill(0);
    for (PageId p = data_pages + 1; ok && p < total; ++p) {
      ok = pwrite_full(tfd, static_cast<std::int64_t>(p) * static_cast<std::int64_t>(kPageSize),
                       zero.data(), kPageSize);
    }
  }
  if (ok) ok = fsync_fd(tfd);
  ::close(tfd);
  if (!ok) {
    ::unlink(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), path_.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  fsync_dir_of(path_);
  // Reopen: old fd still points at the pre-rename inode.
  drop_mmap();
  ::close(fd_);
  fd_ = ::open(path_.c_str(), O_RDWR);
  if (fd_ < 0) return false;

  page_count_ = total;
  // Keep LRU coherent: staged raw pages in the data region are
  // superseded by the KV image; header is authoritative.
  for (PageId p = 0; p <= data_pages; ++p) drop_cached(p);
  image_dirty_ = false;
  return true;
}

bool Pager::ensure_mmap() {
  drop_mmap();
  const std::int64_t len = static_cast<std::int64_t>(page_count_) * static_cast<std::int64_t>(kPageSize);
  if (len <= 0) {
    use_mmap_ = false;
    return false;
  }
  void* base = ::mmap(nullptr, static_cast<std::size_t>(len), PROT_READ, MAP_SHARED, fd_, 0);
  if (base == MAP_FAILED) {
    mmap_base_ = nullptr;
    mmap_len_ = 0;
    use_mmap_ = false;
    return false;  // pread fallback stays active
  }
  mmap_base_ = base;
  mmap_len_ = static_cast<std::size_t>(len);
  use_mmap_ = true;
  return true;
}

void Pager::drop_mmap() {
  if (mmap_base_ != nullptr && mmap_base_ != MAP_FAILED) {
    ::munmap(mmap_base_, mmap_len_);
  }
  mmap_base_ = nullptr;
  mmap_len_ = 0;
  use_mmap_ = false;
}

bool Pager::file_size(std::int64_t& out) const {
  struct stat st {};
  if (::fstat(fd_, &st) != 0) return false;
  out = st.st_size;
  return true;
}

bool Pager::read_exact(std::int64_t offset, void* buf, std::size_t n) const {
  auto* p = static_cast<std::uint8_t*>(buf);
  std::size_t done = 0;
  while (done < n) {
    ssize_t r = ::pread(fd_, p + done, n - done, offset + static_cast<off_t>(done));
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;
    done += static_cast<std::size_t>(r);
  }
  return true;
}

bool Pager::write_exact(std::int64_t offset, const void* buf, std::size_t n) {
  const auto* p = static_cast<const std::uint8_t*>(buf);
  std::size_t done = 0;
  while (done < n) {
    ssize_t w = ::pwrite(fd_, p + done, n - done, offset + static_cast<off_t>(done));
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    done += static_cast<std::size_t>(w);
  }
  return true;
}

void Pager::cache_put(PageId id, const PageBuffer& data, bool dirty) {
  auto it = cache_.find(id);
  if (it != cache_.end()) {
    it->second.second.data = data;
    it->second.second.dirty = it->second.second.dirty || dirty;
    lru_.erase(it->second.first);
    lru_.push_front(id);
    it->second.first = lru_.begin();
    return;
  }
  evict_if_needed();
  lru_.push_front(id);
  CachedPage cp;
  cp.data = data;
  cp.dirty = dirty;
  cache_.emplace(id, std::make_pair(lru_.begin(), std::move(cp)));
}

bool Pager::cache_get(PageId id, PageBuffer& out) {
  auto it = cache_.find(id);
  if (it == cache_.end()) return false;
  out = it->second.second.data;
  lru_.erase(it->second.first);
  lru_.push_front(id);
  it->second.first = lru_.begin();
  return true;
}

bool Pager::evict_if_needed() {
  while (cache_.size() >= cache_capacity_ && !lru_.empty()) {
    const PageId victim = lru_.back();
    lru_.pop_back();
    auto it = cache_.find(victim);
    if (it == cache_.end()) continue;
    if (it->second.second.dirty) {
      if (!write_exact(static_cast<std::int64_t>(victim) * static_cast<std::int64_t>(kPageSize),
                       it->second.second.data.data(), kPageSize)) {
        return false;
      }
    }
    cache_.erase(it);
  }
  return true;
}

bool Pager::flush_raw_pages() {
  for (auto& [id, node] : cache_) {
    if (!node.second.dirty) continue;
    // Pages inside the KV image region were already reconciled by
    // store_image(); only tail/raw pages need write-back here.
    if (!write_exact(static_cast<std::int64_t>(id) * static_cast<std::int64_t>(kPageSize),
                     node.second.data.data(), kPageSize)) {
      return false;
    }
    node.second.dirty = false;
  }
  return true;
}

}  // namespace dbengine::storage
