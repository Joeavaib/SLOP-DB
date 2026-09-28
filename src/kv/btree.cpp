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

#include "dbengine/kv/btree.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "dbengine/storage/pager.h"

namespace dbengine::kv {
namespace {

// Mindestgrad t: 32 => max. 63 Keys / 64 Kinder je Knoten.
constexpr std::size_t kT = 32;
constexpr std::size_t kMaxKeys = 2 * kT - 1;
constexpr std::size_t kMinKeys = kT - 1;

constexpr char kNodeMagic[8] = {'B', 'T', 'R', 'E', 'E', 'P', 'G', '1'};
constexpr char kSbMagic[8] = {'B', 'T', 'R', 'E', 'E', 'S', 'B', '1'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint64_t kSuperKey = 0;
constexpr std::uint64_t kFirstNodeId = 1;

void PutU32(std::string& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(v >> (8 * i))));
}

void PutU64(std::string& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(v >> (8 * i))));
}

void PutBytes(std::string& out, const std::string& s) {
  PutU32(out, static_cast<std::uint32_t>(s.size()));
  out.append(s);
}

bool GetU32(const std::string& in, std::size_t& pos, std::uint32_t& v) {
  if (pos + 4 > in.size()) return false;
  v = 0;
  for (int i = 0; i < 4; ++i)
    v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[pos + i]))
         << (8 * i);
  pos += 4;
  return true;
}

bool GetU64(const std::string& in, std::size_t& pos, std::uint64_t& v) {
  if (pos + 8 > in.size()) return false;
  v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[pos + i]))
         << (8 * i);
  pos += 8;
  return true;
}

bool GetBytes(const std::string& in, std::size_t& pos, std::string& s) {
  std::uint32_t n = 0;
  if (!GetU32(in, pos, n)) return false;
  if (pos + n > in.size()) return false;
  s.assign(in.data() + pos, n);
  pos += n;
  return true;
}

