#include "dbengine/txn/mvcc.h"

namespace dbengine::txn {

MvccStore::MvccStore() : next_ts_(1) {}

Timestamp MvccStore::BeginTimestamp() {
  // Eindeutige begin_ts ziehen; Snapshot = begin_ts (SI default).
  // Commit-TS wird spaeter separat gezogen und ist damit immer > begin_ts
  // aller zuvor begonnenen Txns -> korrekte Sichtbarkeit per <=.
  return next_ts_.fetch_add(1, std::memory_order_acq_rel);
}

void MvccStore::RegisterActive(std::uint64_t id, Timestamp snap) {
  std::lock_guard<std::mutex> g(mu_);
  active_.emplace(id, snap);
}

void MvccStore::UnregisterActive(std::uint64_t id) {
  std::lock_guard<std::mutex> g(mu_);
  active_.erase(id);
}

void MvccStore::ReleaseWriter(std::uint64_t owner) {
  bool owned = false;
  {
    std::lock_guard<std::mutex> g(mu_);
    if (active_writer_.has_value() && *active_writer_ == owner) {
      active_writer_.reset();
      owned = true;
    }
  }
  if (owned) writer_mu_.unlock();
}

Transaction MvccStore::BeginRead(Isolation iso) {
  Timestamp begin = BeginTimestamp();
  Transaction txn;
  txn.id = begin;
  txn.snapshot = begin;  // SI: fix; ReadCommitted: wird je Read aufgefrischt
  txn.isolation = iso;
  txn.read_only = true;
  // HINWEIS (Setup billig): write_set bleibt leer konstruiert (leere std::map
  // allokiert keinen Knoten), einziger Map-Knoten ist active_.emplace unten —
  // unvermeidbar als Purge-Watermark (Purge-Semantik unantastbar). Read spart
  // sich bei leerem Puffer den Lookup komplett (s. Read).
  RegisterActive(txn.id, txn.snapshot);
  return txn;
}

std::optional<Transaction> MvccStore::TryBeginWrite(Isolation iso) {
  if (!writer_mu_.try_lock()) {
    return std::nullopt;  // write-write serialisiert: ein Writer aktiv
  }
  Timestamp begin = BeginTimestamp();
  Transaction txn;
  txn.id = begin;
  txn.snapshot = begin;
  txn.isolation = iso;
  txn.read_only = false;
  {
    std::lock_guard<std::mutex> g(mu_);
    active_writer_ = txn.id;
    active_.emplace(txn.id, txn.snapshot);
  }
  return txn;
}

Transaction MvccStore::BeginWriteBlocking(Isolation iso) {
  writer_mu_.lock();
  Timestamp begin = BeginTimestamp();
  Transaction txn;
  txn.id = begin;
  txn.snapshot = begin;
  txn.isolation = iso;
  txn.read_only = false;
  {
    std::lock_guard<std::mutex> g(mu_);
    active_writer_ = txn.id;
    active_.emplace(txn.id, txn.snapshot);
  }
  return txn;
}

Timestamp MvccStore::EffectiveSnapshot(const Transaction& txn) const {
  if (txn.isolation == Isolation::ReadCommitted && !txn.read_only) {
    // Gilt auch fuer Reads innerhalb einer Schreib-Txn: frischester Stand.
    // Committete Ketten sind per Konstruktion die einzigen installierten,
    // daher ist next-1 ein sicherer "latest committed"-Proxy.
    Timestamp latest = next_ts_.load(std::memory_order_acquire);
    return latest == 0 ? 0 : latest - 1;
  }
  if (txn.isolation == Isolation::ReadCommitted) {
    Timestamp latest = next_ts_.load(std::memory_order_acquire);
    return latest == 0 ? 0 : latest - 1;
  }
  return txn.snapshot;
}

std::optional<std::string> MvccStore::Read(Transaction& txn,
                                           std::string_view key) {
  if (txn.state != TxnState::Active) return std::nullopt;
  // 1) Read-own-writes aus dem Undo-/Redo-Puffer.
  //    Allokationsfrei: leerer Puffer (Normalfall Read-Only) -> kein Lookup;
  //    sonst heterogener find(string_view) via less<> ohne temporaeren string.
  //    Rueckgabe-Kopie ist Resultat, kein Temporar.
  if (!txn.write_set.empty()) {
    auto itw = txn.write_set.find(key);
    if (itw != txn.write_set.end()) {
      return itw->second;  // kann nullopt (Delete-Tombstone) sein
    }
  }
  // 2) Snapshot-sichtbare Ketten-Version (neueste zuerst).
  //    snap-Berechnung UNVERAENDERT vor Lock (gleiche RC/SI-Semantik wie bisher).
  Timestamp snap = EffectiveSnapshot(txn);
  std::lock_guard<std::mutex> g(mu_);
  // Heterogener Lookup ohne std::string-Temporaer (less<>).
  auto itc = chains_.find(key);
  if (itc == chains_.end()) return std::nullopt;
  const auto& chain = itc->second;
  // 2a) Single-Version-Shortcut (semantikerhaltend, s. Header-Beweisidee):
  //     Bedingung = chain.size() == 1 && v.trx_end == kInfTs (committed/neueste,
  //     nur via Commit installierbar) && kein fremder Writer
  //     (!active_writer_ || *active_writer_ == txn.id). Eigener Writer ist ok:
  //     dessen Puffer enthaelt `key` nicht (oben per Miss bewiesen), und per
  //     Single-Writer-Invariant kann sonst niemand chains_ erweitern.
  //     Check ist EXAKT IsVisible (derselbe Aufruf wie in der Schleife).
  //     Aequivalenz: Vollsuche der Laenge 1 == IsVisible(sole) + Tombstone-Map;
  //     Shortcut macht exakt das; Guards waehlen nur Pfad, Fallback = Altcode.
  if (chain.size() == 1) {
    const bool no_foreign_writer =
        !active_writer_.has_value() || *active_writer_ == txn.id;
    if (no_foreign_writer && chain.front().trx_end == kInfTs) {
      const Version& v = chain.front();
      if (!IsVisible(v, snap)) return std::nullopt;
      if (v.deleted) return std::nullopt;
      return v.value;  // einzige Allokation: owning Resultat-Kopie
    }
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    if (IsVisible(*it, snap)) {
      if (it->deleted) return std::nullopt;
      return it->value;
    }
  }
  return std::nullopt;
}

bool MvccStore::Write(Transaction& txn, std::string_view key,
                      std::string_view value) {
  if (txn.state != TxnState::Active || txn.read_only) return false;
  {
    std::lock_guard<std::mutex> g(mu_);
    if (!active_writer_.has_value() || *active_writer_ != txn.id) return false;
  }
  if (key.empty()) return false;
  txn.write_set[std::string(key)] = std::string(value);
  return true;
}

bool MvccStore::Erase(Transaction& txn, std::string_view key) {
  if (txn.state != TxnState::Active || txn.read_only) return false;
  {
    std::lock_guard<std::mutex> g(mu_);
    if (!active_writer_.has_value() || *active_writer_ != txn.id) return false;
  }
  if (key.empty()) return false;
  txn.write_set[std::string(key)] = std::nullopt;
  return true;
}

bool MvccStore::Commit(Transaction& txn) {
  if (txn.state != TxnState::Active) return false;
  if (txn.read_only) {
    txn.state = TxnState::Committed;
    UnregisterActive(txn.id);
    return true;
  }
  // Writer-Inhaberschaft pruefen.
  {
    std::lock_guard<std::mutex> g(mu_);
    if (!active_writer_.has_value() || *active_writer_ != txn.id) return false;
  }
  // Atomare Installation unter EINER commit_ts (Single-Writer -> keine
  // First-Committer-Konflikte noetig; Serialisierung via writer_mu_).
  Timestamp commit_ts = CommitTimestamp();
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& [k, opt] : txn.write_set) {
      auto& chain = chains_[k];  // erzeugt Kette bei Bedarf
      if (!chain.empty()) {
        // Vorherige neueste Version schliessen (Undo-Link).
        if (chain.back().trx_end == kInfTs) {
          chain.back().trx_end = commit_ts;
        }
      }
      Version v;
      v.trx_begin = commit_ts;
      v.trx_end = kInfTs;
      v.deleted = !opt.has_value();
      v.value = opt.has_value() ? *opt : std::string{};
      chain.push_back(std::move(v));
    }
    active_.erase(txn.id);
    active_writer_.reset();
  }
  writer_mu_.unlock();
  txn.write_set.clear();
  txn.state = TxnState::Committed;
  return true;
}

