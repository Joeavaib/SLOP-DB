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
