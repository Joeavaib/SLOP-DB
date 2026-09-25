# dbengine — Runbook (Build / Test / Bench / Recovery / Env)

## 1. Build

Voraussetzungen: CMake ≥ 3.20, C++20-Compiler (GCC 11+, Clang 14+), POSIX
(Linux). Keine externen Deps (nur STL/POSIX, siehe `SBOM.md`).

```sh
cmake -S . -B build
cmake --build build -j4
./build/dbengine   # dbengine version 0.1.0
```

Hygiene: `-Wall -Wextra -Wpedantic`, `CMAKE_CXX_STANDARD 20`, Extensions OFF.

## 2. Test

```sh
ctest --test-dir build --output-on-failure        # alles
ctest --test-dir build -R "docs|wal|mvcc|kv" --output-on-failure
cmake --build build -j4 && ctest --test-dir build -R docs --output-on-failure  # s20-Verify
```

Suites (Auswahl): `smoke, sql, pager, columnar, index, hybrid, wal, vector,
kv, mvcc, raft, pgserver, clock, jsonb, executor, docs`.

Der `docs`-Test (`tests/test_docs.cpp`) prüft ohne Build-Bruch-Risiko:

- Existenz von `docs/ARCH.md`, `docs/RUNBOOK.md`, `k8s/statefulset.yaml`,
  `SBOM.md` (Fallback `docs/SBOM.md`),
- nicht-leer (>0 Bytes, ARCH ≥ 1 KiB erwartet),
- Markdown-Titel (erste nicht-leere Zeile oder beliebig: `# ...`),
- Stub-Keywords (`replica`/`PVC` bzw. `volumeClaimTemplates` in YAML).

## 3. Bench (Manuell, s18-Tooling noch offen)

Bis `tools/bench.cpp` (s18) landet, manuell messen:

```sh
# WAL-Recovery-Zeit steht im Test-Log:
./build/test_wal 2>&1 | grep recovery_ms
# ANN Recall/Latenz: test_vector / test_hybrid Logs
./build/test_vector && ./build/test_hybrid
```

SLO-Richtwerte (N1): KV Point-Lookup p99 <1ms, SQL-PK <5ms, HNSW Recall@10
>0.9–0.95 p95 <10ms bis 5M Vektoren, Write-Amplification <3x.

## 4. Recovery

WAL ist Single Source of Truth für Crash-Recovery (A3: <30s bei 100GB,
kein Verlust bei Kill -9):

1. `open()` beim Start: `scan()` + Torn-Tail-Cap + `max-LSN`-Bestimmung.
2. `replay()` / `replay_file(path)` anwenden (Prefix-Semantik bei CRC-Fehler).
3. Weiter mit `next_lsn = max+1`; kein Checkpoint beim Start nötig.
4. `checkpoint(lsn)` erst nach durablem Snapshot der State-Machine
   (Pager/Columnar-Flush), crash-sicher via `tmp+rename`.

Manuelle Prüfung:

```sh
./build/test_wal   # 1000 appends, kill-sim, checkpoint-Rest, torn-tail
./build/test_pager # restart persistence single-file
```

Torn-Tail anhängen (Debug): die letzten 7 Bytes mit Müll überschreiben —
`replay` muss Prefix liefern (siehe `test_wal.cpp` Abschnitt 4).

## 5. Env / Config

| Var | Default | Wirkung |
|---|---|---|
| `DBENGINE_DATA_DIR` | `./data` | WAL/Pager-Dateien (V1 single-node) |
| `DBENGINE_WAL_PATH` | `$DATA_DIR/wal.log` | REDO-Log Pfad |
| `DBENGINE_FLUSH_EVERY` | `on-commit` | `append` ohne fsync, `flush()` Group-Commit |
| `DBENGINE_EF_DEFAULT` | `32` | HNSW Beam-Breite (runtime-tunbar per Query) |
| `DBENGINE_PORT` | `5432` | PGServer-Listenport (Tests: ephemeral 127.0.0.1) |
| `TMPDIR` | `/tmp` | Test-Temp-Files (`test_*.wal`, `*.db`) |

K8s (Stub, `k8s/statefulset.yaml`): 1 Replica, PVC `data` 10Gi, Image
Platzhalter `dbengine:0.1.0`, Port 5432, kein Operator in V1 (s20-Doku-Stufe).
Produktiv: RF=3 + `LOCAL_QUORUM`, RPO 0, RTO <30s (lokal <3s) erst mit
Netzwerk-Raft (nach V1).

## 6. Troubleshooting

- `cmake -S . -B build` schlägt fehl → CMake ≥3.20 + GCC 11+/Clang 14+ prüfen.
- `test_docs` FAIL „missing“ → aus Repo-Root bauen (`-S .`), nicht aus `build/`
  (Pfade via `DBENGINE_SOURCE_DIR` verdrahtet).
- WAL-CRC-Fehler im Log → Torn-Tail ist normal nach Kill -9, kein Bug solange
  Prefix-Replay greift.
- Pager `uses_mmap=no` → `pread`-Fallback aktiv (Container ohne mmap), funktional ok.
