# dbengine — Architektur (V2, embedded-first)

> Stand: 09/2026. Ziel: OLTP+OLAP+Vektor+TS in einem Kern, Start embedded
> (SQLite/DuckDB-Vorbild), Cluster-Pfad vorbereitet. Details siehe
> `ANFORDERUNGEN.md` (F1–F6, N1–N7, Phasen 0–3).
>
> V1 (s01–s20): Row-Kern + Stubs. V2 (s21–s26): echte Implementierungen —
> siehe Kapitel 8 (HNSW-multi, PQ-N/IVF, WAL-Group-Commit, COL1/COL2-File, Raft-TCP).
> Welle 8–10 (s38–s52): SQL-Aggregate/Session/Filter, Metrics/Backup/Kill-9,
> Pager-Atomaritaet, HNSW/IVF-Skala, Auth/TLS — siehe Kapitel 9.
> Welle 11–12: JOIN/DDL/COL2/SoA/Audit/TPC-H/Parallel-Build/Extended — siehe Kapitel 10.
> Phase 0 (s72–s77): Single-File-Container/BTreeKV/CLI/Mirror/Autotune/Duell — siehe Kapitel 11.

## 1. Schichtenmodell

Single-Binary, phasierter Aufbau. Datenfluss für Writes (Pfeile abwärts),
Reads nutzen Snapshot-Pfade (Pfeile aufwärts):

```text
SQL (parser, pgwire, executor, jsonb)
  |
Txn (mvcc, clock: TSO/HLC + Commit-Wait, echt)
  |
KV (kv: Get/Put/Delete/Scan/Iterator, WriteBatch, Snapshot)
  |
Raft (shard: Range/Tablet + RaftGroup 3er-Sim + TCP-Wire/Persistenz s25)
  |
Storage (pager 16KiB + wal REDO + columnar Parts + index + vector/search)
```

| Schicht | Code | Verantwortung |
|---|---|---|
| SQL | `src/sql/parser.cpp`, `src/sql/executor.cpp`, `src/sql/jsonb.cpp`, `src/server/pgwire.cpp`, `src/server/pgserver.cpp` | Mini-SQL `CREATE/INSERT/SELECT` + Filter/Sort/Aggregate (s39/s43/s48: `SUM/AVG/MIN/MAX/COUNT`, `GROUP BY`, `BETWEEN/IN/OR`, `ORDER BY/LIMIT/OFFSET`), `TableRegistry`, JSONB-Operatoren `->/->>/@>`, PGWire-Session + TCPServer (127.0.0.1 ephemeral, echte T/D/C mit OIDs/NULLs, Auth-Hook opt-in) |
| Txn | `src/txn/mvcc.cpp`, `src/txn/clock.cpp` | Snapshot-Isolation, Undo-Ketten, Single-Writer pro Shard, TSO/HLC + Commit-Wait + monotone Snapshots (echt, `txn/clock.h`: `HybridLogicalClock`/`TimestampOracle`/`SnapshotIssuer`, kein Stub) |
| KV | `src/kv/kv.cpp` | Native API `Get/Put/Delete/Scan/Iterator`, atomarer `WriteBatch`, `Snapshot`-Handle |
| Raft | `src/raft/shard.cpp` | `Shard{id, [start,end)}` + `RaftGroup` (3 Nodes in-process-Sim + TCP-Wire-Codec/`SaveLog`/`SaveSnapshot`/Follower-Reads, s25) |
| Storage | `src/storage/pager.cpp`, `src/storage/wal.cpp`, `src/columnar/store.cpp`, `src/index/btree.cpp`, `src/vector/hnsw.cpp`, `src/search/hybrid.cpp` | 16KiB-Pages + mmap, REDO-WAL, immutable Columnar-Parts (COL1-File+Merge, s24), Secondary-B-Tree, mehrschichtiger filterbarer HNSW + BM25-Hybrid |

Regeln:

1. **SQL ruft nie Storage direkt auf** — immer via `executor → KV + MVCC + WAL`.
2. **Genau ein WAL + Checkpoint + Snapshots** für alle Engines (kein doppeltes
   WAL/Binlog, vgl. F2.4).
3. **OLTP (Row/Pager) und OLAP (Columnar-Parts) sind zwei Engines, ein Log**
   (HTAP-Lernsatz: TiFlash-Modell).
4. **Vektor/Hybrid sind Filter-First**, kein Sidecar-Index.

## 2. WAL-Format (`include/dbengine/storage/wal.h`, `src/storage/wal.cpp`)

REDO-Log, append-only, little-endian, pro Record:

```text
magic u32 = 0x57414C31 ("WAL1")
lsn   u64 = streng monoton ab 1 (next_lsn)
len   u32 = payload bytes (max 16 MiB, kMaxPayload)
crc   u32 = CRC32-IEEE (0xEDB88320) über lsn-Bytes + len-Bytes + payload
payload[len]
```

Crash-Safety-Modell:

- `append()` → nur `write()` in Page-Cache, **kein fsync pro Record**.
- `flush()` → genau ein `fdatasync/fsync` (Group-Commit).
- Regel: **nach `flush()` ist alles Kill--9-sicher**.
- `open()` scannt `max-LSN`, kappt Torn-Tail (abgerissener letzter Record →
  Prefix gewinnt, `scan()` validiert Magic+Len+CRC).
- `replay()` / `replay_file(path)` liefern alle gültigen Records.
- `checkpoint(lsn)` verwirft Records `<= lsn`, crash-sicher via `tmp+rename`;
  `checkpoint(all)` leert Datei, `next_lsn` reset auf 1.
- Referenztest: 1000 appends, Kill-Sim (close ohne Checkpoint → reopen →
  Replay identisch), Checkpoint-Rest-Semantik, CRC-Sanity `123456789 → 0xCBF43926`.

## 3. MVCC-Regeln (`include/dbengine/txn/mvcc.h`, `src/txn/mvcc.cpp`)

InnoDB-nah (Undo-Log), **bewusst nicht PG-Heap** (kein Bloat, kein Vacuum):

- Genau **eine gültige Zeilen-Version** liegt in der Primary-Kette, alte
  Versionen sind Undo-Historie pro Key (`map<key, vector<Version>>`, oldest→newest).
- `Version{trx_begin, trx_end, value, deleted}`. Sichtbar für Snapshot `s` gdw.
  `trx_begin <= s && s < trx_end` (`IsVisible`, `kInfTs = UINT64_MAX`).
- Writes landen erst im Txn-Buffer (`write_set: map<key, optional<value>>`,
  `nullopt` = Delete-Tombstone). `Commit` installiert **atomar unter einer
  `commit_ts`**, `Abort` verwirft (Undo = No-Op).
- Daraus per Konstruktion: **keine Dirty-Reads, keine partiellen Commits**.
- Timestamps: `atomic<uint64_t> next_ts_` ab 1. `Begin` zieht `begin_ts`,
  `Commit` zieht `commit_ts`. SI default: `snapshot = begin_ts` (fix bei Begin).
- **Single-Writer pro Shard**: `writer_mu_ + active_writer_`.
  `TryBeginWrite()` scheitert sofort wenn besetzt, `BeginWriteBlocking()` wartet.
- Read-Only-Txns sind **lockfrei gegenüber Writer** (nur kurzer `mu_` für
  Ketten-Lookup), beliebig parallel, `read-own-writes` sichtbar.
- `Purge()` (Undo-GC): entfernt alte Versionen, die für kein aktives Snapshot
  mehr sichtbar sein können. Niemals die neueste Version.
