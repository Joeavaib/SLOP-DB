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
