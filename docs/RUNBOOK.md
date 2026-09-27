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
Kill-9-sicher nur nach `flush()`; ungeflushte `append`s gehen verloren —
per Design, siehe `wal.h`):

1. `open()` beim Start: `scan()` + Torn-Tail-Cap + `max-LSN`-Bestimmung.
2. `replay()` / `replay_file(path)` anwenden (Prefix-Semantik bei CRC-Fehler).
3. Weiter mit `next_lsn = max+1`; kein Checkpoint beim Start nötig.
4. `checkpoint(lsn)` erst nach durablem Snapshot der State-Machine
   (Pager/Columnar-Flush), crash-sicher via `tmp+rename`.

Persistenzgrenzen: Columnar aktiver Part erst nach `Seal`/`Save` dauerhaft
(nach s27 inkl. Active-Save); Raft nur nach explizitem `SaveLog`/`SaveSnapshot`
bzw. Opt-in-Autosave dauerhaft, sonst in-memory — kein Auto-Persist bei `append`.

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

## 7. TLS-Sidecar (stunnel, optional)

Der Server selbst spricht Klartext (Trust / Proxy-Vertrauen): TLS terminiert
im Sidecar, der Server vertraut Connections von `127.0.0.1` / dem Sidecar
weiter als Trust (kein eigenes TLS im Server in V1). Beispiel `stunnel.conf`:

```ini
[postgres]
accept = 5433
connect = 5432
cert = /etc/stunnel/server.pem
key = /etc/stunnel/server.key
```

Test via `psql` mit Pflicht-TLS:

```sh
psql "host=127.0.0.1 port=5433 sslmode=require" -c "SELECT 1;"
```

Warnung: ohne TLS (direkt auf Port 5432 oder Auth-Hook ohne Sidecar) laufen
Auth-Daten im Klartext übers Netz — Auth-Hook nur mit TLS-Sidecar davor in
nicht-vertrauenswürdigen Netzen betreiben.

## 8. Backup / Restore (dbbackup, offline, single-shard)

Tool: `./build/dbbackup` (Quelle: `src/server/backup.cpp`, Binary-Ziel
`dbbackup` in `CMakeLists.txt`). Limitation laut CLI-Hilfe: offline,
single-shard, kein PITR (s. `backup.h`).

```sh
./build/dbbackup --selfcheck
./build/dbbackup --backup --wal /data/wal.log --out /backup/dbengine-2026-09-26
./build/dbbackup --backup --wal /data/wal.log --out /backup/dbengine-full \
  --col /data/columnar --raft-log /data/raft.log --raft-snap /data/raft.snap
./build/dbbackup --restore --in /backup/dbengine-2026-09-26 --wal /data/wal.log
./build/dbbackup --restore --in /backup/dbengine-full --wal /data/wal.log \
  --col /data/columnar
```

Flags belegt aus `Usage()` in `src/server/backup.cpp`:

- `--selfcheck` (allein, ohne weitere Args)
- `--backup --wal <src.wal> --out <backupdir>`
  `[--col <coldir>] [--raft-log <f>] [--raft-snap <f>]`
- `--restore --in <backupdir> --wal <dst.wal> [--col <dstdir>]`

Ablauf: Backup flusht das WAL, speichert Columnar (`columnar/`) + Raft-Log/
Snapshot, kopiert das WAL und schreibt das MANIFEST zuletzt (atomar via
`tmp+rename+fsync`); Restore prüft MANIFEST zuerst und verifiziert
WAL-max-LSN, Raft-commit und Columnar-Rows (siehe `BackupCoordinator::Backup` /
`Restore` in `src/server/backup.cpp`).

## 9. Metrics (dbmetrics, Prometheus-Textformat)

Tool: `./build/dbmetrics` (Quelle: `src/server/metrics.cpp`, Binary-Ziel
`dbmetrics` in `CMakeLists.txt`). Server bindet `127.0.0.1`, Endpunkt
`GET /metrics` (andere Pfade → `404 not found`).

```sh
./build/dbmetrics --selfcheck          # ephemeral Port + Scrape + Asserts
./build/dbmetrics --port 9090          # Dauerbetrieb (Demo-Snapshot)
curl http://127.0.0.1:9090/metrics
```

Flags belegt aus `main()` / Usage in `src/server/metrics.cpp`:
`--selfcheck`, `--port <n>` (`0..65535`; Usage: `[--selfcheck] [--port <n>]`).
`--selfcheck --port <n>` nutzt den festen Port statt ephemeral.

Beispiel-Metriken (belegt aus `MetricsServer::render` in
`src/server/metrics.cpp`, Felder in `include/dbengine/server/metrics.h`):

```text
dbengine_wal_durable_lsn 3
dbengine_wal_next_lsn 4
dbengine_wal_appends_total 3
dbengine_wal_flushes_total 1
dbengine_raft_commit_index 2
dbengine_raft_log_size 2
dbengine_raft_alive_count 1
dbengine_raft_leader_id 0
dbengine_raft_term 1
dbengine_columnar_total_rows 4
dbengine_columnar_sealed_parts 1
dbengine_hnsw_size 4
dbengine_hnsw_dim 4
dbengine_hnsw_max_level 1
dbengine_kv_keys 3
dbengine_kv_sequence 3
```

