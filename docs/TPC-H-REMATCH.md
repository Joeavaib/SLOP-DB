# TPC-H-Rematch 1M nach Scan-Replika: dbengine vs. SQLite vs. DuckDB

Datum: 2026-09-27 (UTC). Repo unverändert, nur diese Datei neu.
Kein cmake/ctest/build; dbengine-Binary `./build/dbbench` wie es lag.

## Exakte Queries (aus `tools/bench.cpp` `RunTpch`, keine Abwandlung)

```sql
-- Q6 (bench.cpp:420-423): 5x AND / 6 Range-Praedikate
SELECT SUM(price*disc) FROM lineitem WHERE disc BETWEEN 0.05 AND 0.07
  AND qty < 24 AND price >= 500.0 AND price < 5000.0 AND tax <= 0.05
  AND shipdate BETWEEN 19940101 AND 19951231;
-- Q1-Kern (bench.cpp:464-466): Hash-Agg + GROUP BY rf, ls
SELECT rf, ls, SUM(qty), SUM(price), SUM(price*disc), AVG(disc), COUNT(*)
  FROM lineitem GROUP BY rf, ls ORDER BY rf, ls;
```

Generator (`bench.cpp:374-412`): **`std::mt19937 rng(42u)` — Seed 42
(nicht 41)**, je Row in dieser Reihenfolge: `qty(1,50)`,
`price_cents(1000,1000000)/100`, `disc_pct(0,10)/100`, `tax_pct(0,8)/100`,
`rf∈{A,N,R}`, `ls∈{O,F}`, `yr(1992,1998)`, `mo(1,12)`, `da(1,28)`,
`shipdate=yr*10000+mo*100+da`, `orderkey=idx+1`, Batched-INSERTs à 500.
`uniform_int_distribution` = libstdc++-Lemire-Pfad (conda-GCC 16.2).

## Methodik

- dbengine: erst `--tpch 100000` gemessen (2,400 s total) → Hochrechnung
  ~24 s für 1M, klar unter 10 min → **1M ehrlich gemessen, 2 Läufe**
  (`./build/dbbench --tpch 1000000`, je Query `kReps=5`, p95/Scan).
- SQLite (stdlib, `sqlite_version 3.51.2`) + DuckDB 1.5.5 (pip):
  **byte-identische Daten** — Python-Replikation des C++-MT19937 +
  Lemire-`uniform_int_distribution`, validiert ueber die dbbench-Referenzsummen:
  1k → 782,775000, 100k → 180650,160300, 1M → 1783190,382600
  (alle exakt auf 6 Nachkommastellen). Semantisch identische Queries
  (Strings oben), je 2 Läufe à 5 Reps (p95/Scan wie dbbench).
  SQLite: Datei-DB `/tmp` (tmpfs), 1 Transaktion, `executemany`,
  Default-Pragmas. DuckDB: Datei-DB `/tmp`, `executemany`.
- Korrektheit: Q6-Summe **1783190,382600 auf allen drei Engines**
  (beide Läufe), Q1: 6 Gruppen, `counted = 1000000` überall;
  Gruppen-`SUM(qty)`/`COUNT(*)` exakt gleich, `SUM(price)`-DOUBLEs
  bis ~1e-9 identisch (Summationsreihenfolge). dbengine-intern:
  Q6-Hash `f0bd39a16c88de01`, Q1-Hash `f02f69cd1e273cb0` (beide Läufe).
- Artefakte: alles unter `/tmp`, danach DB-Files + Skripte gelöscht.

## Ergebnisse 1M (p95 ms/Scan; Throughput = rows/s wie dbbench-CSV)

| Query | dbengine Lauf A | dbengine Lauf B | SQLite L1 / L2 | DuckDB 1.5.5 L1 / L2 |
| --- | --- | --- | --- | --- |
| Q6 | 1094,39 ms / 892432 | 1051,34 ms / 953456 | 75,34 ms / 73,59 ms | 3,09 ms / 3,27 ms |
| Q1 | 1920,90 ms / 510438 | 1974,27 ms / 500460 | 887,87 ms / 885,63 ms | 4,00 ms / 4,19 ms |

dbengine-Gesamtzeit 1M (Aufbau + je 5 Scans): 23,55 s / 23,32 s.
100k-Probe: Q6 p95 137,61 ms, Q1 p95 191,11 ms (nahezu lineare Skalierung ×10).
Build-Zeiten Konkurrenz (kein Urteilskriterium, nur Doku):
SQLite 0,83/0,82 s (47,7 MB DB), DuckDB ~357/359 s
(`executemany` row-by-row = Methoden-Artefakt, ~10 MB DB).

