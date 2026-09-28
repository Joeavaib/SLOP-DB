# Volles TPC-H — Kit, Daten, Dialekt (Stand 09/2026)

Ziel: alle 22 TPC-H-Queries statt nur Q1/Q6-Kernen. Kein Compliance-Claim
(s. docs/FAIR-DUELL.md §5 für die Lückenliste).

## 1. Herkunft (reproduzierbar, kein Vendoring von Binaries)

- Queries: kanonische Templates aus tpch-kit (`dbgen/queries/1..22.sql`,
  TPC-H v2.17.3), substituiert mit offiziellem `qgen -s 1 -r 42`
  (DSS_QUERY-Env, `limit -1`-Suffix wie generiert). Vendored unter
  `tools/tpch/queries/q01..q22.sql` (Seed 42, fixiert und versioniert).
  Build-Hinweis qgen (conda-GCC 16.2): `Makefile`-CFLAGS um
  `-std=gnu11 -Wno-error=incompatible-pointer-types -w` ergänzen
  (C23-`()`-Semantik + Pointer-Warnungen als Errors).
- Daten: DuckDB-`dbgen` (`CALL dbgen(sf=…)`), kanonische Verteilung
  (kein Uniform-Eigenbau wie bisher). Export via
  `tools/tpch/tpchgen.py --sf S --out DIR --manifest`.
- SF0.01-Manifest (`/tmp/tpchdata`, DuckDB 1.5.5): customer 1500,
  lineitem 60175, nation 25, orders 15000, part 2000, partsupp 8000,
  region 5, supplier 100 (SHA256 s. MANIFEST.json).

## 2. Dialekt-Adaptation (dbengine, dokumentiert statt verschwiegen)

| Kanonisch | dbengine | Grund |
| --- | --- | --- |
| `DATE` (+ `date '…'`-Literale, `interval`-Arithmetik) | `INT` yyyymmdd, `BETWEEN a AND b` | kein DATE-Typ (Roadmap) |
| `DECIMAL(15,2)` | `DOUBLE` | kein DECIMAL-Typ (Rundung dokumentiert, Validierung mit Toleranz) |
| `LIMIT n` / `limit -1` | `LIMIT n`, `-1` = kein Limit | qgen-Suffix übernommen |
| `CHAR/VARCHAR` | `TEXT` | vorhanden |

Sprünge über mehrere Tabellen (Multi-Join), `EXISTS`/`NOT EXISTS`,
`HAVING`, Views (Q15) hängen vom Engine-Stand ab → Support-Matrix
(s123) statt Behauptung.

## 3. Repro

```sh
python3 tools/tpch/tpchgen.py --sf 0.01 --out /tmp/tpchdata --manifest
```

## 4. Support-Matrix Baseline (s123, SF0.01, kanonische Queries unverändert)

Loader: `tools/tpch/tpchmatrix.py` (alle 8 Tabellen ok: customer 1500,
lineitem 60175 in 73 s, nation 25, orders 15000 in 42 s, part 2000,
partsupp 8000, region 5, supplier 100; Batches à 200, Quotes escaped).
Runner: jede Query einzeln per CLI, Timeout 120 s, OK/FAIL + ms + Hash.

| Q | Stand | Fehlerklasse |
| --- | --- | --- |
| q01, q04, q06 | FAIL | `date`-Literal (kein DATE-Typ) |
| q03, q05, q10, q11, q18, q21 | FAIL | Spalten+Aggregate ohne (erkanntes) GROUP BY |
| q07, q09, q13, q22 | FAIL | Derived Tables `FROM (SELECT…)` |
| q08, q12, q16 | FAIL | `Erwartet ')'` (CASE/EXTRACT-Verdacht) |
| q14 | FAIL | `Erwartet Identifier` |
| q15 | FAIL | View/DDL (`CREATE VIEW`) |
| q17 | FAIL | `Erwartet Keyword FROM` (EXISTS-Verdacht) |
| q02 | FAIL | `Unbekannte Spalte: s_acctbal` (korreliert/ORDER-BY-Verdacht) |
| q19, q20 | OK (ungeprüft!) | läuft durch — Korrektheit vs. DuckDB noch offen |

Stand: **2/22 laufen durch (Korrektheit ungeprüft), 20/22 scheitern**,
gruppiert in ~7 Fehlerklassen. Nächste Schritte: INT-Date-Adaptation
(q01/q04/q06-Kandidaten), GROUP-BY-Erkennung, CASE, Multi-Join —
jeweils mit Ergebnis-Validierung gegen DuckDB (OK ≠ korrekt).
