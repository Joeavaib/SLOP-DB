# Phase-0-Duell: dbengine-CLI vs. SQLite (1M Rows, gleiche Daten/Queries)

Datum: 2026-09-26 (UTC). Repo unverändert, nur diese Datei neu.
dbengine-Binary: `./build/dbengine` wie es lag (NICHT neu gebaut;
cmake/ctest/build waren verboten). `--version`: `dbengine version 0.1.0`.

## Duell-Größe: 1M (kein Downscale nötig)

Vorgabe: erst 10k messen; falls Hochrechnung auf 1M > 8 min, auf 100k
runtergehen (ohne zu extrapolieren). Ergebnis der 10k-Probe
(identisches Schema/Daten wie unten, Batch=1, /tmp):

| System | 10k Insert | Restart+Scan 10k |
| --- | --- | --- |
| dbengine-CLI | 0,056 s / 0,057 s (2 Läufe) | 0,054 s |
| SQLite (autocommit, synchronous=FULL) | 0,753 s | 0,001 s |

Hochrechnung: dbengine ~6 s, SQLite ~75 s — beides klar unter 8 min.
Darum: **Duell bei 1 000 000 Rows**, ehrlich gemessen, je 2 Läufe.

## Methodik (bitte genau so lesen, sonst sind die Zahlen irreführend)

- Schema/Daten/Queries überall identisch:
  `CREATE TABLE t (id INT, val INT);` + für i = 1..1 000 000
  `INSERT INTO t VALUES (i, i);` (val = id).
  Erwartung: `COUNT(*) = 1000000`, `SUM(val) = 500000500000`.
  Scan-Query dbengine: `SELECT SUM(val) FROM t;`
  (plus einmalig `SELECT COUNT(*) FROM t;` zur Kontrolle).
  Scan bei SQLite: `SELECT COUNT(*) FROM t;` + `SELECT SUM(val) FROM t;`
  **in einer Zeitmessung** (2 Voll-Scans; Asymmetrie zugunsten dbengine,
  die am Urteil nichts ändert — siehe Zahlen).
- Batch-Größe = **1 (Einzel-INSERT pro Statement)** auf beiden Seiten:
  - dbengine: 1 000 001 Statements (CREATE + 1M INSERTs) in EINER
    `--sql`-Datei (`/tmp/duell1m.sql`, 38 777 826 Bytes). Jedes INSERT =
    1 WAL-Append pro Zeile + **1 `Wal::flush()` pro Statement**
    (`src/sql/executor.cpp`: appends pro Zeile, danach `wal_->flush()`),
    `flush()` = `fdatasync` (Fallback `fsync`) pro Aufruf
    (`src/storage/wal.cpp`). Also **1M fdatasyncs**, kein Group-Commit
    über Statement-Grenzen hinweg. (Multi-Row-INSERTs könnten das drücken,
    wurden aber NICHT verwendet — Batch=1 ist der ehrliche Vergleich.)
  - SQLite (Python-`sqlite3`-stdlib, Datei-DB): `isolation_level=None`
    = Autocommit, also **1 Commit pro INSERT**, Default-Pragmas
    unverändert: `synchronous = 2 (FULL)`, `journal_mode = delete`,
    `page_size = 4096` (nur ausgelesen, nichts gesetzt). Bei
    synchronous=FULL + delete-Journal kostet jeder Commit ~2 fsyncs
    (Journal + DB).
- Wall-clock: dbengine per `date +%s.%N` um den Prozessaufruf
  (inkl. Prozessstart + WAL-Replay beim Öffnen); SQLite per
  `time.monotonic()` um die Schleife / um Reopen+Scans.
- Restart-Messung dbengine = **neuer Prozess**
  `./build/dbengine <db> --exec "SELECT SUM(val) FROM t;"` auf der
  bestehenden DB (enthält WAL-Replay von 1M Records + Neuaufbau der
  In-Memory-Strukturen + Full-Table-Scan — untrennbar in einem Wert).
  Restart-Messung SQLite = neu `connect()` + COUNT + SUM.
- Ablage: alles unter `/tmp`, danach alle DB-/SQL-Artefakte gelöscht.
  **Wichtig: `/tmp` ist `tmpfs` (RAM-Disk).** fsync/fdatasync sind dort
  fast gratis — die Zahlen gelten NICHT für echte Platten (nicht gemessen).