(Werte: Demo-Snapshot aus `BuildDemoSnapshot` in `src/server/metrics.cpp`;
Produktiv füllt der Aufrufer `Snapshot` aus den Live-Stores.)

## 10. Auth (nur programmatisch, kein CLI-Flag)

Ehrlicher Stand (belegt aus `src/server/pgserver.cpp`): Auth existiert nur
programmatisch via `PgServer::setAuth(users)` (`std::map<std::string,
std::string>`) und `PgServer::setAuthRequired(bool)` — es gibt derzeit
**kein CLI-Flag** (kein `--auth`-o.ä. in `src/server/`). Empfehlung:
Auth-Hook nur hinter TLS-Sidecar betreiben (s. Abschnitt 7), Credentials
nie ohne Sidecar übers Netz schicken.

## 11. S3-Tiering (Sidecar-Skizze, kein Code-Upload)

Ehrlicher Stand: es gibt **keinen echten S3-Upload im Code**.
`ColumnarStore::StageToS3(bucket, prefix)` (Deklaration:
`include/dbengine/columnar/store.h:211`, Definition:
`src/columnar/store.cpp:879`) ist ein Stub — er bildet nur deterministisch
die Ziel-URI, kein Netzwerk/Upload (Kommentar in `store.h:209` +
`store.cpp:881`: „kein Upload, nur deterministische URI-Bildung“).

URI-Format (belegt aus `src/columnar/store.cpp:884-887`):

```text
"s3://" + bucket + "/" + prefix-mit-trailing-'/' + "columnar-" + N + "parts/"
```

Dabei ist `N = parts_.size()` (Anzahl sealed Parts; aktive Replika zählt
nicht, HTAP-Replika wird laut `store.h:224` von Save/Load nicht
persistiert). Prefix ohne trailing `/` bekommt einen angehängt
(`store.cpp:884-885`). Beispiel belegt aus `tests/test_columnar.cpp:144`:
`StageToS3("my-bucket", "tier1")` → `s3://my-bucket/tier1/columnar-1parts/`.

Was der Sidecar synct (Manifest-Format exakt aus Code-Read):

- Quelle: `ColumnarStore::Save(dir)` (`src/columnar/store.cpp:734`,
  Doku: `include/dbengine/columnar/store.h:196-203`).
- Verzeichnisinhalt nach `Save`:
  - je sealed Part: `part-<id>.col` (`store.cpp:745-749`),
  - falls aktiver Part nicht leer: genau ein
    `part-<id>-active.col` (`store.cpp:743-744,751-754`; leere Active
    => kein File),
  - `manifest.txt` **zuletzt**, atomar via `tmp+rename+fsync`
    (`store.cpp:756-784`).
- `manifest.txt`-Format (`store.cpp:761-770`):
  - Zeile 1: `<n>` (Anzahl Einträge = sealed + ggf. 1 aktiv),
  - danach je Zeile: `<id> <fname> <rows> <active 0/1>`
    (`0` = sealed, `1` = aktiv; Beispiel aus Code:
    `man << p.id() << " " << fname << " " << p.size() << " 0\n"`).
- Ziel im Objektstore (Sidecar entscheidet Bucket/Prefix, Code gibt nur
  die URI-Vorlage): `s3://<bucket>/<prefix>/manifest.txt` +
  `s3://<bucket>/<prefix>/part-*.col` (flache Namen, keine Unterverzeichnisse;
  `Load` weist Pfad-Traversal mit `/`, `\`, `..` ab — `store.cpp:818-822`).

Sidecar-Prinzip (Operator-Tooling, nicht Repo-Code):

```sh
# Backup/Tier: erst lokal sealen+saven, dann syncen (Manifest wurde zuletzt geschrieben,
# Sidecar kopiert das fertige Verzeichnis als Ganzes):
rclone sync /data/columnar remote:my-bucket/tier1 --checksum
# Alternative mit MinIO-Client (Operator-Beispiel, Standard-Syntax):
# mc mirror /data/columnar s3alias/my-bucket/tier1
```

(`remote`/`s3alias` ist Operator-Konfiguration des Sidecars; kein Flag/
Credential ist im Repo belegt — Credentials gehören in ein K8s-Secret,
s. `k8s/statefulset.yaml` S3-Skizze.)

Restore-Reihenfolge (belegt aus `Load` in `src/columnar/store.cpp:787`):

1. Sidecar kopiert zurück (`rclone sync remote:my-bucket/tier1 /data/columnar`
   bzw. `mc mirror s3alias/my-bucket/tier1 /data/columnar` — Standard-Syntax,
   Operator-Beispiel).
2. `ColumnarStore::Load(dir)` prüft **zuerst** `manifest.txt`
   (`store.cpp:789-794`), parst `<n>` + `<id> <fname> <rows> <active>`-Zeilen,
   lehnt `>1` aktive Einträge ab (`store.cpp:825-827`), lädt dann je Eintrag
   das Part-File und verifiziert `p.size() == rows` und `p.id() == id`
   (`store.cpp:839-840`); aktive Rows werden unsealed/mutabel wiederhergestellt
   (`store.cpp:843-849`). Fehlt/korrumpiert ein File → `false`.
3. Danach ggf. WAL-Tail per Abschnitt 4 (`replay` ab `wal_lsn+1`) — WAL bleibt
   Single Source of Truth für Crash-Recovery; Columnar-Tiering ersetzt kein
   `flush()`/`checkpoint()`.