// FNV-1a 64 als Checksumme (Integritaet, nicht kryptografisch).
std::uint64_t Checksum(const std::string& s) {
  std::uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : s) {
    h ^= static_cast<std::uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

bool HasPrefix(const std::string& s, const std::string& prefix) noexcept {
  if (prefix.size() > s.size()) return false;
  return s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

BTreeKV::BTreeKV() = default;
BTreeKV::~BTreeKV() { Close(); }

// ---- Serialisierung --------------------------------------------------------

// Knoten-Layout: magic(8) ver(4) leaf(1) n(4) [klen+key]*n [vlen+val]*n
//   [child u64]*(leaf?0:n+1) checksum(8 ueber alles davor).
std::string BTreeKV::EncodeNode(const Node& node) {
  std::string out;
  out.append(kNodeMagic, 8);
  PutU32(out, kFormatVersion);
  out.push_back(node.leaf ? '\x01' : '\x00');
  PutU32(out, static_cast<std::uint32_t>(node.keys.size()));
  for (const auto& k : node.keys) PutBytes(out, k);
  for (const auto& v : node.vals) PutBytes(out, v);
  for (std::uint64_t c : node.childs) PutU64(out, c);
  PutU64(out, Checksum(out));
  return out;
}

bool BTreeKV::DecodeNode(const std::string& in, Node& node) {
  node = Node{};
  std::size_t pos = 0;
  if (in.size() < 8 + 4 + 1 + 4 + 8) return false;
  if (std::memcmp(in.data(), kNodeMagic, 8) != 0) return false;
  pos += 8;
  std::uint32_t ver = 0;
  if (!GetU32(in, pos, ver) || ver != kFormatVersion) return false;
  const bool leaf = (in[pos++] == '\x01');
  std::uint32_t n = 0;
  if (!GetU32(in, pos, n) || n > kMaxKeys) return false;
  node.leaf = leaf;
  node.keys.reserve(n);
  node.vals.reserve(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    std::string k;
    if (!GetBytes(in, pos, k)) return false;
    node.keys.push_back(std::move(k));
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    std::string v;
    if (!GetBytes(in, pos, v)) return false;
    node.vals.push_back(std::move(v));
  }
  if (!leaf) {
    node.childs.reserve(n + 1);
    for (std::uint32_t i = 0; i < n + 1; ++i) {
      std::uint64_t c = 0;
      if (!GetU64(in, pos, c)) return false;
      node.childs.push_back(c);
    }
  }
  std::uint64_t want = 0;
  if (!GetU64(in, pos, want) || pos != in.size()) return false;
  if (Checksum(in.substr(0, in.size() - 8)) != want) return false;
  if (node.vals.size() != node.keys.size()) return false;
  if (!leaf && node.childs.size() != node.keys.size() + 1) return false;
  for (std::size_t i = 1; i < node.keys.size(); ++i) {
    if (!(node.keys[i - 1] < node.keys[i])) return false;  // strikt sortiert
  }
  return true;
}

// Superblock-Layout: magic(8) ver(4) root(8) next(8) count(8) seq(8)
//   checksum(8). Freelist wird beim Open aus Erreichbarkeit rekonstruiert
//   (GC), daher nicht persistiert -> Commit klein, kein Freelist-Drift.
namespace {

struct Super {
  std::uint64_t root = 0;
  std::uint64_t next = kFirstNodeId + 1;
  std::uint64_t count = 0;
  std::uint64_t seq = 0;
};

std::string EncodeSuper(const Super& sb) {
  std::string out;
  out.append(kSbMagic, 8);
  PutU32(out, kFormatVersion);
  PutU64(out, sb.root);
  PutU64(out, sb.next);
  PutU64(out, sb.count);
  PutU64(out, sb.seq);
  PutU64(out, Checksum(out));
  return out;
}

bool DecodeSuper(const std::string& in, Super& sb) {
  sb = Super{};
  std::size_t pos = 0;
  if (in.size() != 8 + 4 + 8 * 4 + 8) return false;
  if (std::memcmp(in.data(), kSbMagic, 8) != 0) return false;
  pos += 8;
  std::uint32_t ver = 0;
  if (!GetU32(in, pos, ver) || ver != kFormatVersion) return false;
  if (!GetU64(in, pos, sb.root) || !GetU64(in, pos, sb.next) ||
      !GetU64(in, pos, sb.count) || !GetU64(in, pos, sb.seq))
    return false;
  std::uint64_t want = 0;
  if (!GetU64(in, pos, want) || pos != in.size()) return false;
  if (sb.root < kFirstNodeId || sb.root >= sb.next) return false;
  return Checksum(in.substr(0, in.size() - 8)) == want;
}

}  // namespace

// ---- Lebenszyklus ----------------------------------------------------------

bool BTreeKV::Open(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (open_) return false;
  auto pager = std::make_unique<storage::Pager>(path);
  if (!pager->open()) return false;
  pager_ = std::move(pager);
  path_ = path;
  if (!LoadAll()) {
    pager_->close();
    pager_.reset();
    return false;
  }
  open_ = true;
  return true;
}

bool BTreeKV::Flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Persist()) {
    // s111: wie Write — kein sichtbarer Teil-Commit nach I/O-Fehler.
    if (pager_) {
      pager_->close();
      if (!pager_->open() || !LoadAll()) {
        open_ = false;
      }
    } else {
      open_ = false;
    }
    return false;
  }
  return true;
}

void BTreeKV::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return;
  (void)Persist();  // best effort (Destruktor-Pfad)
  pager_->close();
  pager_.reset();
  open_ = false;
  nodes_.clear();
  dirty_.clear();
  free_.clear();
  root_ = 0;
  next_id_ = kFirstNodeId;
  count_ = 0;
  seq_ = 0;
}

bool BTreeKV::IsOpen() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return open_;
}

const std::string& BTreeKV::Path() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return path_;
}

// ---- Punktzugriffe ---------------------------------------------------------

std::optional<std::string> BTreeKV::Get(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_ || !IsValidKey(key)) return std::nullopt;
  std::uint64_t id = root_;
  for (;;) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return std::nullopt;  // dürfte nie passieren
    const Node& node = it->second;
    const auto pos = std::lower_bound(node.keys.begin(), node.keys.end(), key);
    const std::size_t idx =
        static_cast<std::size_t>(pos - node.keys.begin());
    if (pos != node.keys.end() && *pos == key) return node.vals[idx];
    if (node.leaf) return std::nullopt;
    id = node.childs[idx];
  }
}

