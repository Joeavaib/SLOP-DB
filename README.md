# dbengine — Embedded-Engine-Prototyp (kein Produktionsersatz)

Embedded SQL-Engine-Prototyp in C++20 (STL/POSIX-only, Apache-2.0): Row-KV + MVCC + WAL,
B-Tree-Spiegel, Columnar- und Vektor-Pfade in einer Binary. Zum Lernen/Experimentieren —
**kein Ersatz für SQLite/DuckDB/Postgres** (Restart braucht WAL-Replay, Daten ohne WAL
verloren, kein HA/2PC).

## Quickstart

Voraussetzungen: CMake >= 3.20, C++20-Compiler (GCC 11+, Clang 14+).

```sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/dbengine foo.db --exec "CREATE TABLE t (id INT, val INT); INSERT INTO t VALUES (1, 1); SELECT SUM(val) FROM t;"
```

REPL: `./build/dbengine foo.db` (Ende mit `.quit`), Batch: `--sql init.sql --exec "..."`,
`--help` / `--version` (`dbengine version 0.1.0`). Dateien nebeneinander: `foo.db`
(Container, 41 038 Bytes, keine Rows), `foo.db.wal` (Wahrheit), `foo.db.btree` (Spiegel, tolerant).

## Feature-Matrix (Stand 09/2026)

| Bereich | Stand | Ein Satz |
| --- | --- | --- |
| SQL Basis (CREATE/INSERT/SELECT, UPDATE/DELETE/DROP, 1× INNER JOIN) | 🟢 | Equi-Hash-Join bzw. Nested-Loop mit `ambiguous`-Check läuft über Snapshot-Scan. |
| Filter/Aggregate (WHERE BETWEEN/IN/LIKE, SUM/AVG/MIN/MAX/COUNT, GROUP BY, ORDER BY/LIMIT, Subqueries unkorreliert Tiefe ≤ 8) | 🟢 | Q1/Q6-Kerne inkl. Hash-Agg sind messbar korrekt (gleiche Summen wie SQLite/DuckDB). |
| Txn (MVCC Snapshot + ReadCommitted, Single-Writer, TSO/HLC) | 🟢 | Kein SSI/Serializable, kein 2PC — bewusst offen. |
| Storage (WAL REDO + fdatasync-Flush, Pager 16 KiB, BTreeKV Fanout 64, COL2-Codec) | 🟢 | Regel: nur nach `flush()` Kill-9-sicher, Torn-Tail → Prefix gewinnt. |
| CLI/Mirror (REPL/Batch, B-Tree-Sidecar, Flush alle 1000 + Exit-Checkpoint) | 🟡 | Restart spart WAL-Parsing, aber RAM-Rebuild bleibt O(n); nach Kill mit `.btree.tmp`-Rest fällt er auf Voll-Replay zurück. |
| OLAP (Columnar Parts, Scan-Replika, Parallel-Scan) | 🟡 | Replika-Effekt im Rematch nicht belegbar (innerhalb Run-Rauschen), Q6 bleibt ~14× hinter SQLite. |
| Vektor/Suche (HNSW multi + PQ-N/IVF + Autotune, BM25-Hybrid, DiskANN-lite) | 🟡 | Funktional inkl. Recall-Formeln, aber kein Produktions-Tuning gegen FAISS & Co. behauptet. |
| Server/Ops (PGWire-Session, Metrics/Prometheus, Backup offline, Prodsim, pg_stat-light) | 🟡 | Auth/SCRAM + TLS nur opt-in (Default Trust-All/Klartext), Backup ohne PITR/Incremental. |
| Cluster/HA (Raft-3er-Sim, Timer-Tick, TCP-Loopback-Codec) | 🔴 | Kein echtes Raft-over-TCP, kein Auto-Split mit Daten-Move, kein k8s-HA/Jepsen. |
| Persistenz ohne WAL | 🔴 | `<db>` ohne `.wal` ist leer, WAL-Verlust = Totalverlust (keine eingebaute Replikation). |

## Benchmarks — nur gemessen, nichts extrapoliert (alle `/tmp` = tmpfs)

Setup Duell: `CREATE TABLE t (id INT, val INT)` + `INSERT (i,i)` Batch=1 (1 Statement/Row,
dbengine 1 fdatasync/INSERT, SQLite Autocommit `synchronous=FULL`), Quelle `docs/PHASE0-DUELL.md`.

