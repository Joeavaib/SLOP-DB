#pragma once

// MVCC + Transaktionen (s04-mvcc): Undo-Log-Gedanke, kein Heap-Bloat.
//
// Design (InnoDB-nahe, bewusst NICHT Postgres-Heap):
// - Genau EINE gueltige Zeilen-Version liegt in der "Primary"-Kette,
//   alte Versionen sind Undo-Historie pro Key und werden via GC (Purge)
//   entsorgt, sobald kein aktives Snapshot sie mehr braucht.
// - Writes werden erst im Txn-Buffer (write_set = Redo-/Undo-Puffer)
//   gesammelt und beim Commit atomar installiert. Abort = Buffer verwerfen
//   (Undo = No-Op, da nie installiert). Dadurch per Konstruktion:
//   keine Dirty-Reads, keine partiellen Commits.
// - TSO-aehnlich: atomic<uint64_t> next_ts_. Begin zieht begin_ts,
//   Commit zieht commit_ts. Snapshot-Isolation default: Snapshot = begin_ts,
//   sichtbar ist Version mit trx_begin <= snapshot < trx_end.
// - Single-Writer pro Shard: genau eine aktive Schreib-Txn (std::mutex
//   writer_mu_ + active_writer_). TryBeginWrite() scheitert sofort wenn
//   besetzt (write-write serialisiert), BeginWriteBlocking() wartet.
// - Read-Only-Txns sind lockfrei gegenueber Writer (nur kurzer mu_ fuer
//   Ketten-Lookup), beliebig parallel.
// - Luecke (bewusst): kein SSI/Serializable (nur Snapshot + ReadCommitted),
//   kein Distributed/2PC, keine echten TSO-Orakel/HLC (lokaler Zaehler).

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dbengine::txn {

using Timestamp = std::uint64_t;
inline constexpr Timestamp kInfTs = UINT64_MAX;
inline constexpr Timestamp kInvalidTs = 0;

enum class Isolation : std::uint8_t { Snapshot, ReadCommitted };

enum class TxnState : std::uint8_t { Active, Committed, Aborted };

// Eine Version in der Kette eines Keys (Undo-Log-Eintrag).
struct Version {
  Timestamp trx_begin = kInvalidTs;  // Commit-TS, die diese Version erzeugte
  Timestamp trx_end = kInfTs;        // Commit-TS, die sie abloeste (INF = aktuell)
  std::string value;                 // Nutzdaten (leer bei deleted)
  bool deleted = false;              // Tombstone (Undo-Delete)
};

// Sichtbarkeits-Praedikat fuer Snapshot-Reads.
inline bool IsVisible(const Version& v, Timestamp snapshot) {
  return v.trx_begin <= snapshot && snapshot < v.trx_end;
}

struct Transaction {
  std::uint64_t id = 0;              // begin_ts (TSO-aehnlich, eindeutig)
  Timestamp snapshot = kInvalidTs;   // Lese-Snapshot (SI: fix bei Begin)
  TxnState state = TxnState::Active;
  Isolation isolation = Isolation::Snapshot;
  bool read_only = false;
  // Ungeschriebene (uncommittete) Writes: Undo-/Redo-Puffer.
  // nullopt = Delete-Tombstone im Puffer.
  std::map<std::string, std::optional<std::string>> write_set;

  bool IsActive() const { return state == TxnState::Active; }
};

class MvccStore {
 public:
  MvccStore();
  MvccStore(const MvccStore&) = delete;
  MvccStore& operator=(const MvccStore&) = delete;

  // ---- Transaktionsgrenzen ---------------------------------------------
  Transaction BeginRead(Isolation iso = Isolation::Snapshot);
  std::optional<Transaction> TryBeginWrite(
      Isolation iso = Isolation::Snapshot);
  Transaction BeginWriteBlocking(Isolation iso = Isolation::Snapshot);

  // ---- Datenoperationen (nur mit aktiver Txn) ---------------------------
  // Read sieht zuerst eigene Buffered-Writes (read-own-writes), dann die
  // Snapshot-sichtbare Ketten-Version. nullopt = nicht vorhanden/geloescht.
  std::optional<std::string> Read(Transaction& txn, std::string_view key);

  // Buffered Write (Undo-Puffer). false wenn txn nicht (mehr) schreibfaehig.
  bool Write(Transaction& txn, std::string_view key, std::string_view value);
  bool Erase(Transaction& txn, std::string_view key);

  // ---- Abschluss ---------------------------------------------------------
  // Commit installiert alle Buffered-Writes atomar unter EINER commit_ts.
  // false wenn txn nicht aktiv / nicht Writer-Inhaber.
  bool Commit(Transaction& txn);
  // Abort verwirft den Puffer (Undo = No-Op). Idempotent fuer aktive Txns.
  void Abort(Transaction& txn);

  // ---- Introspektion / Wartung -------------------------------------------
  std::vector<Version> GetChain(const std::string& key) const;
  std::size_t VersionCount(const std::string& key) const;
  std::size_t NumKeys() const;
  Timestamp SnapshotOf(const Transaction& txn) const { return txn.snapshot; }

  // Purge (Undo-GC): entfernt pro Key alte Versionen, die fuer kein aktives
  // Snapshot mehr sichtbar sein koennen. Gibt Anzahl geloeschter Versionen
  // zurueck. Nie wird die neueste Version entfernt (kein Heap-Bloat).
  std::size_t Purge();

  std::uint64_t NextTimestamp() const {
    return next_ts_.load(std::memory_order_acquire);
  }

 private:
  Timestamp BeginTimestamp();
  Timestamp CommitTimestamp() { return next_ts_.fetch_add(1); }
  Timestamp EffectiveSnapshot(const Transaction& txn) const;
  void RegisterActive(std::uint64_t id, Timestamp snap);
  void UnregisterActive(std::uint64_t id);
  void ReleaseWriter(std::uint64_t owner);

  std::atomic<std::uint64_t> next_ts_;  // TSO-aehnlicher Zaehler, startet bei 1
  mutable std::mutex mu_;               // schuetzt chains_ + active_ + Writer-Owner
  std::mutex writer_mu_;                // Single-Writer-Lock pro Shard
  std::optional<std::uint64_t> active_writer_;  // unter mu_
  std::map<std::string, std::vector<Version>> chains_;  // unter mu_, oldest->newest
  std::map<std::uint64_t, Timestamp> active_;           // txn-id -> snapshot
};

}  // namespace dbengine::txn