- Isolation: `Snapshot` (default) + `ReadCommitted`. Lücke (bewusst):
  **kein SSI/Serializable, kein Distributed/2PC** in V1.

## 4. Shard-Konzept (`include/dbengine/raft/shard.h`, `src/raft/shard.cpp`)

- `Shard{id, range_start, range_end}`: Key-Range `[start, end)`, leeres `end` =
  offenes letztes Tablet. `contains(key)` prüft Zugehörigkeit.
- Logische Tablet-Größe: `kShardTargetBytes = 96 MiB` (Platzhalter, kein
  Auto-Split/Merge in V1; Zielarchitektur 96MB/512MB/5GB vgl. F5.2).
- Jede Shard trägt eine `RaftGroup` (RF=3, in-process-Sim, **kein Netzwerk**):
  - `Node{id, role, current_term, voted_for, log[], commit_index, last_applied, alive, applied(KV-State-Machine)}`.
  - `Entry{term, index (1-basiert), command ("put k v" / "del k")}`.
  - `electLeader()`: lebende Kandidaten in Id-Reihenfolge; Vote von V nur wenn
    `alive` und `can_send` symmetrisch. Default-Mesh = erster Lebender, alle
    Lebenden voten (Bestand `test_raft`). Mehrheit ≥2, sonst `-1`.
  - `append(cmd)`: nur Leader, `replicateToFollowers` synchron, Commit bei
    Mehrheit **erreichbarer** Lebender, `apply()` parst Command in KV-Map.
    Rückgabe Log-Index, `0` bei Fehler (kein Leader / tot / kein Quorum /
    Partition). Isolierter Leader schiebt `commit_index` nicht vor.
  - `killLeader()` + `failover()`: deterministische Neuwahl unter Lebenden
    (in-process µs, Sim-Ziel <100ms). `killNode/reviveNode` mit Catch-up + apply.
  - Partition-API (Prodsim): `isolate`/`heal`/`isolate_from_all`/`heal_all`/
    `set_drop_rate`/`can_send` — 3×3-Matrix, Default fully meshed, Drop-PRNG
    Seed 42. `alive` (Crash) ≠ Partition (Netz-Cut). Tests: `test_prodsim`;
    TCP-Loopback-Hosts: `tools/prodsim_cluster.cpp`. Siehe `docs/PRODSIM.md`.
- V1-Grenzen (s25 geschlossen, Sim bleibt): persistentes Raft-Log via
  `SaveLog/LoadLog` (`RAFT1`), Snapshots via `SaveSnapshot/LoadSnapshot`
  (`RSNP1` + `log_base_`-Compaction), TCP-Framing via Wire-Codec
  (`Send/RecvWire`, `TcpLoopbackPair`), Follower-Reads (`follower_get`,
  `is_caught_up`), Split/Merge-Range-Ops. Weiter Sim: keine Membership-Change,
  keine echten Follower-Reads über Netzwerk, kein Raft-over-TCP.
  Failover-Pfad ist testabgedeckt (`test_raft` + `prodsim`).

## 5. Vektor / Hybrid (`include/dbengine/vector/hnsw.h`, `include/dbengine/search/hybrid.h`)

Vektor first-class, **nur STL/POSIX, kein FAISS** (siehe `SBOM.md`):

- Typ: `Vector = vector<float>` (float32, C++20).
- Distanz: L2 (`l2_distance`) + Cosine (`1 - cos`, [0,2]), per-Query
  umschaltbar (`DistanceMetric`), Dispatch `dispatch_distance`.
- **HNSW-lite**: Single-Layer (Layer0), `M=16` Nachbarn default
  (fully-connected k=16, exakt via Brute-Force in `build()`, `O(N²·dim)`),
  Beam-Search mit `ef` **runtime-tunbar pro `search()`-Call**
  (`ef<=0 → ef_default=32`, F4.3). `set_m/set_ef_default/set_metric`.
- **Filter+ANN gemeinsam** (kein Pre/Post-Filter): `search(q,k,ef,filter_fn)`
  evaluiert Filter während Traversierung in einem Durchgang. Unpassende Knoten
  werden expandiert (Frontier), nur passende in Top-`ef`→Top-`k` aufgenommen.
  Beam stoppt erst wenn nächste Frontier-Distanz schlechter als schlechtestes
  gefiltertes Ergebnis **und** `ef` gefilterte Treffer vorliegen.
  → traversierbar auch bei selektiven Filtern (kein Disconnect, kein
  Post-Filter-Recall-Verlust).
- Baseline: `brute_force(q,k,filter)` Full-Scan mit gleichem Filter/Distanz
  für Recall-Messung (`Recall@10`, Ziel >0.9–0.95).
- Quantisierung: `SQ8` echt (`vector/quant.h: Sq8Quantizer`: min/max pro Dim →
  uint8, `encode/decode`, ADC `adc_l2_squared` + Re-Rank `quantized_search_rerank`;
  alter `hnsw.h:SQ8Quantizer`-Stub nur noch historisch, aktive Suche nutzt
  `quant.h`). PQ siehe 8.2/9.8 (PQ-N + residuales IVF).
- Thread-Safety: `search/brute_force` const & nebenläufig sicher nach `build()`;
  `add/build` nicht nebenläufig sicher.

Hybrid (`src/search/hybrid.cpp`, unabhängig von s08):

- BM25-lite: lowercase/alphanumerisch-Tokenisierung, FNV-1a 64-bit Term-Hash,
  `IDF = ln(1+(N-df+0.5)/(df+0.5))`, `k1=1.2, b=0.75`.
- Dense kommt als Parameter (`vector<DenseScore>` / `map<id,float>`), kein
  s08-Include (Entkopplung).
- Fusion: `Weighted (w_dense·norm + w_bm25·norm, min-max)` oder
  `RRF (w/(k+rank), k=60)`. Top-K via Min-Heap, Tie-Break Score-desc/ID-asc.
- Columnar-Anbindung (s10): immutable Parts + Dict/RLE-Stub + Arrow-Export +
  S3-Tier-Stub; Vektor/FTS-Pushdown (`ef/lists/probes` tunbar) läuft über
  dieselbe Parts-Abstraktion (Partition-Pruning vorbereitet).

## 6. Weitere Module (Kurzreferenz)

- **Pager** (`storage/pager.h`): 16KiB-Pages, Single-File, LRU, `mmap`
  Zero-Copy Reads (`read_zero_copy`) mit `pread`-Fallback, `allocate/write/read_page`;
  clustered B+Tree-Hülle (`insert/find/erase`, key-sortiert) + atomares
  `store_image` (`tmp+rename+fsync`, `load_image` strikt: Torn→Fehler, s50).
- **KV** (`kv.h`): `Get/Put/Delete/Scan/Iterator`, `WriteBatch` atomar.
- **Index** (`index/btree.h`): Secondary B-Tree via `multimap` + TTL + ART/GIN-Stubs (unverändert Stub).
- **Columnar** (`columnar/store.h`): immutable Parts + Pruning + COL1-File/Merge/Compact via `Save/Load` (s24, kein OLTP-Retrofit).
- **JSONB** (`sql/jsonb.h`): Binär-JSON, GIN-Inverted-Index, `->/->>/@>`.
- **Clock** (`txn/clock.h`): TSO/HLC + Commit-Wait + monotone Snapshots (echt: `HybridLogicalClock`, `TimestampOracle`, `SnapshotIssuer`, s14).
- **PGServer** (`server/pgserver.h`): POSIX-TCP, Startup/Q-Flow über Socket,
  echte Session (`Executor::execute`, T mit OIDs / D mit `NULL=-1` / C / Z,
  `SELECT 1`-Sonderpfad erhalten) + opt-in Auth-Hook (`setAuth/setAuthRequired`,
  Fail `28P01`, Default Trust-All, s41/s49).