bool BTreeKV::Put(std::string key, std::string value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_ || !IsValidEntry(key, value)) return false;
  if (nodes_[root_].keys.size() == kMaxKeys) {
    // Wurzel voll => neue Wurzel, alte als Kind 0, dann spalten.
    const std::uint64_t old = root_;
    root_ = AllocNode(false);
    nodes_[root_].childs.push_back(old);
    SplitChild(root_, 0);
  }
  InsertNonFull(root_, key, value);
  MarkDirty(root_);  // Wurzel immer dirty => Superblock-Commit garantiert
  return true;
}

bool BTreeKV::Delete(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_ || !IsValidKey(key)) return false;
  if (!Remove(root_, key)) return false;
  ShrinkRoot();
  MarkDirty(root_);
  return true;
}

std::size_t BTreeKV::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<std::size_t>(count_);
}

bool BTreeKV::Empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_ == 0;
}

// ---- Scans -----------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> BTreeKV::ScanRange(
    const std::string& from, const std::string& to,
    std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<std::string, std::string>> out;
  if (!open_ || limit == 0) return out;
  if (!to.empty() && to < from) return out;
  std::vector<std::pair<std::string, std::string>> all;
  all.reserve(static_cast<std::size_t>(count_));
  InOrder(root_, &all);
  for (auto& kv : all) {
    if (kv.first < from) continue;
    if (!to.empty() && !(kv.first < to)) break;  // sortiert => Abbruch
    out.push_back(std::move(kv));
    if (out.size() >= limit) break;
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> BTreeKV::Scan(
    const std::string& prefix, std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<std::string, std::string>> out;
  if (!open_ || limit == 0) return out;
  std::vector<std::pair<std::string, std::string>> all;
  all.reserve(static_cast<std::size_t>(count_));
  InOrder(root_, &all);
  auto it = prefix.empty()
                ? all.begin()
                : std::lower_bound(all.begin(), all.end(), prefix,
                                   [](const auto& kv, const std::string& p) {
                                     return kv.first < p;
                                   });
  for (; it != all.end(); ++it) {
    if (!prefix.empty() && !HasPrefix(it->first, prefix)) break;
    out.emplace_back(it->first, it->second);
    if (out.size() >= limit) break;
  }
  return out;
}

bool BTreeKV::Write(const std::vector<Op>& ops) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return false;
  // 1. Validierung zuerst: alles-oder-nichts.
  for (const auto& op : ops) {
    if (op.type == Op::Type::Put) {
      if (!IsValidEntry(op.key, op.value)) return false;
    } else {
      if (!IsValidKey(op.key)) return false;
    }
  }
  // 2. Anwenden (kann intern nicht mehr an Validierung scheitern).
  for (const auto& op : ops) {
    if (op.type == Op::Type::Put) {
      if (nodes_[root_].keys.size() == kMaxKeys) {
        const std::uint64_t old = root_;
        root_ = AllocNode(false);
        nodes_[root_].childs.push_back(old);
        SplitChild(root_, 0);
      }
      InsertNonFull(root_, op.key, op.value);
    } else {
      (void)Remove(root_, op.key);  // delete-missing idempotent
    }
  }
  if (ops.empty()) return true;
  ShrinkRoot();
  MarkDirty(root_);
  // 3. Commit: dauerhaft flushen.
  if (!Persist()) {
    // s111: Memory-vs-durable-Split heilen — nodes_ enthalten die neue
    // Version, der Superblock zeigt auf die alte. false muss "kein Commit"
    // bedeuten (weder sichtbar noch haltbar): Pager + Baum auf Datei-Stand
    // zuruecksetzen (nur Fehlerpfad, kostet im Normalfall nichts).
    // Scheitert der Reload (oder kein Pager vorhanden), ist das Handle
    // defekt (open_=false).
    if (pager_) {
      pager_->close();
      if (!pager_->open() || !LoadAll()) {
        open_ = false;
      }
    } else {
      open_ = false;
    }
    return false;
  }
  return true;
}

