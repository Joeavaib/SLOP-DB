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

// Phase-0-Herzstueck: persistentes geordnetes KV als seitenbasierter B-Baum.
//
// Baum-Design
// -----------
// Klassischer B-Baum nach CLRS mit Mindestgrad t = 32:
//   - max. 2t-1 = 63 Keys / max. 2t = 64 Kinder je Knoten (Fanout 64),
//   - jeder Knoten ausser der Wurzel haelt stets t-1..2t-1 Keys,
//   - alle Blaetter haben gleiche Tiefe, Keys in jedem Knoten sortiert,
//   - Keys UND Values stehen in inneren Knoten wie in Blaettern
//     (kein B+-Blattketten-Overhead; Range-Scan via In-Order-Traversierung).
// Split: voller Kindknoten wird beim Abstieg gespalten (Median steigt auf).
// Merge/Redistribute: Loeschen fuellt untervolle Kinder auf (Ausleihen vom
//   Geschwister mit >= t Keys, sonst Verschmelzen); leere Wurzel schrumpft.
//
// Persistenz / Atomaritaet (Shadow-Paging, CoW)
// ---------------------------------------------
// Knoten ("Pages", serialisiert wenige KiB) liegen im Pager-Record-Image
// (dessen 16-KiB-Datenseiten): Superblock unter Pager-Key 0, Knoten-Id n
// unter Pager-Key n (Ids ab 1). Mutationen laufen zuerst nur im RAM;
// Flush() schreibt alle dirty Knoten (Copy-on-Write: neue Version des
// Knotens) und kippt danach den Superblock (root/next/count/seq)
// in EINEM Pager-Insert. Da Pager::insert via tmp+rename+fsync atomar ist,
// ist der Commit atomar: Crash davor -> alter Superblock = alter konsistenter
// Baum (verwaiste Knoten ignoriert, beim naechsten Open per GC entsorgt);
// Crash danach -> neuer Baum. WriteBatch validiert zuerst (leerer Key oder
// uebergrosser Eintrag => false, keine Teilmutation) und flusht danach
// automatisch (= dauerhafter Commit). Einzel-Put/Delete puffern im RAM und
// werden durch Flush()/Close() dauerhaft ("committed == geflusht").
//
// WICHTIG: absichtlich NICHT die Raw-Page-API (allocate/write_page), weil
// Pager::open()/flush() die Seiten 1..N als strikten Record-Stream parsen
// bzw. reserialisieren (load_image/store_image). Beliebige B-Baum-Bytes
// wuerden open() scheitern lassen bzw. von flush() mit Nullen ueberschrieben.
// Details siehe Risiken im Step-Bericht.
//
// Semantik-Anlehnung an KVStore (include/dbengine/kv.h):
// Get/Put/Delete/Scan/Iterator/WriteBatch; leere Keys ungueltig (Put/Delete
// ignorieren bzw. melden false, Write verwirft den Batch atomar).
// Abweichungen (dokumentiert): Put gibt bool zurueck (false bei
// geschlossen/ungueltig/uebergross statt stillem Ignorieren); kein Snapshot-
// Typ (Iteratoren sind per Kopie snapshot-stabil); Einzel-Put/Delete sind
// erst nach Flush()/Write()/Close() crash-fest; Eintraege auf ~1 MiB
// (Pager-Limit) begrenzt; kein WAL-Hook.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dbengine/kv.h"

namespace dbengine::storage {
class Pager;  // Ownership per unique_ptr: Pager ist weder kopier- noch
              // verschiebbar (mutex-Member macht die deklarierten Moves deleted).
}

namespace dbengine::kv {

// Persistenter geordneter KV-Store auf B-Baum-Basis. Eigene Klasse, kein
// Ersatz fuer KVStore in diesem Step. Single-writer-Modell (Mutex), keine
// Exceptions (Fehler => false/nullopt). STL/POSIX-only.
class BTreeKV {
 public:
  BTreeKV();
  ~BTreeKV();

  BTreeKV(const BTreeKV&) = delete;
  BTreeKV& operator=(const BTreeKV&) = delete;

  // ---- Lebenszyklus ----------------------------------------------------
  // Open legt die Datei an (Pager-Format) bzw. laedt einen vorhandenen Baum
  // (Superblock + alle Knoten, checksummen-geprueft, Waisen-GC). False bei I/O- oder
  // Korruptionsfehler. Bereits geoeffnet => false.
  [[nodiscard]] bool Open(const std::string& path);
  // Persistiert dirty Knoten + Superblock atomar (Commit). Ohne dirty Knoten
  // No-Op (true). False bei geschlossen oder I/O-Fehler (dirty bleibt).
  [[nodiscard]] bool Flush();
  void Close();
  [[nodiscard]] bool IsOpen() const;
  [[nodiscard]] const std::string& Path() const;

  // ---- Punktzugriffe ---------------------------------------------------
  [[nodiscard]] std::optional<std::string> Get(const std::string& key) const;
  // False bei geschlossen, leerem Key oder Eintrag > MaxEntryBytes().
  // Sonst RAM-gepuffert (dauerhaft nach Flush/Write/Close).
  bool Put(std::string key, std::string value);
  // True wenn Key existierte (delete-missing idempotent => false).
  bool Delete(const std::string& key);

  [[nodiscard]] std::size_t Size() const;
  [[nodiscard]] bool Empty() const;

  // ---- Scans (lexikographisch sortiert) --------------------------------
  // Bereich [from, to): to leer => bis Ende. limit 0 => leer.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> ScanRange(
      const std::string& from, const std::string& to,
      std::size_t limit = SIZE_MAX) const;
  // Prefix-Variante (KVStore-Paritaet).
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> Scan(
      const std::string& prefix, std::size_t limit = SIZE_MAX) const;