## 7. Nicht-Ziele V1/V2 / Grenzen (Stand s26)

- V1-Grenzen (s01–s20, teils in V2 geschlossen): Single-Layer-HNSW → s21
  mehrschichtig; PQ-2-Stub → s22 PQ-N+IVF; WAL ohne Wait/CDC → s23;
  Columnar ohne File/Merge → s24; Raft ohne Netz/Persistenz → s25.
- Weiter offen: verteiltes 2PC/SSI, Auto-Split mit Daten-Move (nur Range-Ops,
  s25), Graph (V2-Empfehlung Kuzu-embedded),
  RESP/Arrow-Flight/REST (F6.2), SDKs/WASM (F6.3). DiskANN-These überholt:
  Spill via `vector/quant.h: DiskSpill` (mmap-File, echt) + HNSW+PQ+IVF vorhanden.
  Siehe Roadmap `.rfg/roadmap.yaml`.

## 8. V2-Vertiefungen (s21–s25, alle STL/POSIX-only, Apache-2.0)

### 8.1 Mehrschichtiger filterbarer HNSW (s21, `vector/hnsw.h`)

- Level-Sampling `l = floor(-ln(U) * mL)`, `mL = 1/ln(M)`, Cap 8.
- `M` (obere Layer) vs. `Mmax0 = 2*M` (Layer0), `efConstruction` (Default 64,
  `set_ef_construction`), `ef` pro Query, deterministischer Seed 42.
- Inkrementelles `add()` (sofortiges Insert, kein O(N²)-Rebuild),
  `build()` deterministischer Neuaufbau O(N log N).
- Filter+ANN gemeinsam auf Layer0 (s08-Semantik), obere Layer ungefilterter
  Descent. Messung 1000×64-dim: Recall@10 avg 0.988 (ef=64), p95 0.06ms.

### 8.2 PQ-N + IVF-Coarse + ADC (s22, `vector/quant.h`)

- `PqNQuantizer(dim, M)`, M=1..32 (2/4/8/16): k-means deterministisch,
  echte ADC-Tabellen `build_adc_table()` (M×256 L2²) + `adc_with_table()`.
  `PqQuantizer` (M=2 fix) und SQ8 bleiben kompatibel erhalten.
- `IvfPqIndex(dim, M, nlist)`: Coarse-k-means, Inverted Lists,
  `search(q, base, k, nprobe, ef_rerank)` mit ADC-Coarse + exaktem Re-Rank.
- Bench 10k×64-dim uniform (Worst-Case): PQ-N+M16+ReRank200 → Recall 1.0;
  IVF nprobe=nlist → 1.0 bei ~0.6ms (Teilscan-Tradeoff dokumentiert).

### 8.3 WAL Group-Commit + LSN-Wait + CDC (s23, `storage/wal.h`)

- `append_many()` (ein Lock für N Records) + ein `flush()`-fsync:
  500 appends → 1 flush (`group_stats()`).
- `durable_lsn()` + `wait_for_lsn(target, timeout_ms)` (CondVar,
  Notify bei flush/open/checkpoint) — `WAIT FOR LSN`-Analogie (F2.4).
- CDC: `read_from(from_lsn, max)` + `WalCdcSlot::poll/seek` (Cursor-Pollen).
- Crash-Safety unverändert: write ohne fsync, fdatasync pro flush,
  torn-tail-cap, tmp+rename-Checkpoint.

### 8.4 COL1-File + Merge + Compaction (s24, `columnar/store.h`)

- `Part::Save/Load`: `COL1`-Format (Header id/name/rows/min/max + RLE-Ints +
  Dict-Strings + Codes), roundtrip-treu inkl. Zonemaps.
- `Part::Merge(a, b, id)`: stabiler Sorted-Merge nach int, frische Zonemaps.
- `ColumnarStore::Save/Load(dir)`: Manifest + `part-<id>.col` je sealed Part;
  `Compact()` merged alle sealed Parts sortiert zu einem.
- `SumLessThan` block-vektorisiert (2048er-Blöcke, gleiche Pruning-Semantik).

### 8.5 Raft-Transport + Log + Snapshots (s25, `raft/shard.h`)

- Wire-Codec: `Encode/DecodeEntryWire` (term/index/cmd, BE) +
  `Send/RecvWire` (u32-Längen-Framing, 16MB-Cap) + `TcpLoopbackPair`
  (127.0.0.1 ephemeral, POSIX-TCP).
- `SaveLog/LoadLog` (`RAFT1`: term/commit/base + lückenlose Entries).
- `SaveSnapshot/LoadSnapshot` (`RSNP1`: last_index + KV-Map) mit echter
  Log-Compaction via `log_base_` (Append/Replikation/Apply basis-bewusst;
  Base 0 = V1-Semantik).
- `follower_get` + `is_caught_up` (read_index light),
  `Shard::split(mid)` (binärer ID-Baum, Range-validiert) + `can_merge_with`
  (Adjazenz); Daten-Move als Follow-up dokumentiert.

## 9. Welle 8–10 (s38–s52, alle STL/POSIX-only)

### 9.1 NVMe-Bench (`tools/bench.cpp`, s38)

- `dbbench --data-dir DIR`: `Wal::append_many` + `flush` in Batches auf
  `DIR/dbbench.wal`; CSV `op,throughput,lat_p95` + Zeilen `wal_durable_puts`,
  `wal_flush_p95` (alte Modi unverändert).
- Messwerte (WAL durable puts): NVMe ~40k ops/s, p95 ~1.2ms; tmpfs ~682k ops/s
  (fsync vs. Page-Cache; Vergleichsanker, kein SLO).

### 9.2 SQL-Aggregate + GROUP BY (`sql/parser.h`, `src/sql/parser.cpp`, `src/sql/executor.cpp`, s39/s43)

- Skalar-Aggregate ohne `GROUP BY`: `SUM/AVG/MIN/MAX/COUNT(col|*)`, Arg = Spalte
  oder binärer `*/+-`-Ausdruck (`AggExpr`, DOUBLE, NULL propagiert);
  Q6-Kern `SUM(price*(1-disc))` grün; leere Eingabe ohne `GROUP BY` = 1 Zeile.
- `GROUP BY` (1..n Spalten, Hash-Aggregation, Q1-Kern ohne `ORDER BY`):
  gemischte Projektion Gruppen-Spalten + Aggregate; leere Eingabe = 0 Gruppen.
- Alias-Fix: `AS`-Alias + Blank-Alias für Spalten und Aggregate
  (`SELECT SUM(x) AS s`, `SELECT rf g`), Klausel-Keywords schlucken keinen Alias
  (`parser.cpp` Klausel-Guard); `ORDER BY` löst Alias/Aggregat/Ordinal auf.

### 9.3 SQL-Filter + Sort (`sql/parser.h`, `src/sql/parser.cpp`, s48)

- `WHERE`: `BETWEEN/NOT BETWEEN`, `IN/NOT IN (v, ...)`, `IS [NOT] NULL`,
  `LIKE/ILIKE`, Vergleichs-Ops; DNF via `where_groups` (`OR` von `AND`-Ketten).
- `ORDER BY` multi, je Item `ASC/DESC` + `NULLS FIRST/LAST` (Default PG-konform:
  `ASC→NULLS LAST`, `DESC→NULLS FIRST`), Referenzen: Alias/Aggregat/Ordinal/
  Tabellenspalte; danach `LIMIT n|ALL` + `OFFSET n`.