std::unique_ptr<BTreeKV::Iterator> BTreeKV::NewIterator(
    const std::string& prefix) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<std::string, std::string>> data;
  if (open_) {
    data.reserve(static_cast<std::size_t>(count_));
    InOrder(root_, &data);
    if (!prefix.empty()) {
      std::vector<std::pair<std::string, std::string>> filt;
      filt.reserve(data.size());
      for (auto& kv : data) {
        if (HasPrefix(kv.first, prefix)) filt.push_back(std::move(kv));
      }
      data = std::move(filt);
    }
  }
  return std::unique_ptr<Iterator>(new Iterator(std::move(data)));
}

void BTreeKV::Iterator::Seek(const std::string& target) {
  idx_ = static_cast<std::size_t>(
      std::lower_bound(data_.begin(), data_.end(), target,
                       [](const auto& kv, const std::string& t) {
                         return kv.first < t;
                       }) -
      data_.begin());
}

// ---- Diagnose --------------------------------------------------------------

std::size_t BTreeKV::Height() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_ || count_ == 0) return 0;
  std::size_t h = 0;
  std::uint64_t id = root_;
  for (;;) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return 0;  // korrupt (darf nie passieren)
    ++h;
    if (it->second.leaf) return h;
    if (it->second.childs.empty()) return 0;
    id = it->second.childs.front();
  }
}

std::size_t BTreeKV::NodeCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return nodes_.size();
}

std::uint64_t BTreeKV::Sequence() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return seq_;
}

// ---- B-Baum-Kern (CLRS) ----------------------------------------------------

std::uint64_t BTreeKV::AllocNode(bool leaf) {
  std::uint64_t id = 0;
  if (free_.empty()) {
    id = next_id_++;
  } else {
    id = free_.back();
    free_.pop_back();
  }
  Node node;
  node.leaf = leaf;
  nodes_[id] = std::move(node);
  MarkDirty(id);
  return id;
}

void BTreeKV::MarkDirty(std::uint64_t id) { dirty_[id] = 1; }

void BTreeKV::FreeNode(std::uint64_t id) {
  nodes_.erase(id);
  dirty_.erase(id);
  free_.push_back(id);
}

void BTreeKV::InsertNonFull(std::uint64_t id, const std::string& key,
                            const std::string& value) {
  Node& node = nodes_.at(id);
  auto pos = std::lower_bound(node.keys.begin(), node.keys.end(), key);
  const std::size_t idx = static_cast<std::size_t>(pos - node.keys.begin());
  if (node.leaf) {
    if (pos != node.keys.end() && *pos == key) {
      node.vals[idx] = value;  // Ueberschreiben, count unveraendert
    } else {
      node.keys.insert(pos, key);
      node.vals.insert(node.vals.begin() +
                           static_cast<std::ptrdiff_t>(idx),
                       value);
      ++count_;
    }
    MarkDirty(id);
    return;
  }
  if (pos != node.keys.end() && *pos == key) {
    node.vals[idx] = value;  // Treffer im inneren Knoten
    MarkDirty(id);
    return;
  }
  std::uint64_t child = node.childs[idx];
  if (nodes_.at(child).keys.size() == kMaxKeys) {
    SplitChild(id, idx);
    // Nach Split Referenzen neu holen (AllocNode kann rehashen).
    Node& parent = nodes_.at(id);
    if (key == parent.keys[idx]) {
      parent.vals[idx] = value;  // Median-Duplikat
      MarkDirty(id);
      return;
    }
    child = (parent.keys[idx] < key) ? parent.childs[idx + 1]
                                     : parent.childs[idx];
  }
  InsertNonFull(child, key, value);
}

