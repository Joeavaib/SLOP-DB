# Netsandbox-Duell — Dummy-App unter L2/L3-Partition/Heal (Stand 09/2026)

Kein Sieg über SQLite/DuckDB behauptet. Nur gemessen, nichts extrapoliert.
Alle `/tmp` = tmpfs (fdatasync fast gratis, nicht auf Platte übertragbar).

## 1. Methodik (ehrlich, reproduzierbar)

- Sandbox: `tools/netsandbox.cpp` (s101) — L2-Switch (MAC-Lerntable,
  Unicast/Broadcast, Partition/Heal pro Port-Paar) + L3-Router (10.0.1.0/24
  App, 10.0.2.0/24 DB, Subnetz-Isolation). In-process, STL-only, deterministisch,
  kein Kernel-netns/veth/root (bewusst: SBOM bleibt STL/POSIX-only).
- Dummy-App: `tools/dummy_app.cpp` (s102) — jede DB-Op = genau ein L3-Paket
  app→db. Mix: 2000 KV-Puts (KVStore) + CREATE + 500 SQL-INSERTs + SUM
  (Executor auf eigenem KV/MVCC) + 500 ANN-Vektoren d=16 + 100 Queries
  (HnswIndex). Chaos: 50 Pakete unter L2-Cut (müssen droppen), 50 unter
  L3-Isolation (müssen droppen), Heal → 1 Paket ok + Daten intakt.
- Befehle (dieser Lauf, Repo-Root):
  `cmake --build build -j4`, `./build/netsandbox --selfcheck`,
  `./build/dummy_app --selfcheck`, `./build/dbbench --smoke`.
- SQLite: in dieser Env kein `sqlite3`-Binary (`which sqlite3` leer) → kein
  neuer SQLite-Lauf. Speed-Urteil nutzt die bestehenden Duelle
  (`docs/PHASE0-DUELL.md`, `docs/TPC-H-REMATCH.md`, `README.md:38-74`).

## 2. Zahlen (dieser Lauf)

netsandbox `--selfcheck` (12/12 PASS):
l2/unicast-learn, broadcast-fanout, partition-blocks, heal-restores,
l3/intra-subnet, cross-subnet, subnet-isolate-blocks, intra-survives-isolate,
l2-cut-breaks-l3, heal-restores, stats-monoton, stats-exact (L3 6 sent /
4 routed / 2 dropped).

dummy_app `--selfcheck` (10/10 PASS):
kv-no-drop-when-healed, kv-readable, sql-create, sql-no-drop-when-healed,
sql-sum, ann-all-hit, partition-drops (50/50), subnet-isolate-drops (50/50),
heal-restores, no-loss-after-heal.
Ops (Run-Rauschen, 1 Lauf):
kv_puts ~2,5e+06 ops/s p95 ~0,36 µs (in-memory, ohne fsync),
sql_inserts ~2,0e+05 ops/s p95 ~7 µs,
ann_queries p95 ~9,8 µs; Netz 2703 sent / 2603 ok / 100 dropped (nur Chaos).

dbbench `--smoke` (Referenz, gleiche Maschine):
kv_puts ~2,79e+06 ops/s, sql_q1 ~1,5e+09 rows/s, ann ~51k q/s p95 0,021 ms.

## 3. Urteil (urteilsneutral, wichtig für Ziel „bestehende DBs schlagen“)

- Stabilität in Sandbox: belegt — Partition/Heal verhalten sich wie
  spezifiziert (Drops gezählt, kein Phantom-Erfolg, kein Datenverlust nach
  Heal). Das ist ein Testharnisch-Sieg, kein Produktions-Sieg.
- Speed vs. bestehende DBs: NICHT geschlagen. Referenz aus Rematch 2026-09-27
  (N=1M): Q6 dbengine ~1015–1035 ms vs. SQLite ~73 ms (~14×) vs. DuckDB
  ~3 ms (~330×); Q1 ~1,8 s vs. 0,79 s (~2,3×) vs. 3,4 ms (~530×).
  Insert-Duell 1M: dbengine schneller (fdatasync-Batch-Artefakt auf tmpfs),
  Restart/Replay ~130× langsamer. Details s. PHASE0-DUELL/TPC-H-REMATCH.
- Lücke zum Ziel: OLAP-Scan (vektorisiert, 5 GB/s/Core), echte
  Raft-Replikation mit Failover <3 s, WAL-unabhängige Persistenz und
  Jepsen/Partition-Tests fehlen. Sandbox ist Schritt 1 (Last + Netz
  deterministisch), kein Ersatz für Jepsen, k8s-HA oder NVMe-Bench.

## 4. Repro

```sh
cmake --build build -j4
./build/netsandbox --selfcheck
./build/dummy_app --selfcheck
ctest --test-dir build -R bench --output-on-failure
```
