# Fairer Drei-Wege-Benchmark — Methodik + Ergebnisse (Stand 09/2026)

Kein TPC-H-Compliance-Anspruch. Nur gemessen, nichts extrapoliert.
Alle Pfade `/tmp` = tmpfs (16 GiB, 24 Cores, 31 GiB RAM).

## 1. Was die Recherche ergab (und was wir davon umsetzen)

**TPC-H (Spec v2/v3, dbgen, DuckDB-power-test):** Compliant sind nur
SF 1/10/100/..., QphH aus Power-Test (Single-Stream) + Throughput-Test
(Multi-Stream), qgen-Substitutions-Parameter pro Lauf (nicht 5× dieselbe
Query), Validierung gegen Qualifikations-DB (Ergebnisse müssen stimmen),
2 Runs, Refresh-Funktionen. Fazit für uns: voll compliant sind wir nicht
(2 Queries statt 22, fixe Prädikate, kein Refresh, kein Multi-Stream).
Übernommen: **byte-identische Daten für alle Engines, identische Queries,
2 Läufe-Gedanke (hier: 1 Warmup + 5 Reps), Median UND p95, Ergebnis-
Validierung (Summen/Gruppen müssen übereinstimmen), Load-Zeiten getrennt.**

**SQLite (speedtest1.c, Doku):** Die Fairness-Hebel sind `synchronous`
(FULL vs. NORMAL/OFF = fsync pro Commit oder nicht), `journal_mode`
(delete vs. WAL), `cache_size` (Seiten!), Transaktions-Schnitt (einzeln
vs. eine Transaktion). SQLite misst intern per Cachegrind (Wall-Clock
kaum 1 signifikante Stelle). Übernommen: **eine Transaktion +
executemany für alle (gleicher Schnitt), PRAGMAs berichtet, Median/p95.**

**Generell:** fsync-Verhalten, Thread-Zahl und DB-Größen offenlegen;
keine Extrapolation über Skalen.

## 2. Umsetzung (`tools/fairbench.py`)

- Datensatz: exakt EIN Generator für alle — Python-Replikation von
  `std::mt19937(42)` + libstdc++-Lemire-`uniform_int_distribution`
  (empirisch gegen C++-Probe Byte-für-Byte verifiziert: 3/3 Probe-Rows
  identisch; Summen-Validierung 10k/100k/1M gegen dbbench-Referenz).
- dbengine-Zahlen via `./build/dbbench --tpch N` (In-Prozess-Timing,
  intern kReps=5; kein separater Warmup — 1. Rep ist de facto warm).
- SQLite 3.51.2 (stdlib): Datei-DB, 1 Transaktion, `executemany`,
  Default-Pragmas (s. u.).
- DuckDB 1.5.5 (pip): Datei-DB, `executemany` (gleiche Methode wie
  bisher; Load-Zeit als Artefakt berichtet, nicht als Urteil).
- Je Query: 1 Warmup (verworfen) + 5 Reps → Median + p95.

## 3. Ergebnisse 100k (fairbench 2026-09-28)

| Query | dbengine (dbbench p95) | SQLite Median / p95 | DuckDB Median / p95 |
| --- | --- | --- | --- |
| Q6 | 63,16 ms | 7,45 / 8,12 ms (~8×) | 1,55 / 1,87 ms (~34×) |
| Q1 | 86,35 ms | 70,17 / 71,21 ms (~1,2×) | 3,26 / 3,53 ms (~24×) |

Validierung: Q6-Summe 180650,160300 auf allen drei (rel < 1e-9),
Q1 6 Gruppen + counted=100000 überall.
Offenlegung: SQLite `journal_mode=delete, synchronous=FULL(2),
cache_size=-2000 (2 MiB), page_size=4096`; DuckDB Threads=24,
memory_limit=25 GiB; DB-Größen SQLite 4,7 MB / DuckDB 0,8 MB;
Load-Zeiten: gen 1,0 s, dbbench total 1,6 s, SQLite 0,1 s,
DuckDB executemany 37,2 s (Methoden-Artefakt, row-by-row).

## 4. Ergebnisse 1M

(TBD — Lauf `fair_1M.json` nach Abschluss hier eintragen.)

## 5. Ehrliche Lücken (kein Compliance-Claim)

1M-Laufkosten DuckDB-executemany (~6 min), kein qgen (fixe Prädikate),
kein Throughput-Test (nur Single-Stream), 2 Queries statt 22, kein
Refresh, tmpfs statt Platte, dbengine ohne separaten Warmup. Diese Datei
ersetzt nicht TPC-H-REMATCH.md (Historie), sondern dokumentiert die
fairste verfügbare Methode + aktuelle Zahlen.