void BTreeKV::SplitChild(std::uint64_t parent, std::size_t i) {
  const std::uint64_t yid = nodes_.at(parent).childs[i];
  const bool leaf = nodes_.at(yid).leaf;
  const std::uint64_t zid = AllocNode(leaf);
  // Erst alle Inhalte umkopieren, dann Referenzen anfassen.
  const std::string median_key = nodes_.at(yid).keys[kT - 1];
  const std::string median_val = nodes_.at(yid).vals[kT - 1];
  Node& z = nodes_.at(zid);
  Node& y = nodes_.at(yid);
  z.keys.assign(y.keys.begin() + static_cast<std::ptrdiff_t>(kT), y.keys.end());
  z.vals.assign(y.vals.begin() + static_cast<std::ptrdiff_t>(kT), y.vals.end());
  y.keys.resize(kT - 1);
  y.vals.resize(kT - 1);
  if (!leaf) {
    z.childs.assign(y.childs.begin() + static_cast<std::ptrdiff_t>(kT),
                    y.childs.end());
    y.childs.resize(kT);
  }
  Node& p = nodes_.at(parent);
  p.keys.insert(p.keys.begin() + static_cast<std::ptrdiff_t>(i), median_key);
  p.vals.insert(p.vals.begin() + static_cast<std::ptrdiff_t>(i), median_val);
  p.childs.insert(p.childs.begin() + static_cast<std::ptrdiff_t>(i + 1), zid);
  MarkDirty(parent);
  MarkDirty(yid);
  MarkDirty(zid);
}

bool BTreeKV::Remove(std::uint64_t id, const std::string& key) {
  Node& node = nodes_.at(id);
  auto pos = std::lower_bound(node.keys.begin(), node.keys.end(), key);
  const std::size_t idx = static_cast<std::size_t>(pos - node.keys.begin());
  const bool found =
      (pos != node.keys.end() && *pos == key);

  if (node.leaf) {
    if (!found) return false;
    node.keys.erase(node.keys.begin() + static_cast<std::ptrdiff_t>(idx));
    node.vals.erase(node.vals.begin() + static_cast<std::ptrdiff_t>(idx));
    --count_;
    MarkDirty(id);
    return true;
  }

  if (found) {
    const std::uint64_t left = node.childs[idx];
    const std::uint64_t right = node.childs[idx + 1];
    if (nodes_.at(left).keys.size() >= kT) {
      // Vorgaenger ersetzt Key, dann Vorgaenger unten loeschen.
      const auto pred = MaxOf(left);
      nodes_.at(id).keys[idx] = pred.first;
      nodes_.at(id).vals[idx] = pred.second;
      MarkDirty(id);
      return Remove(left, pred.first);
    }
    if (nodes_.at(right).keys.size() >= kT) {
      const auto succ = MinOf(right);
      nodes_.at(id).keys[idx] = succ.first;
      nodes_.at(id).vals[idx] = succ.second;
      MarkDirty(id);
      return Remove(right, succ.first);
    }
    MergeChild(id, idx);
    return Remove(nodes_.at(id).childs[idx], key);
  }

  // Abstieg in Kind idx; vorher auf t auffuellen.
  std::uint64_t child = node.childs[idx];
  if (nodes_.at(child).keys.size() == kMinKeys) {
    child = nodes_.at(id).childs[FillChild(id, idx)];
  }
  return Remove(child, key);
}

std::size_t BTreeKV::FillChild(std::uint64_t parent, std::size_t idx) {
  Node& p = nodes_.at(parent);
  const std::size_t n = p.keys.size();
  if (n == 0) return idx;  // nur via Invariantenbruch erreichbar (defensiv)
  if (idx > 0 && nodes_.at(p.childs[idx - 1]).keys.size() >= kT) {
    BorrowFromPrev(parent, idx);
    return idx;
  }
  if (idx < n && nodes_.at(p.childs[idx + 1]).keys.size() >= kT) {
    BorrowFromNext(parent, idx);
    return idx;
  }
  if (idx < n) {
    MergeChild(parent, idx);
    return idx;
  }
  MergeChild(parent, idx - 1);
  return idx - 1;
}