### Vorher/nachher/seitlich

| Kennzahl (1M) | vorher (pre-Replika) | nachher (Rematch, Scan-Replika) | seitlich |
| --- | --- | --- | --- |
| Q6 p95 | **nicht dokumentiert** (s58/s68–s71 ohne 1M-Timing im Repo) | ~1050–1095 ms | SQLite ~74 ms, DuckDB ~3,2 ms |
| Q1 p95 | **nicht dokumentiert** (s. o.) | ~1920–1975 ms | SQLite ~887 ms, DuckDB ~4,1 ms |

## Urteil (ungeschönt)

1. **Replika-Effekt: nein — aus diesem Rematch nicht belegbar, und die
   absoluten Zahlen sprechen gegen einen Durchbruch.** Es existieren keine
   dokumentierten Pre-Replika-1M-Zahlen für Q1/Q6, also ist kein
   vorher/nachher-Vergleich möglich (oben ehrlich als Lücke markiert
   statt extrapoliert). Was messbar ist: der seitliche Abstand.
2. **Wo wir stehen: Q6 ~14× hinter SQLite und ~340× hinter DuckDB;
   Q1 ~2,2× hinter SQLite und ~480× hinter DuckDB.**
   Q1 (Hash-Agg über Voll-Scan) liegt relativ nahe an SQLite — dort
   wirkt der Replika-Pfad (kein KV-Snapshot/MVCC-Read pro Zeile) plausibel —,
   aber Q6 (selektiver Filter-Scan) bleibt 14× zurück, und DuckDBs
   vektorisiertes Columnar-Format spielt in einer anderen Liga (3–4 ms).
3. **Korrektheit steht:** identische Q6-Summe (6 Nachkommastellen) und
   identische Q1-Gruppen auf allen Engines — der Rematch vergleicht
   wirklich dasselbe Ergebnis, nur unterschiedlich schnell.
4. **Nicht gemessen (bewusst):** Pre-Replika-Baseline auf gleichem Stand
   (würde den Replika-Effekt isolieren), echte Disk statt tmpfs,
   Spalten- vs. Zeilenprojektions-Details, parallele Scans.
   Empfehlung: Baseline mit abgeschalteter Replika (`replica_`-Pfad
   umgehen) auf 1M nachholen — dann ist vorher/nachher eine Zahl
   statt einer Lücke.

---

## Rematch 2026-09-27 (UTC) nach Scan-Replika — N=100000 + N=1000000, je 2 Läufe

Datum: 2026-09-27 (UTC). Nur diese Datei angehängt, Bestand unverändert.
Kein cmake/ctest/build; `./build/dbbench` wie es lag.

Exakte Queries/Generator aus `tools/bench.cpp` `RunTpch` (keine Abwandlung):
Q6 = `SELECT SUM(price*disc) ... WHERE disc BETWEEN 0.05 AND 0.07 AND qty < 24
AND price >= 500.0 AND price < 5000.0 AND tax <= 0.05
AND shipdate BETWEEN 19940101 AND 19951231` (bench.cpp:420-423);
Q1-Kern = `SELECT rf, ls, SUM(qty), SUM(price), SUM(price*disc), AVG(disc),
COUNT(*) ... GROUP BY rf, ls ORDER BY rf, ls` (bench.cpp:464-466).
Generator bench.cpp:374-412: `std::mt19937 rng(42u)`, je Row
`qty(1,50)`, `price_cents(1000,1000000)/100`, `disc_pct(0,10)/100`,
`tax_pct(0,8)/100`, `rf(0,2)`, `ls(0,1)`, `yr(1992,1998)`, `mo(1,12)`,
`da(1,28)`, libstdc++-Lemire-`uniform_int_distribution` (conda-GCC 16.2).

Methodik: `./build/dbbench --tpch 100000` (2 Läufe, total ~2,08 s) →
Hochrechnung ~21 s für 1M < 10 min → `./build/dbbench --tpch 1000000`
ehrlich gemessen, 2 Läufe (total 22,225 s / 22,460 s; je Query kReps=5,
p95/Scan). SQLite stdlib 3.51.2 + DuckDB 1.5.5: Python-Replikation
MT19937(42)+Lemire, byte-identisch verifiziert über dbbench-Referenzsummen
(100k: py 180650,160300 = dbbench ref; 1M: py 1783190,382600 = dbbench ref;
1k: 782,775000 wie s84). Semantisch identische Queries oben, je 2 Läufe
à 5 Reps (p95/Scan, throughput=rows*5/total wie dbbench). SQLite Datei-DB
/tmp, 1 Transaktion, executemany, Default-Pragmas. DuckDB Datei-DB /tmp,
executemany. Artefakte danach gelöscht.