### 9.4 PGWire-Session + Auth (`server/pgserver.h`, `src/server/pgserver.cpp`, s41/s49)

- Session echt: `Q` → server-eigene `Executor`-Instanz (`execMu_`-serialisiert);
  `SELECT` → `T` (Spalten + OIDs) / `D*` (Text, `NULL=-1`) / `C(SELECT n)` / `Z`;
  `INSERT/CREATE` → `C(tag)/Z`; Fehler → `E(42601/0A000)/Z`; `SELECT 1`-Sonderpfad
  byte-identisch; `X` beendet sauber, Extended/COPY → `E(0A000)/Z`.
- Auth-Hook opt-in: `setAuth(map)` + `setAuthRequired(bool)` (Default Trust-All =
  altes `R(0)+Z`); required: `R(3 Cleartext)` → `PasswordMessage('p')` → Match
  `R(0)`, sonst `E FATAL 28P01` + close; Cleartext nur hinter Sidecar-TLS
  (Kommentar-Warnung in `pgserver.h`).

### 9.5 Metrics (`server/metrics.h`, `src/server/metrics.cpp`, `dbmetrics`, s42)

- `MetricsServer` (127.0.0.1, ephemeral, `GET /metrics`): rendert `Snapshot` als
  Prometheus-Text (`dbengine_*`); hält keine globalen Stores (Aufrufer füllt).
- Liste: `wal_durable_lsn/next_lsn/appends/flushes`, `raft_commit_index/log_size/
  alive_count/leader_id/term`, `columnar_total_rows/sealed_parts`,
  `hnsw_size/dim/max_level`, `kv_keys/sequence`.
- `dbmetrics --selfcheck` (ephemeral, assertet Keywords), Demo-Fütterung via WAL.

### 9.6 Backup (`server/backup.h`, `src/server/backup.cpp`, `dbbackup`, s44)

- Offline, single-shard: `flush` → `ColumnarStore::Save` → `Raft SaveLog+
  SaveSnapshot` → WAL-Copy (`wal.log`) → `MANIFEST` **zuletzt**, atomar
  (`tmp+rename+fsync` File + Dir, Muster `wal.cpp`).
- `MANIFEST` (`DBBACKUP1`, `key=value`): `wal_lsn/rows/term/commit`; Layout:
  `wal.log`, `raft.log` (`RAFT1`), `raft.snap` (`RSNP1`), `columnar/` + `MANIFEST`.
- Restore: `MANIFEST` validieren → WAL kopieren + `replay_file`-Check
  (`max-LSN == wal_lsn`) → `LoadLog` + `LoadSnapshot` (Commit-Match) →
  `ColumnarStore::Load` (Rows-Match) → Tail-Count ab `wal_lsn+1` (offline: 0,
  `Executor::recover()` anwendbar); kein PITR/Incremental/Multi-Shard.

### 9.7 Crash-Garantien (`storage/wal.h`, `tests/test_wal.cpp` s46, `tests/test_chaos.cpp`)

- Regel bleibt: Kill-9-sicher **nur nach `flush()`**; ungeflushte `append`s gehen
  verloren (Design); Torn-Tail → Prefix gewinnt (Magic+Len+CRC, open kappt).
- s46-Harness echt: `fork+kill -9` vor/nach `flush` + mitten in `checkpoint`;
  Reopen-Replay = Prefix-Modell, Verlust 0 nach `flush`.
- Chaos (10k Ops, Seed 42): put/del/scan + flush/256 + Checkpoint/CDC/Wait +
  Torn-Inject nach Op 5000; Replay == Modell, `map_replayed == expected == Scan`.

### 9.8 Vektor-Skala (`vector/hnsw.h`, `src/vector/hnsw.cpp`, `vector/quant.h`, `src/vector/quant.cpp`, s40/s47/s51)

- Symmetrische Shrink-Heuristik (s40, `shrink_layer[_after_add]`): reziproke
  Kantenstützung mit Diversitäts-Heuristik (Paper Alg. 3) statt nächste-only;
  Fernkanten bleiben → clustered-Recall 1.0, uniform ≥ Stand.
- Query-Probes (s47): `P=min(256,N/64)` adaptive Multi-Start-Coarse-Probes
  (flach, deterministisch, `kQueryProbeMax=256`) + squared-Hotpath
  (`dispatch_distance2`, L2 ohne sqrt pro Kandidat); kleine N deutlich schneller.
- Residuales IVF + Autotune (s51, Format-Wechsel): PQ trainiert auf Residuen
  (`v − coarse[assign(v)]`), Codes inkompatibel zu alt (neu `train+build` nötig);
  Sample-Regel `min(N,max(2048,N/10))`; Defaults `nlist≈4·sqrt(N)` ([1,4096], ≤N),
  `nprobe≈nlist/8` (`default_nlist/nprobe/autotune(n)`); `ef`-Autotune
  (`autotune_ef(k,sel)`, `autotune_ef_for_filter`) + Coarse-P256-Pfad
  (AVX2 `_mm256`-Kerne, `hsum_m256`, Sample-Caps→Prozent-Regel).

### 9.9 Pager-Atomarität (`storage/pager.h`, `src/storage/pager.cpp`, s50)

- `store_image`: serialisieren → `path.tmp` → fsync → `rename` → Dir-fsync
  (alt oder neu vollständig, nie torn-mix); `load_image` strikt
  (Magic/Version/Record-Stream/Trailing-Zero; torn → open-Fehler statt Teilverlust).
- Header `DBENPG01`, clustered KV-Image (key-sortiert), `mmap`-Read + `pread`-
  Fallback (`uses_mmap()`), Torn-Image-Test vorhanden.

### 9.10 TLS-Sidecar (`docs/RUNBOOK.md` Kap. 7, `k8s/statefulset.yaml`, s52)

- Kein eigenes TLS im Server (Klartext, Trust/Proxy-Vertrauen); Terminierung im
  Sidecar (`stunnel`: `accept 5433 → connect 5432`, `envoy`-analog); Test
  `psql "sslmode=require" port 5433`; K8s nur auskommentierte Skizze
  (`tls-cert`-Secret-Volume, `stunnel`-Sidecar, `pgwire-tls`-Port 5433, nicht
   deployed); Warnung: Auth-Hook ohne Sidecar = Klartext über Netz.

## 10. Welle 11–12 (alle STL/POSIX-only)

### 10.1 JOIN — INNER, Hash vs. Nested-Loop (`sql/parser.h`, `src/sql/parser.cpp`)

- Genau ein `INNER JOIN` pro `SELECT`: `FROM a [AS x] [INNER] JOIN b [AS y]
  ON a.c = b.d [AND ...]` (`parser.h:13-16`, `parser.cpp:797-833`:
  `Nur ein JOIN pro SELECT`, kein `LEFT/RIGHT/FULL/OUTER/CROSS`).
- `t.c`-Refs in `SELECT/WHERE/ON/GROUP BY/ORDER BY`/Aggregat-Args
  (`parser.h:21-23`, `parser.cpp:277-284`: `col | t.c`, nur ein Prefix).
  Qualifizierer = Tabellenname oder Alias je Seite; unqualifiziert +
  beidseitig vorhanden → `SqlError (ambiguous, t.c angeben)`
  (`parser.cpp:1779-1811`, `1804-1806`); Self-Join braucht zwei Aliase.
