#pragma once

// SecondaryIndex: B-Tree-artiger Sekundaerindex (key -> primaryKey Liste).
//
// Trennung Primary/Secondary:
//   - Primaerdaten (Row/Heap/KV-Payload) leben NICHT hier.
//   - Diese Klasse speichert nur (Sekundaerschluessel -> PK)-Paare.
//   - Lookup/RangeScan liefern PK-Listen; Aufloesung der Zeilen erfolgt
//     ueber den Primary-Index (kein Payload-Duplikat, kein Full-Scan).
//
// Implementierung V1: std::multimap (RB-Tree, O(log n) Insert/Lookup,
// geordnete Iteration fuer RangeScan via lower_bound/upper_bound).
// Spaeter: paged B+Tree (16KB Pages, CoW) hinter gleichem Interface.
//
// ART/GIN: siehe Stubs unten (ArtIndexStub, GinIndexStub).

#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dbengine::index {

struct IndexEntry {
  int64_t primaryKey = 0;
  int64_t createdSec = 0;  // Insert-Zeitpunkt (Sekunden seit Epoch)
};

class SecondaryIndex {
 public:
  explicit SecondaryIndex(uint64_t expireAfterSec = 0);

  // TTL: 0 = kein Ablauf. >0 = Eintraege laufen nach N Sekunden ab.
  void setExpireAfterSec(uint64_t s);
  [[nodiscard]] uint64_t expireAfterSec() const;

  // --- Schreibpfad ---
  void insert(int64_t secondaryKey, int64_t primaryKey);
  // Test-/Simulations-Overload mit explizitem Zeitstempel:
  void insert(int64_t secondaryKey, int64_t primaryKey, int64_t nowSec);

  // Entfernt genau ein (skey,pk)-Paar. true wenn gefunden.
  bool remove(int64_t secondaryKey, int64_t primaryKey);

  // --- Lesepfad (KEIN Full-Scan) ---
  // lookup nutzt equal_range -> nur Treffer-Partition wird geprueft.
  [[nodiscard]] std::vector<int64_t> lookup(int64_t secondaryKey) const;
  [[nodiscard]] std::vector<int64_t> lookup(int64_t secondaryKey,
                                           int64_t nowSec) const;

  // RangeScan [low, high] inklusiv, aufsteigend nach secondaryKey.
  // Nutzt lower_bound/upper_bound -> nur Range-Fenster wird geprueft.
  [[nodiscard]] std::vector<std::pair<int64_t, int64_t>> rangeScan(
      int64_t low, int64_t high) const;
  [[nodiscard]] std::vector<std::pair<int64_t, int64_t>> rangeScan(
      int64_t low, int64_t high, int64_t nowSec) const;

  // Anzahl lebender (nicht abgelaufener) Eintraege zum aktuellen Zeitpunkt.
  [[nodiscard]] size_t size() const;
  [[nodiscard]] size_t size(int64_t nowSec) const;
  // Rohgroesse inkl. abgelaufener, noch nicht gesweepter Eintraege.
  [[nodiscard]] size_t rawSize() const;
  [[nodiscard]] bool empty() const;

  // Lazy-Sweep: einziger Full-Scan-Pfad, explizit fuer GC/Retention.
  // Gibt Anzahl entfernter Eintraege zurueck.
  size_t sweepExpired();
  size_t sweepExpired(int64_t nowSec);

  void clear();

  // Test-Uhr: override >= 0 erzwingt nowSec() (sonst Wall-Clock).
  // Erlaubt deterministische TTL-Tests ohne Sleep.
  void setNowOverride(int64_t t);
  void clearNowOverride();

 private:
  [[nodiscard]] int64_t nowSec() const;
  [[nodiscard]] bool expired(int64_t createdSec, int64_t now) const;

  mutable std::mutex mu_;
  // Geordneter Multimap: B-Tree-Semantik (log Insert, geordneter Scan).
  std::multimap<int64_t, IndexEntry> idx_;
  uint64_t expireAfterSec_ = 0;
  mutable int64_t nowOverrideSec_ = -1;  // -1 = Wall-Clock
};

// ---------------------------------------------------------------------------
// ART-Stub (Adaptive Radix Tree): geplant fuer In-Memory/String-Schluessel
// (VARCHAR, Pfade, Embedded). V1 NICHT implementiert – Interface reserviert,
// damit CBO/Planner spaeter per Index-Typ dispatchen kann.
// ---------------------------------------------------------------------------
class ArtIndexStub {
 public:
  void insert(const std::string& key, int64_t primaryKey);
  [[nodiscard]] std::vector<int64_t> lookup(const std::string& key) const;
};

// ---------------------------------------------------------------------------
// GIN-Stub (Generalized Inverted Index): geplant fuer Multikey/Document
// (JSONB-Arrays, FTS-Tokens, Array-Spalten: ein Dokument -> N Posting-Keys).
// V1 NICHT implementiert – wirft logic_error, bis Posting-Listen + BM25
// (s09-hybrid) landen.
// ---------------------------------------------------------------------------
class GinIndexStub {
 public:
  void insert(int64_t docPk, const std::vector<std::string>& tokens);
  [[nodiscard]] std::vector<int64_t> lookup(
      const std::string& token) const;
};

}  // namespace dbengine::index