void MvccStore::Abort(Transaction& txn) {
  if (txn.state != TxnState::Active) return;
  // Undo = Puffer verwerfen (nie installiert -> No-Op auf chains_).
  txn.write_set.clear();
  txn.state = TxnState::Aborted;
  if (txn.read_only) {
    UnregisterActive(txn.id);
    return;
  }
  ReleaseWriter(txn.id);
  // active_-Eintrag wurde in ReleaseWriter-Pfad noch nicht entfernt wenn
  // Abort ohne Commit; ReleaseWriter entfernt nur Writer-Owner, also hier:
  std::lock_guard<std::mutex> g(mu_);
  active_.erase(txn.id);
}

std::vector<Version> MvccStore::GetChain(const std::string& key) const {
  std::lock_guard<std::mutex> g(mu_);
  auto it = chains_.find(key);
  if (it == chains_.end()) return {};
  return it->second;
}

std::size_t MvccStore::VersionCount(const std::string& key) const {
  std::lock_guard<std::mutex> g(mu_);
  auto it = chains_.find(key);
  if (it == chains_.end()) return 0;
  return it->second.size();
}

std::size_t MvccStore::NumKeys() const {
  std::lock_guard<std::mutex> g(mu_);
  return chains_.size();
}

std::size_t MvccStore::Purge() {
  std::lock_guard<std::mutex> g(mu_);
  // Watermark = aeltester noch aktiver Snapshot; ohne aktive Txns alles
  // bis next-1. Versionen mit trx_end <= watermark braucht niemand mehr.
  Timestamp watermark;
  if (active_.empty()) {
    std::uint64_t nxt = next_ts_.load(std::memory_order_acquire);
    watermark = nxt == 0 ? 0 : nxt - 1;
  } else {
    watermark = kInfTs;
    for (const auto& [id, snap] : active_) {
      if (snap < watermark) watermark = snap;
    }
  }
  std::size_t removed = 0;
  for (auto& [k, chain] : chains_) {
    if (chain.size() <= 1) continue;
    // Finde aeltesten Index, der noch gebraucht wird: erste Version von
    // hinten mit trx_begin <= watermark muss bleiben (sichtbar fuer
    // Snapshot == watermark), alles davor kann weg. Neueste immer behalten.
    std::size_t keep_from = chain.size() - 1;  // Index der aeltesten zu haltenden
    for (std::size_t i = chain.size(); i-- > 0;) {
      if (chain[i].trx_begin <= watermark) {
        keep_from = i;
        break;
      }
      if (i == 0) break;
      keep_from = 0;
    }
    if (keep_from > 0) {
      chain.erase(chain.begin(),
                  chain.begin() + static_cast<std::ptrdiff_t>(keep_from));
      removed += keep_from;
    }
  }
  return removed;
}

}  // namespace dbengine::txn