- Ausführung `execJoinRows` (`parser.cpp:2039-2042`): reine Equi-`ON`-Kette
  über beide Seiten → Hash-Join über die kleinere Seite (Build/Probe,
  `L.size() <= R.size()` vs. umgekehrt, `parser.cpp:2049-2101`), NULL-Keys
  matchen nie (`parser.cpp:2055`); sonst Nested-Loop mit voller
  `AND`-Auswertung (`parser.cpp:2112ff`). Output links-major
  (`stable_sort` nach `(li,ri)` bei Links-Build).
- `WHERE/GROUP BY/ORDER BY/LIMIT/OFFSET`/Aggregate danach auf Combined-Rows
  `[links..., rechts...]` wie bisher (`parser.h:15-16`).

### 10.2 COL2-Codec — Delta/FOR/Bitpacking (`columnar/store.h`, `src/columnar/store.cpp`)

- `Save` schreibt `COL2`, `Load` liest `COL1` (Fallback) UND `COL2`
  (`store.h:126-133`, `store.cpp:357-371/430`).
- COL2-Int-Layout (`store.h:129-131`, `store.cpp:84-93`): Basis = `min`
  (int64, Zonemap); `Delta_i = value_i - base` (mod 2^64);
  `bitwidth = bit_width(max_delta)` (`ForBitWidth`, `0` = alle == min,
  keine Worte); LSB-first bitgepackt (`PackForDeltas`, `store.cpp:101-118`),
  `nwords = ceil(rows*bitwidth/64)`; `bitwidth==64` nur Offset 0.
  Roundtrip exakt (`UnpackFor`, `store.cpp:120-149`).
- File: `COL2 | u64 id | name | rows | min/max | bitwidth/nwords/Worte |
  Dict-Strings` (`store.cpp:359-393`); `Load` mit `rows`-Mismatch-Guard
  (`FOR`, `store.cpp:495`).
- Grösse (Erwartung, keine Messung — Agenten-Abschaetzung): 1M monotone
  Ints (z.B. `0..1M-1`, `max_delta ~ 1M` → `bitwidth = 20`) →
  `1M*20 bit ≈ 2.5 MB` Int-Payload + Header/Dict. Allgemein
  `rows*bitwidth/8` Bytes; `bitwidth==0` (konstant) → 0 Worte.

### 10.3 SoA-Layout + `get()`-Semantik (`vector/hnsw.h`, `src/vector/hnsw.cpp`)

- Primaer-Speicher flach: `data_flat_` (`N*dim` floats, Row-Major,
  Zeile `id` ab `id*dim_`) + `norms_` parallel (Cosine), Invariante
  `data_flat_.size() == size()*dim_` (`hnsw.h:283-286`).
  Distanzkerne auf Buffer-Pointern (`row_ptr`, `hnsw.cpp:1017-1019`),
  kein `Vector`-Umweg (`hnsw.h:33-36`).
- `get(id)` gibt aus API-Gruenden weiter `const Vector&` zurück, backed
  durch `thread_local`-Kopie-Puffer (`hnsw.h:37-45`, `hnsw.cpp:1021-1030`):
  gültig nur bis zum naechsten `get()` auf DEMSELBEN Thread; zwei Refs
  gleichzeitig halten ist UNSICHER → bei Bedarf kopieren
  (`Vector v = idx.get(id);`). Nebenlaeufige `get()` auf VERSCHIEDENEN
  Threads sicher; `add/clear/build` macht alte Kopien stale.

### 10.4 Audit-Log — AUD1-Konvention auf WAL (`storage/wal.h`, `src/storage/wal.cpp`)

- Normale WAL-Records mit Prefix `AUD1\n` + `esc(actor)\n + esc(action)\n +
  esc(detail)` (`wal.h:37-52`, `wal.cpp:473`); Escaping pro Feld
  `\\ → \\\\`, `\n → \\n`, strikter 5-Byte-Prefix-Match, sonst Nicht-Audit
  → skip (`wal.h:51-52`, `wal.cpp:483-504`).
- API: `append_audit(actor,action,detail)` (neue API only, `wal.h:94-98`),
  `read_audit(from_lsn,max)` = `read_from` + `parse_audit`, Limit zaehlt
  gefilterte Events (`wal.h:99-104`, `wal.cpp:520-527`); `parse_audit`
  rein, kein Lock/Dateizugriff (`wal.h:60-64`).
- Compliance-Vorstufe, KEIN Tamper-Schutz: ohne HMAC/Signatur/Kette,
  faelschbar per Dateizugriff/`checkpoint()` — nur Filter-Konvention
  (`wal.h:39-43`).

### 10.5 TPC-H-Harness — Q1/Q6-Modi (`tools/bench.cpp`)

- `RunSqlQ1` (`bench.cpp:289-329`): Columnar-`ScanSumLessThan`-Mikrobench
  (Q1-Analogie `SUM(l_extendedprice) WHERE < threshold`), 5 Reps, CSV
  `sql_q1` + stderr-Prüfsumme.
- `RunTpch` über echten Executor (KV+MVCC-Pfad, `bench.cpp:331-336`,
  `361-370`): deterministische `lineitem`-Tabelle (Seed 42, batched
  `INSERT`s à 500, `bench.cpp:372-416`).
- Q6 (`bench.cpp:419-461`): `SELECT SUM(price*disc) WHERE disc BETWEEN
  0.05 AND 0.07 AND qty < 24 AND price >= 500 AND price < 5000 AND tax <=
  0.05 AND shipdate BETWEEN 19940101 AND 19951231` (5x AND/Range,
  Referenz-Summe + rel-Check `1e-9`, CSV `tpch_q6`).
- Q1-Kern (`bench.cpp:463-466`): `SELECT rf, ls, SUM(qty), SUM(price),
  SUM(price*disc), AVG(disc), COUNT(*) ... GROUP BY rf, ls ORDER BY rf, ls`
  (Hash-Agg, kanonischer Hash über `valueToString`-Keys, CSV `tpch_q1`).

### 10.6 DDL — UPDATE/DELETE/DROP via Tombstones (`sql/parser.h`, `src/sql/executor.cpp`)

- Syntax (`parser.h:17-20`, `100-123`): `UPDATE t SET c=v [, ...] [WHERE
  ...]` (SET-Literale typkoerziert, WHERE-DNF wie `SELECT`),
  `DELETE FROM t [WHERE ...]` (ohne `WHERE` = alle), `DROP TABLE
  [IF EXISTS] t` (kein `CASCADE/TRUNCATE`).
- `UPDATE` (`executor.cpp:330-409`): Snapshot-Scan (KV-Key-Menge + nur
  committed MVCC-Werte, kein KV-Fallback-Dirty-Read), `rowMatchesWhere`,
  Treffer als neue Vollzeilen; WAL zuerst (Opcode `U`), dann KV-Batch,
  dann eine Writer-Txn (MVCC-Commit, alte Version via `trx_end` abgelöst);
  Message `UPDATE n`.
- `DELETE` (`executor.cpp:411-473`): Treffer per Tombstone (`MVCC-Erase`
  → `deleted`-Version, `SELECT` unsichtbar) + KV-Key hart entfernt; WAL
  zuerst (Opcode `D`); Message `DELETE n`.
- `DROP TABLE` (`executor.cpp:474-499`): Schema-Key + alle Row-Keys aus KV,
  MVCC-Tombstones je Row-Key, Registry-Eintrag geloescht; WAL zuerst
  (Opcode `T`); danach Tabelle unbekannt (`SqlError`, ausser
  `IF EXISTS` → `DROP TABLE` 0).

### 10.7 Parallel-Build — `build_parallel` (`vector/hnsw.h`, `src/vector/hnsw.cpp`)

- Nur `<thread>/<mutex>/<atomic>`, kein OpenMP/TBB (`hnsw.h:168-177`);
  `build()` = `build_parallel(1)` exakt alter Single-Pfad (`hnsw.cpp:723`).