### dbengine (./build/dbbench --tpch N)

- N=100000 Lauf1: Q6 sum=180650,160300 ref=gleich hash=c5703609095017c7
  thr=979904,8 rows/s p95=102,9247 ms; Q1 groups=6 counted=100000
  hash=4625bd4e25bdc907 thr=605825,1 p95=163,4316 ms.
- N=100000 Lauf2: Q6 thr=983122,4 p95=104,4496 ms (sum/hash gleich);
  Q1 thr=619903,9 p95=154,3886 ms (groups/counted/hash gleich).
- N=1000000 Lauf A: Q6 sum=1783190,382600 ref=gleich hash=f0bd39a16c88de01
  thr=975500,2 p95=1034,6679 ms; Q1 groups=6 counted=1000000
  hash=f02f69cd1e273cb0 thr=544485,5 p95=1768,7437 ms.
- N=1000000 Lauf B: Q6 thr=988745,0 p95=1015,3886 ms (sum/hash gleich);
  Q1 thr=526077,3 p95=1847,5373 ms (groups/counted/hash gleich).

### Konkurrenz, gleiche Daten/Queries

- N=100000 SQLite: Q6 L1 thr=14438292,8 p95=6,91 ms / L2 thr=13636266,4
  p95=7,63 ms, Summe 180650,16030000002; Q1 L1 thr=1475874,6 p95=69,27 ms
  / L2 thr=1502157,7 p95=66,60 ms, 6 Gruppen, counted=100000.
- N=100000 DuckDB (build 36,1 s executemany-Artefakt): Q6 L1
  thr=37580543,5 p95=2,00 ms / L2 thr=66732465,0 p95=1,56 ms;
  Q1 L1 thr=31115894,4 p95=3,38 ms / L2 thr=33726815,1 p95=3,20 ms.
- N=1000000 SQLite (build 0,82 s): Q6 L1 thr=13835818,2 p95=72,50 ms /
  L2 thr=13919251,7 p95=73,21 ms, Summe 1783190,3826000001;
  Q1 L1 thr=1277629,7 p95=793,38 ms / L2 thr=1273011,8 p95=790,11 ms,
  6 Gruppen, counted=1000000.
- N=1000000 DuckDB (build 352,1 s executemany-Artefakt): Q6 L1
  thr=339853836,6 p95=3,13 ms / L2 thr=355865224,5 p95=2,95 ms,
  Summe 1783190,3826000006; Q1 L1 thr=304707282,4 p95=3,34 ms /
  L2 thr=305632065,0 p95=3,48 ms. Q1-Gruppen SUM(qty)/COUNT exakt gleich,
  SUM(price)-DOUBLEs bis ~1e-9 identisch.

### Tabelle N=1000000 (rows/s + p95 ms/Scan)

| Engine | Q6 Lauf A | Q6 Lauf B | Q1 Lauf A | Q1 Lauf B |
| --- | --- | --- | --- | --- |
| dbengine | 975500 rows/s / 1034,67 ms | 988745 / 1015,39 ms | 544486 / 1768,74 ms | 526077 / 1847,54 ms |
| SQLite 3.51.2 | 13835818 / 72,50 ms | 13919252 / 73,21 ms | 1277630 / 793,38 ms | 1273012 / 790,11 ms |
| DuckDB 1.5.5 | 339853837 / 3,13 ms | 355865225 / 2,95 ms | 304707282 / 3,34 ms | 305632065 / 3,48 ms |

### Tabelle N=100000 (schnell-Probe)

| Engine | Q6 L1 | Q6 L2 | Q1 L1 | Q1 L2 |
| --- | --- | --- | --- | --- |
| dbengine | 979905 / 102,92 ms | 983122 / 104,45 ms | 605825 / 163,43 ms | 619904 / 154,39 ms |
| SQLite | 14438293 / 6,91 ms | 13636266 / 7,63 ms | 1475875 / 69,27 ms | 1502158 / 66,60 ms |
| DuckDB | 37580544 / 2,00 ms | 66732465 / 1,56 ms | 31115894 / 3,38 ms | 33726815 / 3,20 ms |