void BTreeKV::BorrowFromPrev(std::uint64_t parent, std::size_t idx) {
  const std::uint64_t cid = nodes_.at(parent).childs[idx];
  const std::uint64_t sid = nodes_.at(parent).childs[idx - 1];
  Node& child = nodes_.at(cid);
  Node& sib = nodes_.at(sid);
  Node& p = nodes_.at(parent);
  child.keys.insert(child.keys.begin(), p.keys[idx - 1]);
  child.vals.insert(child.vals.begin(), p.vals[idx - 1]);
  if (!child.leaf) {
    child.childs.insert(child.childs.begin(), sib.childs.back());
    sib.childs.pop_back();
  }
  p.keys[idx - 1] = sib.keys.back();
  p.vals[idx - 1] = sib.vals.back();
  sib.keys.pop_back();
  sib.vals.pop_back();
  MarkDirty(parent);
  MarkDirty(cid);
  MarkDirty(sid);
}

void BTreeKV::BorrowFromNext(std::uint64_t parent, std::size_t idx) {
  const std::uint64_t cid = nodes_.at(parent).childs[idx];
  const std::uint64_t sid = nodes_.at(parent).childs[idx + 1];
  Node& child = nodes_.at(cid);
  Node& sib = nodes_.at(sid);
  Node& p = nodes_.at(parent);
  child.keys.push_back(p.keys[idx]);
  child.vals.push_back(p.vals[idx]);
  if (!child.leaf) {
    child.childs.push_back(sib.childs.front());
    sib.childs.erase(sib.childs.begin());
  }
  p.keys[idx] = sib.keys.front();
  p.vals[idx] = sib.vals.front();
  sib.keys.erase(sib.keys.begin());
  sib.vals.erase(sib.vals.begin());
  MarkDirty(parent);
  MarkDirty(cid);
  MarkDirty(sid);
}

void BTreeKV::MergeChild(std::uint64_t parent, std::size_t idx) {
  const std::uint64_t lid = nodes_.at(parent).childs[idx];
  const std::uint64_t rid = nodes_.at(parent).childs[idx + 1];
  Node& left = nodes_.at(lid);
  Node& right = nodes_.at(rid);
  Node& p = nodes_.at(parent);
  left.keys.push_back(p.keys[idx]);
  left.vals.push_back(p.vals[idx]);
  left.keys.insert(left.keys.end(), right.keys.begin(), right.keys.end());
  left.vals.insert(left.vals.end(), right.vals.begin(), right.vals.end());
  if (!left.leaf) {
    left.childs.insert(left.childs.end(), right.childs.begin(),
                       right.childs.end());
  }
  p.keys.erase(p.keys.begin() + static_cast<std::ptrdiff_t>(idx));
  p.vals.erase(p.vals.begin() + static_cast<std::ptrdiff_t>(idx));
  p.childs.erase(p.childs.begin() + static_cast<std::ptrdiff_t>(idx + 1));
  FreeNode(rid);
  MarkDirty(parent);
  MarkDirty(lid);
}

void BTreeKV::ShrinkRoot() {
  while (!nodes_.at(root_).leaf && nodes_.at(root_).keys.empty()) {
    const std::uint64_t old = root_;
    root_ = nodes_.at(old).childs.front();
    FreeNode(old);
  }
}

std::pair<std::string, std::string> BTreeKV::MaxOf(std::uint64_t id) const {
  std::uint64_t cur = id;
  for (;;) {
    const Node& node = nodes_.at(cur);
    if (node.leaf) return {node.keys.back(), node.vals.back()};
    cur = node.childs.back();
  }
}

std::pair<std::string, std::string> BTreeKV::MinOf(std::uint64_t id) const {
  std::uint64_t cur = id;
  for (;;) {
    const Node& node = nodes_.at(cur);
    if (node.leaf) return {node.keys.front(), node.vals.front()};
    cur = node.childs.front();
  }
}

void BTreeKV::InOrder(
    std::uint64_t id,
    std::vector<std::pair<std::string, std::string>>* out) const {
  const Node& node = nodes_.at(id);
  if (node.leaf) {
    for (std::size_t i = 0; i < node.keys.size(); ++i)
      out->emplace_back(node.keys[i], node.vals[i]);
    return;
  }
  for (std::size_t i = 0; i < node.keys.size(); ++i) {
    InOrder(node.childs[i], out);
    out->emplace_back(node.keys[i], node.vals[i]);
  }
  InOrder(node.childs.back(), out);
}