- Ablauf (`hnsw.h:170-177`, `hnsw.cpp:938-1010`): Level für ALLE Knoten
  vorab sequentiell mit Seed 42 (identischer RNG-Strom → identische Level),
  dann Knoten `1..N-1` als kontige Chunks auf Threads; ein `mutex` pro
  Knoten-Link-Vektor + `entry_mtx` + `atomic` Fortschritt; nie zwei
  Knoten-Locks gleichzeitig (deadlockfrei); Flach-Buffer in Parallelphase
  read-only/groessenstabil.
- Determinismus: gleiche Level + gleicher Algorithmus, aber Shrinks per
  Mutex serialisiert mit scheduling-abhaengiger Reihenfolge → bei exakten
  Distanz-Ties minimale Tie-Order-Abweichung möglich: Recall-Ziel ≥
  Single-Stand, KEINE Bit-Identitaet (`hnsw.h:178-182`).
- Fallback (`hnsw.cpp:981`): `n_threads<=1` oder `N < 512`
  (`kParallelMinN`) → exakt Single-Pfad; `0` = `hardware_concurrency`
  (min 4).
- Speedup (Erwartung, keine Messung): bei grossen N (≫512) und
  Core-Zahl `t` naeherungsweise `~t`-fach im Insert-Teil (Mutex nur kurz
  um Link-Listen, Distanz auf stabilem Buffer); Single-Thread-Fallback
  darunter ohne Regression.

### 10.8 Extended-Protokoll minimal, parameterlos (`server/pgserver.h`, `src/server/pgserver.cpp`)

- Pro Connection Statements+Portale (`pgserver.h:10-14`,
  `pgserver.cpp:265-272`): `P → '1'` (ParseComplete, Query gespeichert),
  `D(S/P) → T/n` ohne Execute (Projektions-Analyse Spalten+OIDs),
  `B → '2'` (Portal, nur ohne Parameter), `E → T/D/C` wie Q-Pfad,
  `S → Z`, `C(S/P) → '3'`.
- Mit Parametern (`$n`/Bind-`nParams>0`) → `E 0A000 + Z` (`pgserver.h:14`,
  `pgserver.cpp:930-997`); Parser/Codec-Guards
  (`pgserver.cpp:285-385`: `Extended: ...`); Q-Sonderpfad `SELECT 1`
  byte-identisch erhalten.

### 10.9 Runbook-Ops (Welle 11–12)

- Bench: `dbbench --tpch [N]` (Default 10000, Seed 42) + `sql_q1`-Modus
  (`bench.cpp:15,289,361`); CSV `tpch_q1/tpch_q6` auf stdout, Summen/Hash
  nach stderr; `test_bench`-kompatibel.
- Audit: `append_audit`/`read_audit`/`parse_audit` sind WAL-local, kein
  Daemon/Sidecar; Compliance-Hinweis aus 10.4 in Ops übernehmen (kein
  Tamper-Schutz, `checkpoint()` kann Events verwerfen).
- Backup/Restore/Metrics/Auth/TLS unverändert Kap. 9.5/9.6/9.10 +
  `docs/RUNBOOK.md` Kap. 7–10; COL2-Files sind `part-*.col`-kompatibel
  (Manifest-Zeilen unverändert, `Load` frisst COL1+COL2).

### 10.10 Stale-Korrekturen zu Kap. 1/8/9 (mit Code-Beleg Datei:Zeile)

- Kap. 1/6 „Mini-SQL nur `CREATE/INSERT/SELECT`" stale → korrekt:
  `UPDATE/DELETE/DROP TABLE` + ein `INNER JOIN` vorhanden
  (Beleg: `include/dbengine/sql/parser.h:13-20`,
  `src/sql/executor.cpp:330,411,474`, `src/sql/parser.cpp:2042`).
- Kap. 1/8.4 „COL1-File" stale → korrekt: `Save` schreibt `COL2`,
  `Load` liest `COL1` (Fallback) UND `COL2`
  (Beleg: `include/dbengine/columnar/store.h:127-128`).
- Kap. 9.4 „Extended/COPY → `E(0A000)/Z`" stale → korrekt: Extended
  minimal parameterlos `P/B/D/E/S/C` vorhanden (`P→1, B→2, S→Z, C→3`),
  nur parametrisierte Pfade + `COPY`/Unbekannt → `E(0A000)`
  (Beleg: `include/dbengine/server/pgserver.h:10-14`,
  `src/server/pgserver.cpp:265-272,930-997`).
- Kap. 8.1 „`build()` deterministischer Neuaufbau" unvollständig →
  korrekt: `build()` = `build_parallel(1)`; `build_parallel` mit
  Seed-42-Vor sampling, `kParallelMinN = 512`-Fallback, keine
  Bit-Identitaet bei Ties
  (Beleg: `src/vector/hnsw.cpp:723,938-981`,
  `include/dbengine/vector/hnsw.h:168-185`).
- Kap. 5 „HNSW-lite Single-Layer / SQ8-Stub" historisch → aktiv:
  mehrschichtig (8.1), aktive Suche via `quant.h` (SQ8/PQ-N/IVF, 8.2/9.8),
  SoA-Primärspeicher + `thread_local`-`get()` (10.3)
  (Beleg: `include/dbengine/vector/hnsw.h:33-45,283-286`,
  `src/vector/hnsw.cpp:1021-1030`).

## 11. Phase 0 (s72–s77, alle STL/POSIX-only)

### 11.1 Single-File-Container — Magic/Regionen (`db.h`, `src/db.cpp`)

- Format fix, little-endian (`include/dbengine/db.h:5-18`):
  `magic[6]="DBEN01"` @0, `version u32=1` @6, Region-Table @10
  (4× `{offset u64, size u64}`: `[catalog, wal-seg, pager, meta]`),
  `crc32 u32` @74 über Bytes `[0,74)` (IEEE `0xEDB88320`, Seed 0),
  Header total `kHeaderSize=78`.
- Body lückenlos, genullt reserviert (`include/dbengine/db.h:41-48`):
  `catalog @78/4096`, `wal-seg @4174/16384`, `pager @20558/16384`,
  `meta @36942/4096`, Datei total `kFileSize=41038`.
  Inhalte in s72 NUR reserviert+genullt, Befüllung erst s73/s74
  (`include/dbengine/db.h:16-18`).
- `build_header` serialisiert 78 Bytes inkl. CRC (`src/db.cpp:82-103`);
  `create_new` via `tmp+fsync+rename+dir-fsync`, nie torn-mix
  (`src/db.cpp:110-145`); `Db::open` erzeugt falls fehlend, sonst
  validiert Magic/Version/Region-Table (lückenlos, sortiert, im File)/
  CRC/Größe, Fehler → Exception ohne Teilzustand (`src/db.cpp:202-256`).

### 11.2 BTreeKV — Fanout/Split/Shadow-Paging, LoadAll-Loch-Fix (`kv/btree.h`, `src/kv/btree.cpp`)

- CLRS-B-Baum, Mindestgrad `t=32` (`src/kv/btree.cpp:26-29`,
  `include/dbengine/kv/btree.h:21-29`): max. `2t-1=63` Keys /
  max. `2t=64` Kinder je Knoten (Fanout 64), `t-1..2t-1` Keys außer
  Wurzel, gleiche Blatttiefe, Keys sortiert, Keys+Values in inneren
  Knoten wie Blättern (kein B+-Overhead, Range via In-Order).