  // ---- WriteBatch: vector<Op> atomar -----------------------------------
  // Validierung zuerst (leerer Key oder Eintrag > MaxEntryBytes => false ohne
  // jede Mutation), dann alles-oder-nichts anwenden + automatisch flushen
  // (dauerhafter Commit). Leerer Batch => true (No-Op).
  bool Write(const std::vector<Op>& ops);
  bool Write(const WriteBatch& batch) { return Write(batch.ops()); }

  // ---- Iterator (stabile Kopie-Sicht, immun gegen spaetere Writes) -----
  class Iterator {
   public:
    void SeekToFirst() { idx_ = 0; }
    void Seek(const std::string& target);
    void Next() {
      if (idx_ < data_.size()) ++idx_;
    }
    [[nodiscard]] bool Valid() const noexcept { return idx_ < data_.size(); }
    [[nodiscard]] const std::string& key() const { return data_[idx_].first; }
    [[nodiscard]] const std::string& value() const {
      return data_[idx_].second;
    }

   private:
    friend class BTreeKV;
    explicit Iterator(std::vector<std::pair<std::string, std::string>> data)
        : data_(std::move(data)) {}
    std::vector<std::pair<std::string, std::string>> data_;
    std::size_t idx_ = 0;
  };

  [[nodiscard]] std::unique_ptr<Iterator> NewIterator(
      const std::string& prefix = "") const;

  // ---- Diagnose ---------------------------------------------------------
  // Height: 0 bei leer, 1 bei nur Wurzel, sonst Ebenen bis Blatt.
  [[nodiscard]] std::size_t Height() const;
  [[nodiscard]] std::size_t NodeCount() const;
  [[nodiscard]] std::uint64_t Sequence() const;  // Commit-Zaehler
  static constexpr std::size_t MaxEntryBytes() noexcept {
    // Pager-Limit (1 MiB pro Record) minus Serialisierungs-Slack.
    return (1u << 20) - 4096;
  }

 private:
  struct Node {
    bool leaf = true;
    std::vector<std::string> keys;
    std::vector<std::string> vals;  // parallel zu keys (auch intern)
    std::vector<std::uint64_t> childs;  // leaf: leer, intern: keys+1
  };

  static bool IsValidKey(const std::string& key) noexcept {
    return !key.empty();
  }
  static bool IsValidEntry(const std::string& key,
                           const std::string& value) noexcept {
    return IsValidKey(key) &&
           key.size() + value.size() <= MaxEntryBytes();
  }

  std::uint64_t AllocNode(bool leaf);
  void MarkDirty(std::uint64_t id);
  void FreeNode(std::uint64_t id);

  // Serialisierung (Knoten-Layout siehe btree.cpp).
  static std::string EncodeNode(const Node& node);
  static bool DecodeNode(const std::string& in, Node& node);

  // B-Baum-Kern (CLRS). IDs statt Pointer (persistenzfaehig).
  void InsertNonFull(std::uint64_t id, const std::string& key,
                     const std::string& value);
  void SplitChild(std::uint64_t parent, std::size_t i);
  bool Remove(std::uint64_t id, const std::string& key);
  std::size_t FillChild(std::uint64_t parent, std::size_t idx);
  void BorrowFromPrev(std::uint64_t parent, std::size_t idx);
  void BorrowFromNext(std::uint64_t parent, std::size_t idx);
  void MergeChild(std::uint64_t parent, std::size_t idx);
  void ShrinkRoot();
  std::pair<std::string, std::string> MaxOf(std::uint64_t id) const;
  std::pair<std::string, std::string> MinOf(std::uint64_t id) const;
  void InOrder(std::uint64_t id,
               std::vector<std::pair<std::string, std::string>>* out) const;
  // s115: sortierter Besuch mit Early-Stop + Subtree-Pruning. `from` = inkl.
  // Untergrenze ("" = alles), `has_to`/`to` = exkl. Obergrenze. fn(key,val)
  // liefert false bei Abbruchwunsch (Limit erreicht). Rueckgabe false =
  // abgebrochen (Limit oder to erreicht), true = Teilbaum vollstaendig.
  // Pruning: Kind-Teilbaeume, deren Keys alle < from sind, werden gar nicht
  // betreten; nach `to` wird nicht weiter abgestiegen. Semantik identisch zu
  // gefiltertem InOrder (gleiche Reihenfolge, gleiche Menge bis Abbruch).
  bool VisitRange(std::uint64_t id, const std::string& from, bool has_to,
                  const std::string& to,
                  const std::function<bool(const std::string&,
                                           const std::string&)>& fn) const;

  // Persistenz (Shadow-Paging ueber Pager-Hull).
  bool LoadAll();  // Superblock + Knoten laden, Waisen-GC
  bool Persist();  // dirty Knoten + Superblock schreiben (Caller haelt Lock)

  mutable std::mutex mutex_;
  std::unique_ptr<storage::Pager> pager_;
  std::string path_;
  bool open_ = false;

  std::unordered_map<std::uint64_t, Node> nodes_;
  std::unordered_map<std::uint64_t, char> dirty_;  // Set via Map (char ungenutzt)
  std::vector<std::uint64_t> free_;
  std::uint64_t root_ = 0;
  std::uint64_t next_id_ = 1;
  std::uint64_t count_ = 0;
  std::uint64_t seq_ = 0;
};

}  // namespace dbengine::kv