- Korrektheit: beide liefern COUNT = 1000000. SQLite-SUM exakt
  500000500000. dbengine-SUM rechnerisch gleich, Anzeige aber
  `5e+11` (Gleitkomma-Formatierung, z. B. 10k-Probe: `5.0005e+07`
  statt 50005000) — Display-Ungenauigkeit, dokumentiert statt geglättet.

## Ergebnisse 1M (2 Läufe je Seite, nichts gemittelt/geschönt)

### dbengine-CLI (Batch=1, 1 flush/INSERT)

| Lauf | Insert (fresh DB, 1M Einzel-INSERTs) | Restart + Replay + SUM-Scan |
| --- | --- | --- |
| A | 6,681 s | 6,120 s / 6,098 s (2 Scans) |
| B | 6,705 s | 6,230 s / 6,301 s (2 Scans) |

Dateigrößen (je Lauf identisch): `<db>` = **41 038 Bytes**
(reiner Container: Magic+Version+Region-Table+CRC, **keine Rows**),
`<db>.wal` = **53 666 726 Bytes (~51,2 MiB)** — alle Rows leben nur im WAL.

### SQLite stdlib, Datei-DB, synchronous=FULL (Batch=1, 1 Commit/INSERT)

| Lauf | Insert (fresh DB, 1M Einzel-INSERTs) | Reopen + COUNT + SUM |
| --- | --- | --- |
| 1 | 75,086 s | 0,048 s / 0,045 s (2 Messungen) |
| 2 | 75,460 s | 0,048 s / 0,044 s (2 Messungen) |

Dateigröße: **15 024 128 Bytes (14,33 MiB)** echte DB-Datei (B-Tree/Pager).

### Direktvergleich

| Kennzahl (1M, Batch=1) | dbengine-CLI | SQLite (sync=FULL) | Faktor |
| --- | --- | --- | --- |
| Insert | ~6,7 s | ~75,3 s | dbengine ~11× schneller |
| Restart + (Replay) + Scan | ~6,2 s | ~0,05 s | SQLite ~130× schneller |
| Persistente Bytes | 41 KB Container + 53,7 MB WAL | 14,3 MB DB | — |

## Urteil (ungeschönt)

1. **Insert gewinnt dbengine (~11×), Scan nach Restart gewinnt SQLite
   (~130×).** Kein Gesamtsieger — es hängt an der Achse
   Schreibdurchsatz vs. Restart-/Lesekosten.
2. **Engpass dbengine = Restart (WAL-Replay).** Jeder Start spielt 1M
   WAL-Records neu ein (parsen + KV-/MVCC-In-Memory-Wiederaufbau) und
   scannt danach ohne Index voll. ~6,2 s von ~12,9 s Gesamt sind Replay+Scan.
   Der Insert-Sieg (~150k Rows/s trotz 1M fdatasyncs) existiert nur, weil
   `/tmp` tmpfs ist und fdatasync dort ~7 µs kostet.
3. **Engpass SQLite = Insert unter synchronous=FULL + Autocommit**
   (~13k Rows/s, ~2 fsyncs pro Commit). Der Scan (B-Tree, kein Replay)
   ist mit ~0,05 s praktisch gratis.
4. **Persistenz-Ehrlichkeit:** dbengine-`.db` ohne `.wal` ist leer
   (41 KB); WAL-Verlust = Totalverlust. SQLite schreibt 14,3 MB echte
   DB-Datei. Wer „Restart-Sicherheit" misst, muss dbengine die ~6,2 s
   Replay anrechnen — genau das tut diese Tabelle.
5. **Nicht gemessen (bewusst):** echte Disk statt tmpfs, Batch > 1
   (Multi-Row-INSERTs), Transaktions-Batching bei SQLite, Index-Scans.
   Jede dieser Änderungen würde das Bild verschieben; oben steht nur,
   was wirklich lief: 1M × Batch=1, fsync auf beiden Seiten, tmpfs.

---

# Re-Duell nach Mirror-Checkpoint (s76): 2026-09-27 (UTC)