- Split beim Abstieg (Median steigt auf, `src/kv/btree.cpp:500-525`),
  Löschen mit Auffüllen (Ausleihen `BorrowFromPrev/Next` ab `>=t`,
  sonst `MergeChild`, leere Wurzel schrumpft via `ShrinkRoot`,
  `src/kv/btree.cpp:527-663`).
- Persistenz Shadow-Paging/CoW (`include/dbengine/kv/btree.h:31-44`):
  Knoten als wenige-KiB-Records im Pager-Image (Superblock unter
  Pager-Key 0, Knoten-Id n unter Key n, Ids ab 1), Mutationen erst RAM,
  `Flush()` schreibt dirty Knoten + kippt Superblock
  (`root/next/count/seq`) in EINEM `Pager::insert` (tmp+rename+fsync
  → atomar, `src/kv/btree.cpp:776-793`); `WriteBatch` validiert zuerst
  (alles-oder-nichts) + flusht automatisch; Einzel-Put/Delete erst nach
  `Flush()/Write()/Close()` crash-fest. Absichtlich NICHT Raw-Page-API
  (`include/dbengine/kv/btree.h:46-50`).
- Serialisierung: Knoten `magic(8)=BTREEPG1/ver(4)/leaf/n/keys/vals/
  childs/checksum(FNV-1a64)` (`src/kv/btree.cpp:103-159`), Superblock
  `BTREESB1/root/next/count/seq/checksum`, Freelist nicht persistiert
  (GC-Rekonstruktion, `src/kv/btree.cpp:161-202`).
- LoadAll-Loch-Fix (`src/kv/btree.cpp:701-774`): IDs ohne Pager-Record
  sind freigegebene, nie persistierte Knoten (`FreeNode` vor Flush) →
  `continue`, kein Fehler, bleiben frei (`src/kv/btree.cpp:728-734`);
  danach Erreichbarkeit ab Wurzel + `reachable_keys==count`-Abgleich
  (`src/kv/btree.cpp:739-761`); Waisen-GC löscht Reste abgebrochener
  Commits, IDs wiederverwendbar (`src/kv/btree.cpp:762-772`);
  `next`-Schranke gegen korrupte Werte (`src/kv/btree.cpp:719-722`).

### 11.3 CLI — open/Replay/REPL (`src/main.cpp`)

- Aufruf `prog <db-datei> [--exec "SQL;..."] [--sql datei.sql]...`,
  ohne Flags REPL auf stdin, `--help/--version` (`src/main.cpp:24-35,267-320`).
- Statement-Split per `;`, Quote-bewusst (`'...'`+`''`-Escape,
  `"..."`+`""`-Escape, Parser = ein Statement/Aufruf,
  `src/main.cpp:92-139`); REPL-Mehrzeiler via `popComplete`,
  `.quit/.exit`, leere Zeilen skip, SQL-Fehler drucken+weiter, EOF-Rest
  ausführen (`src/main.cpp:141-188,231-263`).
- Ablauf: `Db::open` (erzeugt falls fehlend, Fehler→exit 1,
  `src/main.cpp:322-335`) + `KVStore+MvccStore+Wal("<db>.wal")`
  (`src/main.cpp:337-349`) + Mirror-Sidecar `"<db>.btree"` tolerant
  (`enableMirror`, fehlend/korrupt→Voll-Replay+Warnung,
  `src/main.cpp:350-370`) + `recover()` (Skips→Warnung,
  `src/main.cpp:371-380`); Batch-Modus exit 1 bei ≥1 SQL-Fehler, REPL
  bleibt 0 (`src/main.cpp:383-392`).
- Persistenz-Modell ehrlich im `--help` (`src/main.cpp:39-55`): `<db>`
  NUR Container (Magic+Version+Region-Table+CRC), SQL-Daten NICHT im
  Body; Schema+Rows pro Statement ins WAL (`<db>.wal`, flush pro
  Statement), Restart NUR via WAL (ohne WAL leer, Verlust=Verlust,
  Torn-Tail→Prefix gewinnt); Mirror siehe 11.4.
- Ausgabe: `SELECT`→Tabelle (`|`-Trenner, `valueToString`),
  sonst Executor-Message, Fehler→stderr `ERROR: ` (`src/main.cpp:190-221`).

### 11.4 Mirror-Checkpoint — B-Tree-Spiegel, Batch-Protokoll, Restart (`sql/executor.h`, `src/sql/executor.cpp`)

- Format (`src/sql/executor.cpp:593-620`): `BTreeKV`-Sidecar
  (`<db>.btree`) als Latest-State: Row-/Schema-Keys 1:1 wie KVStore
  (`sql/<tabelle>/<pk>`, `sql/__schema/<tabelle>`, gleiche Codec-Helper),
  Meta-Key `"\0mirror/lsn"` (NUL→keine Kollision, kein Prefix-Scan)
  = dezimale durable-LSN bis zu der der Spiegel den WAL abbildet.
- Protokoll inkrementell: Statements spiegeln gerade geschriebene
  Payloads via `syncMirrorBatch(O(Batch))` ohne WAL-Re-Read;
  `recover()`-Catch-up via `syncMirrorFromWal(Vollscan, selten)`
  (`src/sql/executor.cpp:601-607`); danach `mirror_lsn=durable` in
  EINEM `BTreeKV`-Flush (Shadow-Paging→atomar, Crash davor=alter
  Spiegel+alte LSN=konsistent, Tail-Replay holt Rest,
  `src/sql/executor.cpp:604-607,837-853`).
- Laden (`src/sql/executor.cpp:622-730`, `include/dbengine/sql/executor.h:54-68`):
  Sidecar öffnen/erzeugen, LSN parsen (korrupt→degradiert `lsn=0`),
  Pass 1 Schemas, Pass 2 nur Rows bekannter Tabellen (Phantom-Schutz),
  chunkweise `4096` in KV (`WriteBatch`)+MVCC (frische Single-Version-
  Ketten, committed, ohne aktive Txns sichtbar)+Replika; fremde Keys/
  Fehler→degradiert, WAL bleibt Wahrheit (Voll-Replay, last-wins,
  nie Datenverlust durch Spiegel).
- Replay mit Spiegel (`src/sql/executor.cpp:1685-1703`): nur Tail
  `read_from(mirror_lsn+1)` falls `mirror_on+open+lsn>0`, sonst Voll-
  `replay()` (+Fallback bei `read_from`-Fehler); danach
  `syncMirrorFromWal` als Checkpoint-Catch-up
  (`src/sql/executor.cpp:1844-1846`); Seiteneffekt 1:1 inkl. Skip-Regeln
  via `applyMirrorRecord` (`src/sql/executor.cpp:749-797`).
- Restart-Zahlen (Duellbericht `docs/PHASE0-DUELL.md`, untrennbar
  Replay+Neuaufbau+Scan, 1M, tmpfs, Batch=1): dbengine-Neustart
  `~6,1–6,3 s` (Lauf A `6,120/6,098 s`, Lauf B `6,230/6,301 s`) vs.
  SQLite-Reopen+COUNT+SUM `~0,05 s`; Größen `<db>=41 038 B` (leer,
  nur Container) + `<db>.wal=53 666 726 B (~51,2 MiB)` vs. SQLite
  `15 024 128 B (14,33 MiB)` — Details/Vergleich siehe 11.6, nichts
  extrapoliert.

### 11.5 Autotune-Formel + 100k-Recall (`vector/hnsw.h`, `vector/quant.h`)