### Vorher/nachher s84 (1M)

| Kennzahl 1M p95 | s84 (vorher, Scan-Replika) | jetzt (nachher) | Delta |
| --- | --- | --- | --- |
| dbengine Q6 | 1094,39 / 1051,34 ms | 1034,67 / 1015,39 ms | ~-3 bis -5% (Run-Rauschen) |
| dbengine Q1 | 1920,90 / 1974,27 ms | 1768,74 / 1847,54 ms | ~-6 bis -8% (Run-Rauschen) |
| SQLite Q6 | 75,34 / 73,59 ms | 72,50 / 73,21 ms | ~gleich |
| SQLite Q1 | 887,87 / 885,63 ms | 793,38 / 790,11 ms | ~-11% (tmpfs/Version-Rauschen) |
| DuckDB Q6 | 3,09 / 3,27 ms | 3,13 / 2,95 ms | gleich |
| DuckDB Q1 | 4,00 / 4,19 ms | 3,34 / 3,48 ms | ~gleich |

Korrektheit: Q6-Summe 1M auf allen drei Engines 1783190,382600
(±1e-9 Summationsreihenfolge), Q1 6 Gruppen + counted=1000000 überall;
dbengine-Hashes stabil: Q6 f0bd39a16c88de01, Q1 f02f69cd1e273cb0 (beide Läufe).

### Urteil (Replika-Effekt messbar?)

Nein. dbengine 1M liegt innerhalb weniger Prozent der s84-Zahlen
(Q6 ~1015–1035 vs. 1051–1094 ms; Q1 ~1769–1848 vs. 1921–1974 ms) —
das ist Lauf-zu-Lauf-Rauschen auf diesem Host, kein Durchbruch.
Seitlicher Abstand unverändert: Q6 ~14× hinter SQLite, ~330× hinter
DuckDB; Q1 ~2,3× hinter SQLite, ~530× hinter DuckDB. Skalierung 100k→1M
nahezu linear (dbengine Q6 ×10, Q1 ×11). Der Scan-Replika-Pfad liefert
in diesem Rematch keinen belegbaren Sprung; für einen isolierten
vorher/nachher-Nachweis braucht es weiter die Baseline mit
abgeschalteter Replika auf gleichem Stand.

## Rematch s108 (nach s105 WHERE-Index-Cache, s106 from_chars-Decode, s107 Agg-Index)

Stand: 2026-09-28, gleiche Maschine/tmpfs, je 1 Lauf à 5 Reps (kein 2. Lauf —
Einzellauf-Rauschen beachten, q6bench-Schwankung 55–90 ms beobachtet).
Befehl: `./build/dbbench --tpch 100000` / `--tpch 1000000`.

| N | Query | dbengine p95 (s108) | s84-Stand | Delta |
| --- | --- | --- | --- | --- |
| 100k | Q6 | 56,86–66,09 ms (2 Läufe) | 102,92 / 104,45 ms | ~-40% (echt, über Rauschen) |
| 100k | Q1 | 84,56 / 95,10 ms | 163,43 / 154,39 ms | ~-42% (echt) |
| 1M | Q6 | 791,56 ms | 1034,67 / 1015,39 ms | ~-23% (echt) |
| 1M | Q1 | 1335,91 ms | 1768,74 / 1847,54 ms | ~-25% (echt) |

Korrektheit: Summen/Hashes **byte-identisch** zu s84 (100k Q6
`c5703609095017c7` sum=180650,160300; Q1 `4625bd4e25bdc907` groups=6
counted=100000; 1M Q6 `f0bd39a16c88de01` sum=1783190,382600; Q1
`f02f69cd1e273cb0` groups=6 counted=1000000). Die drei Optimierungen
ändern nur Stringsuche/Parsing-Overhead (WHERE-colIndex, stoll/stod-Tmp,
Agg-colIndex), keine Semantik.

Urteil: Erster echter Sprung seit s84 (kein Rauschen: Richtung konsistent
über Q1/Q6 × 100k/1M, Größenordnung passt zu q6bench 111→55 ms).
Abstand bleibt: Q6 ~11× hinter SQLite (~73 ms), ~260× hinter DuckDB (~3 ms);
Q1 ~1,7× hinter SQLite, ~390× hinter DuckDB. Nächster dominanter Rest:
Snapshot-Materialisierung + Filter-Dispatch (s. Welle 25+).