// ---- Persistenz (Shadow-Paging) --------------------------------------------

bool BTreeKV::LoadAll() {
  nodes_.clear();
  dirty_.clear();
  free_.clear();
  root_ = 0;
  next_id_ = kFirstNodeId;
  count_ = 0;
  seq_ = 0;

  std::string raw;
  if (!pager_->find(kSuperKey, raw)) {
    // Frisch: genau ein leeres Blatt (unpersistiert bis zum ersten Flush).
    root_ = AllocNode(true);
    dirty_.clear();  // leeren Baum nicht schreiben muessen
    return true;
  }
  Super sb;
  if (!DecodeSuper(raw, sb)) return false;
  // Schranke gegen korrupte next-Werte (sonst OOM in seen-/Ladeschleife).
  if (sb.next < kFirstNodeId + 1 ||
      sb.next - kFirstNodeId > (1u << 24))
    return false;
  root_ = sb.root;
  next_id_ = sb.next;
  count_ = sb.count;
  seq_ = sb.seq;

  // IDs ohne Pager-Record sind freigegebene, nie persistierte Knoten
  // (FreeNode vor Flush): keine Records, kein Fehler — sie bleiben frei.
  // Echte Korruption (erreichbar, aber Record fehlt/dekodiert nicht) faellt
  // unten in der Erreichbarkeits-Pruefung auf.
  for (std::uint64_t id = kFirstNodeId; id < next_id_; ++id) {
    std::string nraw;
    if (!pager_->find(id, nraw)) continue;  // Loch: freigegebene ID
    Node node;
    if (!DecodeNode(nraw, node)) return false;
    nodes_[id] = std::move(node);
  }
  if (nodes_.find(root_) == nodes_.end()) return false;

  // Erreichbarkeit ab Wurzel (IDs muessen existieren). Zaehlt zugleich alle
  // erreichbaren Keys und gleicht sie mit dem Superblock-Count ab.
  std::vector<char> seen(next_id_, 0);
  std::vector<std::uint64_t> stack{root_};
  std::uint64_t reachable_keys = 0;
  while (!stack.empty()) {
    const std::uint64_t id = stack.back();
    stack.pop_back();
    if (id < kFirstNodeId || id >= next_id_) return false;
    if (seen[id]) continue;
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    seen[id] = 1;
    reachable_keys += static_cast<std::uint64_t>(it->second.keys.size());
    if (!it->second.leaf) {
      if (it->second.childs.size() != it->second.keys.size() + 1)
        return false;
      for (std::uint64_t c : it->second.childs) stack.push_back(c);
    }
  }
  if (reachable_keys != count_) return false;
  // Waisen-GC: Reste abgebrochener Commits loeschen, IDs wiederverwenden.
  // IDs ohne Record (Loecher) sind bereits frei — nichts zu loeschen.
  free_.clear();
  for (std::uint64_t id = kFirstNodeId; id < next_id_; ++id) {
    if (seen[id]) continue;
    if (nodes_.find(id) != nodes_.end()) {
      if (!pager_->erase(id)) return false;
      nodes_.erase(id);
    }
    free_.push_back(id);
  }
  return true;
}

bool BTreeKV::Persist() {
  if (!open_) return false;
  if (dirty_.empty()) return true;
  // Ein Batch = ein Image-Rewrite (statt einem pro Knoten).
  std::vector<std::pair<std::uint64_t, std::string>> batch;
  batch.reserve(dirty_.size());
  for (const auto& dirty : dirty_) {
    auto it = nodes_.find(dirty.first);
    if (it == nodes_.end()) continue;  // geloeschter Knoten (defensiv)
    batch.emplace_back(dirty.first, EncodeNode(it->second));
  }
  if (!batch.empty()) {
    if (!pager_->insert_batch(batch)) return false;
  }
  Super sb;
  sb.root = root_;
  sb.next = next_id_;
  sb.count = count_;
  sb.seq = seq_ + 1;
  if (!pager_->insert(kSuperKey, EncodeSuper(sb))) return false;
  seq_ = sb.seq;
  dirty_.clear();
  return true;
}

}  // namespace dbengine::kv