| N | Kennzahl | dbengine | SQLite | Quelle |
| --- | --- | --- | --- | --- |
| 10k | Insert | 0,056 s / 0,057 s (2 Läufe) | 0,753 s | PHASE0-DUELL Z.13–16 |
| 10k | Restart/Reopen + Scan | 0,054 s | 0,001 s | PHASE0-DUELL Z.13–16 |
| 1M | Insert (38 777 826-B-SQL, je 2 Läufe) | 6,681 s / 6,705 s | 75,086 s / 75,460 s (ca. 11×) | PHASE0-DUELL Z.67–81 |
| 1M | Restart + Replay + SUM / Reopen + COUNT + SUM | 6,120/6,098 s, 6,230/6,301 s | 0,048/0,045 s, 0,048/0,044 s (ca. 130×) | PHASE0-DUELL Z.67–81 |
| 1M | Persistente Bytes | 41 038 B Container + 53 666 726 B (~51,2 MiB) WAL | 15 024 128 B (14,33 MiB) DB | PHASE0-DUELL Z.72–83 |
| 10k | Insert mit Spiegel (Flush-pro-Statement) | 6,542 s / 6,509 s (ca. 117× langsamer) | — | PHASE0-DUELL Z.160–165 |
| 10k | Dateien mit Spiegel | `.wal` 476 720 B, `.btree` 344 064 B | — | PHASE0-DUELL Z.167–168 |
| 10k | Restart + SUM mit Spiegel | 0,058 s / 0,057 s (kein Gewinn) | — | PHASE0-DUELL Z.172–176 |
| ca. 99k | Restart + SUM nach Kill (98 681 Rows, Voll-Replay-Warnung) | 0,590 s / 0,590 s | — | PHASE0-DUELL Z.182–189 |
| 10k | Insert Mirror-Batching (Flush alle 1000) | 0,14 s | — | PHASE0-DUELL Z.236–238 |
| 1M | Insert Mirror-Batching / Restart + SUM | 60 s / 6,5 s | 75 s / 0,05 s | PHASE0-DUELL Z.240–244 |

Setup TPC-H: `./build/dbbench --tpch N` (Seed 42, Batched-INSERTs à 500, je Query 5 Reps,
p95/Scan), SQLite 3.51.2 + DuckDB 1.5.5 byte-identisch, Q6-Summe 1M = 1783190,382600 überall,
Quelle `docs/TPC-H-REMATCH.md` Rematch 2026-09-27 (N=1M total 22,225 s / 22,460 s).

| N | Query | dbengine p95 | SQLite p95 | DuckDB p95 |
| --- | --- | --- | --- | --- |
| 1M | Q6 (Lauf A / B) | 1034,67 ms / 1015,39 ms | 72,50 ms / 73,21 ms (ca. 14×) | 3,13 ms / 2,95 ms (ca. 330×) |
| 1M | Q1 (Lauf A / B) | 1768,74 ms / 1847,54 ms | 793,38 ms / 790,11 ms (ca. 2,3×) | 3,34 ms / 3,48 ms (ca. 530×) |
| 100k | Q6 (L1 / L2) | 102,92 ms / 104,45 ms | 6,91 ms / 7,63 ms | 2,00 ms / 1,56 ms |
| 100k | Q1 (L1 / L2) | 163,43 ms / 154,39 ms | 69,27 ms / 66,60 ms | 3,38 ms / 3,20 ms |

Älterer Stand s84 zum Vergleich (gleiche Quelle, Z.164–173): Q6 1094,39/1051,34 ms,
Q1 1920,90/1974,27 ms — Delta zum Rematch nur Run-Rauschen, kein belegter Sprung.

Caveats: tmpfs macht fdatasync fast gratis (nicht auf Platte übertragbar); dbengine-SUM
zeigt `5e+11`-Format statt `500000500000` (Display, korrekt gerechnet); SQLite-Q6/Q1-Scans
in obigen Duell-Zahlen teils 2 Voll-Scans in einer Messung (Asymmetrie dokumentiert,
urteilsneutral).

## Docs

- `docs/ARCH.md` — Schichten, WAL/MVCC/Shard, V2- und Wellen-Kapitel (Kap. 1–12)
- `docs/PHASE0-DUELL.md` — 1M-Duell + Mirror-Re-Duell (Methodik, Abbrüche, Urteil)
- `docs/TPC-H-REMATCH.md` — Q1/Q6-Rematch 100k + 1M vs. SQLite/DuckDB
- `docs/RUNBOOK.md` — Betrieb (u. a. TLS-Sidecar Kap. 7)
- `docs/PRODSIM.md` — Prodsim-Szenarien A–H + Cluster-Tool
- `docs/SBOM.md` — Abhängigkeiten (STL/POSIX-only)

## Lizenz

Apache-2.0, siehe `LICENSE`.