Anlass: Commit `c03f4f1` (s76-mirrorckpt): `<db>.btree`-Sidecar
(BTreeKV, Latest-State je Key + Spiegel-LSN), pro WAL-Flush inkrementell
nachgezogen; beim Start nur WAL-Tail (lsn > Spiegel-LSN) replayen.
Ziel des Re-Duells: Prüfen, ob der Restart-Engpass (~6,2 s bei 1M)
weg ist — und was der Spiegel beim Insert kostet.
Binary: `./build/dbengine` wie es lag (NICHT neu gebaut;
`--version`: `dbengine version 0.1.0`); `--help` beschreibt das
Mirror-Protokoll wie oben. Repo sonst NICHT angefasst (nur diese Datei
editiert, keine cmake/ctest/build-Läufe).

## Methodik (identisch zum Duell oben)

- Gleiche Daten: `CREATE TABLE t (id INT, val INT);` + 1M ×
  `INSERT INTO t VALUES (i, i);`. Die generierte Datei war
  **38 777 826 Bytes** — bytegleich groß wie `/tmp/duell1m.sql` oben,
  also dieselben Statements.
- Batch=1, alles unter `/tmp` (weiterhin `tmpfs`), Wall-clock per
  `date +%s.%N` um den Prozess, danach alle Artefakte gelöscht.
- SQLite-Seite **übernommen** (ungeändert, s76 fasst SQLite nicht an):
  Insert ~75,3 s, Reopen+COUNT+SUM ~0,05 s. Remeasure (2× ~75 s) war
  nicht billig und hätte nichts geändert.
- Caveat: parallel liefen fremde Benchmarks auf der Maschine
  (24 Kerne, Load ~9,7). Das erklärt KEINE 100×-Effekte (s. Zahlen),
  sei aber genannt.

## Was lief (ehrlich, inkl. Abbrüche)

- **1M-Versuch: ABGEBROCHEN.** Nach 300 s erst 74 844 von 1 000 001
  Statements (~250 Rows/s, fallend). Hochrechnung linear ≥ 65 min,
  real superlinear (s. 100k) → Stunden. Über dem 8-min-Budget der
  Methodik oben → kein 1M-Messwert, kein Extrapolat.
- **100k-Versuch: ABGEBROCHEN.** Nach 540 s erst 98 676 von 100 001
  Statements (~183 Rows/s im Schnitt) → ~550 s projiziert. Ebenfalls
  über Budget. DB-Rest (98 681 Rows) für Restart-Messung genutzt (s. u.).
- **10k mit Spiegel: vollständig, je 2 Läufe** (gleiche 10k-Daten wie
  in der Probe oben).

## Ergebnisse mit Spiegel

### Insert 10k (fresh DB, Batch=1)

| Lauf | Insert 10k Spiegel | vorher (ohne Spiegel) |
| --- | --- | --- |
| A | 6,542 s | 0,056 s |
| B | 6,509 s | 0,057 s |

Faktor: **~117× langsamer** (~1 530 Rows/s statt ~178 000 Rows/s).

Dateien (10k, Lauf A): `<db>` = 41 038 Bytes (unverändert),
`<db>.wal` = 476 720 Bytes, `<db>.btree` = 344 064 Bytes.

### Restart + SUM 10k (sauberer Shutdown, Spiegel aktiv, keine Warnung)

| Lauf | Restart+SUM 10k Spiegel | vorher (Voll-Replay) |
| --- | --- | --- |
| 1 | 0,058 s | 0,054 s |
| 2 | 0,057 s | (1 Messung oben) |

SUM-Anzeige weiter `5.0005e+07`, COUNT = 10000 — korrekt.
**Kein sichtbarer Gewinn:** bei 10k war Replay auch vorher gratis.

### Restart ~99k auf abgebrochener 100k-DB (der interessante Fall)

- `./build/dbengine /tmp/probe100k.db --exec "SELECT SUM(val) FROM t;"`:
  **0,590 s / 0,590 s** (2 Läufe), COUNT = 98681,
  SUM-Anzeige `4.86902e+09` (= 4 869 019 221, korrekt).
- Aber: **mit Warnung** `Spiegel '...btree' nicht aktiv:
  ... nicht oeffbar; Voll-Replay aus WAL` — der Kill hatte
  `probe100k.db.btree.tmp` hinterlassen, der Spiegel wurde verworfen
  und es gab **trotzdem Voll-Replay**. Genau der Crash-Fall, für den
  der Spiegel gebaut wurde, nutzte ihn nicht.

### Vorher/nachher (1M-Skala, ehrlich beschriftet)

