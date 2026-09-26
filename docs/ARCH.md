# dbengine — Architektur (V2, embedded-first)

> Stand: 09/2026. Ziel: OLTP+OLAP+Vektor+TS in einem Kern, Start embedded
> (SQLite/DuckDB-Vorbild), Cluster-Pfad vorbereitet. Details siehe
> `ANFORDERUNGEN.md` (F1–F6, N1–N7, Phasen 0–3).
>
> V1 (s01–s20): Row-Kern + Stubs. V2 (s21–s26): echte Implementierungen —
> siehe Kapitel 8 (HNSW-multi, PQ-N/IVF, WAL-Group-Commit, COL1-File, Raft-TCP).
> Welle 8–10 (s38–s52): SQL-Aggregate/Session/Filter, Metrics/Backup/Kill-9,
> Pager-Atomaritaet, HNSW/IVF-Skala, Auth/TLS — siehe Kapitel 9.

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
  - `electLeader()`: erster lebender Knoten, Term+1, Votes aller Lebenden,
    Mehrheit nötig, sonst `-1` (kein Quorum bei ≤1 lebend).
  - `append(cmd)`: nur Leader, `replicateToFollowers` synchron, Commit bei
    Mehrheit, `apply()` parst Command in KV-Map. Rückgabe Log-Index, `0` bei
    Fehler (kein Leader / tot / kein Quorum).
  - `killLeader()` + `failover()`: deterministische Neuwahl unter Lebenden
    (in-process µs, Sim-Ziel <100ms). `killNode/reviveNode` mit Catch-up + apply.
- V1-Grenzen (s25 geschlossen, Sim bleibt): persistentes Raft-Log via
  `SaveLog/LoadLog` (`RAFT1`), Snapshots via `SaveSnapshot/LoadSnapshot`
  (`RSNP1` + `log_base_`-Compaction), TCP-Framing via Wire-Codec
  (`Send/RecvWire`, `TcpLoopbackPair`), Follower-Reads (`follower_get`,
  `is_caught_up`), Split/Merge-Range-Ops. Weiter Sim: keine Membership-Change,
  keine echten Follower-Reads über Netzwerk.
  Failover-Pfad ist testabgedeckt (`test_raft`).

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
