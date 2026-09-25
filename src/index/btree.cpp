#include "dbengine/index/btree.h"

#include <chrono>
#include <string>

namespace dbengine::index {

SecondaryIndex::SecondaryIndex(uint64_t expireAfterSec)
    : expireAfterSec_(expireAfterSec) {}

void SecondaryIndex::setExpireAfterSec(uint64_t s) {
  std::lock_guard<std::mutex> g(mu_);
  expireAfterSec_ = s;
}

uint64_t SecondaryIndex::expireAfterSec() const {
  std::lock_guard<std::mutex> g(mu_);
  return expireAfterSec_;
}

int64_t SecondaryIndex::nowSec() const {
  if (nowOverrideSec_ >= 0) return nowOverrideSec_;
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch())
      .count();
}

bool SecondaryIndex::expired(int64_t createdSec, int64_t now) const {
  if (expireAfterSec_ == 0) return false;
  if (now < createdSec) return false;  // Clock-Skew: nicht ablaufen lassen
  return (now - createdSec) >= static_cast<int64_t>(expireAfterSec_);
}

void SecondaryIndex::insert(int64_t secondaryKey, int64_t primaryKey) {
  insert(secondaryKey, primaryKey, nowSec());
}

void SecondaryIndex::insert(int64_t secondaryKey, int64_t primaryKey,
                            int64_t nowSec) {
  std::lock_guard<std::mutex> g(mu_);
  idx_.emplace(secondaryKey, IndexEntry{primaryKey, nowSec});
}

bool SecondaryIndex::remove(int64_t secondaryKey, int64_t primaryKey) {
  std::lock_guard<std::mutex> g(mu_);
  auto range = idx_.equal_range(secondaryKey);
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second.primaryKey == primaryKey) {
      idx_.erase(it);  // nur ein Paar entfernen (Duplikate bleiben)
      return true;
    }
  }
  return false;
}

std::vector<int64_t> SecondaryIndex::lookup(int64_t secondaryKey) const {
  return lookup(secondaryKey, nowSec());
}

std::vector<int64_t> SecondaryIndex::lookup(int64_t secondaryKey,
                                            int64_t nowSec) const {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<int64_t> out;
  // KEIN Full-Scan: nur Treffer-Partition via equal_range (O(log n + k)).
  auto range = idx_.equal_range(secondaryKey);
  for (auto it = range.first; it != range.second; ++it) {
    if (!expired(it->second.createdSec, nowSec)) {
      out.push_back(it->second.primaryKey);
    }
  }
  return out;  // Lazy: abgelaufene werden herausgefiltert, nicht gesweept
}

std::vector<std::pair<int64_t, int64_t>> SecondaryIndex::rangeScan(
    int64_t low, int64_t high) const {
  return rangeScan(low, high, nowSec());
}

std::vector<std::pair<int64_t, int64_t>> SecondaryIndex::rangeScan(
    int64_t low, int64_t high, int64_t nowSec) const {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<std::pair<int64_t, int64_t>> out;
  if (high < low) return out;
  // KEIN Full-Scan: Fenster via lower_bound/upper_bound (O(log n + k)).
  auto itLow = idx_.lower_bound(low);
  auto itHigh = idx_.upper_bound(high);
  for (auto it = itLow; it != itHigh; ++it) {
    if (!expired(it->second.createdSec, nowSec)) {
      out.emplace_back(it->first, it->second.primaryKey);
    }
  }
  // idx_ ist geordnet -> out ist aufsteigend nach secondaryKey.
  return out;
}

size_t SecondaryIndex::size() const { return size(nowSec()); }

size_t SecondaryIndex::size(int64_t nowSec) const {
  std::lock_guard<std::mutex> g(mu_);
  if (expireAfterSec_ == 0) return idx_.size();
  size_t n = 0;
  for (const auto& [k, e] : idx_) {
    if (!expired(e.createdSec, nowSec)) ++n;
  }
  return n;
}

size_t SecondaryIndex::rawSize() const {
  std::lock_guard<std::mutex> g(mu_);
  return idx_.size();
}

bool SecondaryIndex::empty() const { return size() == 0; }

size_t SecondaryIndex::sweepExpired() { return sweepExpired(nowSec()); }

size_t SecondaryIndex::sweepExpired(int64_t nowSec) {
  std::lock_guard<std::mutex> g(mu_);
  if (expireAfterSec_ == 0) return 0;
  size_t removed = 0;
  for (auto it = idx_.begin(); it != idx_.end();) {
    if (expired(it->second.createdSec, nowSec)) {
      it = idx_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

void SecondaryIndex::clear() {
  std::lock_guard<std::mutex> g(mu_);
  idx_.clear();
}

void SecondaryIndex::setNowOverride(int64_t t) {
  std::lock_guard<std::mutex> g(mu_);
  nowOverrideSec_ = t;
}

void SecondaryIndex::clearNowOverride() {
  std::lock_guard<std::mutex> g(mu_);
  nowOverrideSec_ = -1;
}

// --- ART/GIN Stubs: bewusst nicht implementiert in V1 ---
void ArtIndexStub::insert(const std::string&, int64_t) {
  throw std::logic_error("ArtIndexStub: ART erst ab Phase 1 (s08+)");
}

std::vector<int64_t> ArtIndexStub::lookup(const std::string&) const {
  throw std::logic_error("ArtIndexStub: ART erst ab Phase 1 (s08+)");
}

void GinIndexStub::insert(int64_t, const std::vector<std::string>&) {
  throw std::logic_error("GinIndexStub: GIN erst mit Document/FTS (s09+)");
}

std::vector<int64_t> GinIndexStub::lookup(const std::string&) const {
  throw std::logic_error("GinIndexStub: GIN erst mit Document/FTS (s09+)");
}

}  // namespace dbengine::index