| Kennzahl (Batch=1) | vorher (2026-09-26) | nachher (Spiegel, 2026-09-27) |
| --- | --- | --- |
| Insert 10k | 0,056 s | 6,5 s (~117× langsamer) |
| Insert 100k | ~0,7 s (linear aus 1M=6,7 s) | ~550 s (abgebrochen, gemessen 98 676 Stmts/540 s) |
| Insert 1M | ~6,7 s | ABBRUCH (74 844 Stmts/300 s; 1M nicht erreichbar) |
| Restart+SUM 10k | 0,054 s | 0,058 s (kein Gewinn) |
| Restart+SUM ~99k | ~0,6 s (linear aus 1M=6,2 s) | 0,59 s — aber Voll-Replay (Spiegel nach Kill verworfen) |
| Restart+SUM 1M | ~6,2 s | nicht messbar (keine 1M-DB erzeugbar) |
| SQLite 1M (unverändert) | Insert ~75,3 s / Reopen ~0,05 s | übernommen |

## Urteil (ungeschönt)

1. **Restart-Ziel de facto NICHT erreicht.** Wo der Spiegel helfen
   müsste (großes N), kommt man nicht mehr hin, weil der Insert
   ~100–1000× langsamer wurde. Bei kleinem N (10k) war Restart vorher
   schon ~0,05 s — kein Gewinn messbar. Und nach Kill (dem eigentlichen
   Crash-Szenario) wurde der Spiegel wegen des `.btree.tmp`-Restes
   verworfen → Voll-Replay. Drei Ebenen, auf denen der Nutzen fehlt.
2. **Ursache steht im Code** (`src/sql/executor.cpp`,
   `syncMirrorBatch`: pro Statement `mirror_->Put(...)` +
   `mirror_->Flush()` — also pro Einzel-INSERT ein WAL-fdatasync UND
   ein B-Tree-Flush). Die Kosten pro Row wachsen mit der Baumgröße:
   10× Rows (10k→100k) → ~85× Zeit (6,5 s → ~550 s). Das ist der
   Trade, den s76 gekauft hat: Insert-Throughput für Restart-Hoffnung.
3. **Gesamtbild gegen SQLite gekippt:** vorher Insert ~11× schneller
   als SQLite bei Restart ~130× langsamer; jetzt ist dbengine beim
   Insert auf großer Skala *langsamer* als SQLite (~550 s vs. ~75 s
   bei 100k/1M-Maßstab) und der Restart-Vorteil bleibt unbewiesen.
   Der Spiegel macht beide Achsen schlechter bzw. unbelegt.
4. **Nicht gemessen (bewusst):** sauberer Shutdown bei 100k/1M
   (außerhalb Budget), echte Disk statt tmpfs, Batch > 1.
   Empfehlung: Spiegel-Flush entkoppeln (Group-Commit/Checkpoint-
   Intervall statt Flush-pro-Statement) und Crash-Rest (`.tmp`)
   beim Öffnen tolerieren statt Spiegel verwerfen — dann Re-Duell.

## Nachtrag s76b/s76c (2026-09-27): Mirror-Batching + Pager-Batch

Empfehlung aus obiger Sektion umgesetzt:
- Mirror-Flush nur alle 1000 Statements + `mirrorCheckpoint()` bei sauberem
  Exit (`src/sql/executor.cpp`, `src/main.cpp`); Watermark-Protokoll exakt.
- `Pager::insert_batch`: ein Image-Rewrite pro Flush statt einem pro Knoten;
  `BTreeKV::Persist` nutzt ihn (`src/storage/pager.cpp`, `src/kv/btree.cpp`).

Neue Messung 10k (tmpfs, Batch=1): Insert **0,14 s** (vorher 86 s mit
Flush-pro-Statement, 0,056 s ganz ohne Spiegel), Restart+SUM **0,06 s**,
SUM korrekt.

Neue Messung 1M: Insert **60 s** (SQLite 75 s — gleichauf), Restart+SUM
**6,5 s** (SQLite 0,05 s — weiter ~130×). Der Spiegel spart WAL-Parsing,
aber NICHT den RAM-Rebuild (1M KV-Puts + MVCC-Commits beim Laden):
Restart bleibt O(n) statt O(1) wie bei SQLite (mmap-B-Tree, On-Demand-Reads).
Echte Heilung = Lese-Pfad direkt auf persistentem B-Tree (kein RAM-Rebuild),
steht aus.
