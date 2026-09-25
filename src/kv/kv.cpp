#include "dbengine/kv.h"

#include <limits>

namespace dbengine::kv {
namespace {

// Falls s03-wal existiert, kann es diesen Hook-Typ direkt wiederverwenden:
//   #include "dbengine/storage/wal.h"
// V1 bleibt bewusst entkoppelt (No-Op default), damit kv ohne WAL baut.

bool StartsWith(const std::string& s, const std::string& prefix) noexcept {
  if (prefix.size() > s.size()) return false;
  return s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

bool KVStore::HasPrefix(const std::string& key,
                        const std::string& prefix) noexcept {
  return StartsWith(key, prefix);
}

std::vector<std::pair<std::string, std::string>> Snapshot::Scan(
    const std::string& prefix, std::size_t limit) const {
  std::vector<std::pair<std::string, std::string>> out;
  if (limit == 0) return out;
  auto it = prefix.empty() ? data_.begin() : data_.lower_bound(prefix);
  for (; it != data_.end(); ++it) {
    if (!prefix.empty() && !StartsWith(it->first, prefix)) break;
    out.emplace_back(it->first, it->second);
    if (out.size() >= limit) break;
  }
  return out;
}

std::optional<std::string> KVStore::Get(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = map_.find(key);
  if (it == map_.end()) return std::nullopt;
  return it->second;
}

void KVStore::Put(std::string key, std::string value) {
  if (!IsValidKey(key)) return;  // Einzel-Put mit leerem Key: still ignoriert
  Op op = Op::PutOp(key, value);
  std::uint64_t seq = 0;
  WalHook hook;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[std::move(key)] = std::move(value);
    seq = ++seq_;
    hook = wal_hook_;  // Kopie unter Lock, Aufruf ausserhalb (kein Deadlock)
  }
  if (hook) hook(op, seq);
}

bool KVStore::Delete(const std::string& key) {
  if (!IsValidKey(key)) return false;
  Op op = Op::DeleteOp(key);
  std::uint64_t seq = 0;
  WalHook hook;
  bool erased = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    erased = map_.erase(key) > 0;
    seq = ++seq_;
    hook = wal_hook_;
  }
  if (hook) hook(op, seq);
  return erased;
}

std::size_t KVStore::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return map_.size();
}

bool KVStore::Empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return map_.empty();
}

std::vector<std::pair<std::string, std::string>> KVStore::Scan(
    const std::string& prefix, std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<std::string, std::string>> out;
  if (limit == 0) return out;
  auto it = prefix.empty() ? map_.begin() : map_.lower_bound(prefix);
  for (; it != map_.end(); ++it) {
    if (!prefix.empty() && !HasPrefix(it->first, prefix)) break;
    out.emplace_back(it->first, it->second);
    if (out.size() >= limit) break;
  }
  return out;
}

bool KVStore::Write(const std::vector<Op>& ops) {
  // 1) Validieren (ausserhalb des Locks, keine Seiteneffekte).
  for (const auto& op : ops) {
    if (!IsValidKey(op.key)) return false;
    if (op.type == Op::Type::Put) {
      // value darf beliebig sein (auch leer) — nur Key muss gueltig sein.
    }
  }
  if (ops.empty()) return true;  // leerer Batch: erfolgreiches No-Op
  // 2) Atomar anwenden unter genau einem Lock.
  std::uint64_t base_seq = 0;
  WalHook hook;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& op : ops) {
      if (op.type == Op::Type::Put) {
        map_[op.key] = op.value;
      } else {
        map_.erase(op.key);
      }
    }
    base_seq = seq_ + 1;
    seq_ += ops.size();
    hook = wal_hook_;
  }
  // 3) WAL-Hook nach Commit (falls gesetzt; default No-Op).
  if (hook) {
    for (std::size_t i = 0; i < ops.size(); ++i) hook(ops[i], base_seq + i);
  }
  return true;
}

std::shared_ptr<Snapshot> KVStore::GetSnapshot() const {
  auto snap = std::make_shared<Snapshot>();
  std::lock_guard<std::mutex> lock(mutex_);
  snap->data_ = map_;  // Map-Copy (MVCC-Stub)
  snap->seq_ = seq_;
  return snap;
}

std::unique_ptr<KVStore::Iterator> KVStore::NewIterator(
    const std::string& prefix) const {
  auto snap = GetSnapshot();
  return NewIterator(*snap, prefix);
}

std::unique_ptr<KVStore::Iterator> KVStore::NewIterator(
    const Snapshot& snap, const std::string& prefix) const {
  auto rows =
      std::make_shared<std::vector<std::pair<std::string, std::string>>>(
          snap.Scan(prefix));
  return std::unique_ptr<Iterator>(new Iterator(std::move(rows)));
}

void KVStore::Iterator::Seek(const std::string& target) {
  std::size_t lo = 0, hi = data_->size();
  while (lo < hi) {
    std::size_t mid = lo + (hi - lo) / 2;
    if ((*data_)[mid].first < target) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  idx_ = lo;
}

void KVStore::SetWalHook(WalHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  wal_hook_ = std::move(hook);
}

std::uint64_t KVStore::Sequence() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return seq_;
}

}  // namespace dbengine::kv