- HNSW-`auto_ef` (`include/dbengine/vector/hnsw.h:150-167`,
  `src/vector/hnsw.cpp:241-265`): `ef_auto=ceil(k*sqrt(N)/10)`,
  effektiv `min(max(max(ef_default,k),ef_auto),1024,N)`; `/10` auf
  `N~=1k,k=10` kalibriert (`~=32`=alter Default, kein Klein-Overhead),
  `ef_default`=Floor/Override, explizites `ef>0` gewinnt immer;
  `ef<=0`→Autotune (`include/dbengine/vector/hnsw.h:215-216`).
- Filter-`autotune_ef` (`include/dbengine/vector/quant.h:260-273`,
  `src/vector/quant.cpp:753-779`): `ef=ceil(k/sel*2.0)`,
  `sel` auf `[1e-4,1.0]` geclampt, `[max(k,ef_min=32),ef_max=1024]`,
  Cap `n`; Komfort `autotune_ef_for_filter(k,matched,total)`
  (`src/vector/quant.cpp:772-779`); N-unabhängig (sel=1→2k), daher für
  UNGEFILTERT zusätzlich `auto_ef`-sqrt-Pfad nutzen.
- IVF-`autotune` (`include/dbengine/vector/quant.h:157-168`,
  `src/vector/quant.cpp:410-436`): `nlist~=4*sqrt(N)` (`[1,4096]`, `<=N`),
  `nprobe~=nlist/8` (aufgerundet, `[1,nlist]`); Groß-Pfad ab
  `N>=kHnswToIvfThreshold=50k` IVF statt HNSW
  (`include/dbengine/vector/quant.h:244-258`).
- 100k-Recall (uniform, k=10, fixer ef): `0,94@2k → 0,78@8k → 0,34@100k`
  (`include/dbengine/vector/quant.h:244-249`,
  `src/vector/hnsw.cpp:241-245`); Erwartung Autotune (k=10):
  `2k→ef~45 Recall~0,94+`, `8k→ef~90 Recall~0,9`,
  `100k→ef~317 Recall>=0,8` bei ~10x Beam-Latenz
  (`include/dbengine/vector/hnsw.h:162-166`); Latenz/Speicher groß
  trotzdem IVF (`include/dbengine/vector/quant.h:250-254`).

### 11.6 Duell-Ergebnisse 1M — Insert/Restart vs. SQLite (`docs/PHASE0-DUELL.md`, Zahlen übernommen)

- Setup (nichts erfunden): `CREATE TABLE t (id INT, val INT)` +
  `1..1M INSERT (i,i)`, `COUNT=1000000`, `SUM=500000500000`
  (dbengine-Anzeige `5e+11` = Display-Format, dokumentiert);
  Batch=1 (Einzel-INSERT/Statement, dbengine 1M `flush()`=1M fdatasyncs
  ohne Group-Commit über Grenzen, SQLite Autocommit 1 Commit/INSERT,
  `synchronous=FULL`, `journal_mode=delete`, `page_size=4096`);
  dbengine-Scan `SELECT SUM(val)`, SQLite COUNT+SUM in einer Messung
  (2 Voll-Scans, Asymmetrie zugunsten dbengine, urteilsneutral);
  alles `/tmp` = tmpfs (fsync fast gratis, NICHT auf Platte übertragbar).
- 10k-Probe (Hochrechnungs-Basis): dbengine `0,056/0,057 s` Insert,
  `0,054 s` Restart+Scan; SQLite `0,753 s` Insert, `0,001 s` Reopen+Scan;
  Hochrechnung `~6 s` vs. `~75 s` (<8 min → 1M ehrlich gemessen, je 2 Läufe).
- 1M Insert: dbengine A `6,681 s`, B `6,705 s` (fresh DB, 1M Einzel-INSERTs,
  38 777 826-B-SQL); SQLite `75,086 s` / `75,460 s` → dbengine `~11×`
  schneller (`~6,7 s` vs. `~75,3 s`).
- 1M Restart+Scan: dbengine A `6,120/6,098 s`, B `6,230/6,301 s`
  (neuer Prozess: WAL-Replay 1M + In-Memory-Neuaufbau + Scan untrennbar);
  SQLite `0,048/0,045 s` + `0,048/0,044 s` → SQLite `~130×` schneller
  (`~6,2 s` vs. `~0,05 s`).
- Bytes persistent: dbengine `41 KB` Container + `53,7 MB` WAL
  (Rows nur im WAL, ohne WAL leer/Totalverlust) vs. SQLite `14,3 MB`
  echte DB (B-Tree/Pager). Urteil Duellbericht: kein Gesamtsieger
  (Schreibdurchsatz vs. Restart-/Lesekosten); Engpass dbengine=Replay,
  Engpass SQLite=Insert (FULL+Autocommit ~13k vs. ~150k Rows/s nur dank
  tmpfs `~7 µs`/fdatasync); nicht gemessen: echte Disk, Batch>1,
  Transaktions-Batching, Index-Scans.

### 11.7 Stale-Korrekturen zu Kap. 1/2/5/6/9 (mit Code-Beleg Datei:Zeile)

- Kap. 6 „Pager Single-File, Rows im Image" unvollständig → korrekt:
  Phase-0-CLI-`<db>` ist Db-Container (`41038 B`, Regionen genullt,
  keine Rows), SQL-Rows leben in `<db>.wal` (+`<db>.btree`-Spiegel);
  ohne WAL startet DB leer (Beleg: `src/main.cpp:39-55`,
  `include/dbengine/db.h:46-48`, `src/main.cpp:337-349`).
- Kap. 6 „Index Secondary B-Tree via multimap (Stub)" ≠ BTreeKV →
  korrekt: Abgrenzung — Secondary-Index bleibt `multimap`/TTL
  (Beleg: `include/dbengine/index/btree.h:11-15`), Phase-0-Herzstück
  `kv::BTreeKV` ist persistenter B-Baum (Fanout 64, Shadow-Paging)
  (Beleg: `include/dbengine/kv/btree.h:21-44`).
- Kap. 2/9.7 „Replay = Voll-Replay" unvollständig → korrekt: mit
  Spiegel nur Tail `lsn>mirror_lsn`, sonst Voll-Replay; WAL bleibt
  Wahrheit (Beleg: `src/sql/executor.cpp:1689-1703`,
  `include/dbengine/sql/executor.h:15-21`).
- Kap. 5/9.8 „fixer `ef_default=32`" historisch → korrekt: `search`
  `ef<=0` = `auto_ef(N)` (`ceil(k*sqrt(N)/10)`, Cap 1024), Filter-ef
  via `autotune_ef(_for_filter)` (Beleg:
  `src/vector/hnsw.cpp:249-265`,
  `include/dbengine/vector/hnsw.h:150-167,215-219`,
  `src/vector/quant.cpp:754-779`).

### 11.8 Runbook-Ops (Phase 0)

- CLI: `dbengine foo.db --sql init.sql --exec "SELECT COUNT(*) FROM t;"`,
  `dbengine foo.db --exec "SELECT SUM(val) FROM t;"` (Restart misst
  Replay+Scan, `src/main.cpp:27-35`); REPL `dbengine foo.db` + `.quit`.
- Dateien nebeneinander: `foo.db` (Container), `foo.db.wal` (Wahrheit),
  `foo.db.btree` (Spiegel, tolerant, heilt via Voll-Replay,
  `src/main.cpp:350-380`); WAL-Verlust=Totalverlust, Spiegel-Verlust
  nur Tail-Replay (11.4).
- Vektor-Groß: `N>=50k` `prefer_ivf_over_hnsw` + `IvfPqIndex::autotune(N)`
  (`include/dbengine/vector/quant.h:255-258,166-168`); HNSW klein mit
  Default-`ef` (Autotune, keine Flags nötig).
