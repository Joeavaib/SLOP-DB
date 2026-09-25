#pragma once

// KV-API (s05-kv): unterste Schicht der DB-Engine (RocksDB/FoundationDB-Vorbild).
// Backend V1: std::map<string,string> + Mutex + Sequenznummer (LSN-Stub).
// Snapshot V1: Map-Copy (MVCC-Stub, O(n)) — wird in s04-mvcc durch
//   per-Key-Versionen mit Seq-Sichtbarkeit ersetzt, API bleibt stabil.
// WAL: No-Op-Hook (s03-wal kann echten REDO-Append hier einklinken).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dbengine::kv {

// Einzelne Mutation. Leerer Key ist ungueltig (Batch wird dann atomar verworfen).
struct Op {
  enum class Type : std::uint8_t { Put, Delete };

  Type type = Type::Put;
  std::string key;
  std::string value;  // nur fuer Put relevant

  static Op PutOp(std::string key, std::string value) {
    Op op;
    op.type = Type::Put;
    op.key = std::move(key);
    op.value = std::move(value);
    return op;
  }
  static Op DeleteOp(std::string key) {
    Op op;
    op.type = Type::Delete;
    op.key = std::move(key);
    return op;
  }
};

// Builder fuer atomare Batches. Alternativ kann direkt vector<Op> uebergeben werden.
class WriteBatch {
 public:
  WriteBatch() = default;

  void Put(std::string key, std::string value) {
    ops_.push_back(Op::PutOp(std::move(key), std::move(value)));
  }
  void Delete(std::string key) { ops_.push_back(Op::DeleteOp(std::move(key))); }
  void Clear() { ops_.clear(); }

  [[nodiscard]] bool Empty() const noexcept { return ops_.empty(); }
  [[nodiscard]] std::size_t Count() const noexcept { return ops_.size(); }
  [[nodiscard]] const std::vector<Op>& ops() const noexcept { return ops_; }

 private:
  std::vector<Op> ops_;
};

// WAL-Hook: wird nach erfolgreichem Commit je Op aufgerufen (seq = Commit-LSN).
// Default: No-Op. s03-wal setzt hier den REDO-Append.
using WalHook = std::function<void(const Op& op, std::uint64_t seq)>;

// Point-in-Time-Sicht (Map-Copy). Threadsicher lesbar ohne KVStore-Lock.
class Snapshot {
 public:
  [[nodiscard]] std::optional<std::string> Get(const std::string& key) const {
    auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;
    return it->second;
  }

  // Sortierter Prefix-Scan auf der Snapshot-Sicht.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> Scan(
      const std::string& prefix, std::size_t limit = SIZE_MAX) const;

  [[nodiscard]] std::size_t Size() const noexcept { return data_.size(); }
  [[nodiscard]] bool Empty() const noexcept { return data_.empty(); }
  [[nodiscard]] std::uint64_t Sequence() const noexcept { return seq_; }

 private:
  friend class KVStore;
  std::map<std::string, std::string> data_;
  std::uint64_t seq_ = 0;
};

class KVStore {
 public:
  KVStore() = default;
  KVStore(const KVStore&) = delete;
  KVStore& operator=(const KVStore&) = delete;

  // ---- Punktzugriffe -----------------------------------------------------
  [[nodiscard]] std::optional<std::string> Get(const std::string& key) const;
  [[nodiscard]] std::optional<std::string> Get(const Snapshot& snap,
                                               const std::string& key) const {
    return snap.Get(key);
  }

  void Put(std::string key, std::string value);
  // true wenn Key existierte (delete-missing ist kein Fehler, bleibt idempotent).
  bool Delete(const std::string& key);

  [[nodiscard]] std::size_t Size() const;
  [[nodiscard]] bool Empty() const;

  // ---- Scan (lexikographisch sortiert, wie std::map) ----------------------
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> Scan(
      const std::string& prefix, std::size_t limit = SIZE_MAX) const;
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> Scan(
      const Snapshot& snap, const std::string& prefix,
      std::size_t limit = SIZE_MAX) const {
    return snap.Scan(prefix, limit);
  }

  // ---- WriteBatch: vector<Op> atomar -------------------------------------
  // Validierung zuerst (leerer Key => false), dann alles-oder-nichts unter
  // einem Mutex. Gibt true bei Commit zurueck, false ohne jede Mutation.
  bool Write(const std::vector<Op>& ops);
  bool Write(const WriteBatch& batch) { return Write(batch.ops()); }

  // ---- Snapshot (copy-on-read via Map-Copy, MVCC-Stub) --------------------
  [[nodiscard]] std::shared_ptr<Snapshot> GetSnapshot() const;

  // ---- Iterator (stabile Snapshot-Sicht, immun gegen spaetere Writes) -----
  class Iterator {
   public:
    void SeekToFirst() { idx_ = 0; }
    void Seek(const std::string& target);
    void Next() {
      if (idx_ < data_->size()) ++idx_;
    }
    [[nodiscard]] bool Valid() const noexcept { return idx_ < data_->size(); }
    [[nodiscard]] const std::string& key() const { return (*data_)[idx_].first; }
    [[nodiscard]] const std::string& value() const {
      return (*data_)[idx_].second;
    }

   private:
    friend class KVStore;
    explicit Iterator(
        std::shared_ptr<const std::vector<std::pair<std::string, std::string>>>
            data)
        : data_(std::move(data)) {}
    std::shared_ptr<const std::vector<std::pair<std::string, std::string>>> data_;
    std::size_t idx_ = 0;
  };

  // Voller DB-Scan (prefix="" => alles). Prefix-Variante fuer SQL/Vector-Layer.
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator(
      const std::string& prefix = "") const;
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator(
      const Snapshot& snap, const std::string& prefix = "") const;

  // ---- WAL-Hook -----------------------------------------------------------
  void SetWalHook(WalHook hook);
  [[nodiscard]] std::uint64_t Sequence() const;

 private:
  static bool IsValidKey(const std::string& key) noexcept {
    return !key.empty();
  }
  static bool HasPrefix(const std::string& key,
                        const std::string& prefix) noexcept;

  mutable std::mutex mutex_;
  std::map<std::string, std::string> map_;
  std::uint64_t seq_ = 0;  // Commit-LSN-Stub (s03-wal/s04-mvcc uebernehmen)
  WalHook wal_hook_;
};

}  // namespace dbengine::kv
