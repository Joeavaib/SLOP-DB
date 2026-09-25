# Anforderungsliste DB Engine – Synthese aus 10-Fach-Recherche (Stand 09/2026)

Quelle: 10 parallele Web-Recherchen zu: Relational (PG/MySQL), Embedded (SQLite/DuckDB/LMDB/RocksDB), KV/In-Memory (Redis/Dragonfly/FoundationDB), Document (Mongo/Couch/Raven), Wide-Column (Cassandra/Scylla/HBase), Graph (Neo4j/Arango/TigerGraph/Kuzu), Time-Series (Influx/Timescale/QuestDB), Vektor (Milvus/Qdrant/Weaviate/pgvector/LanceDB), Distributed SQL (Cockroach/TiDB/Yugabyte/Spanner), OLAP Columnar (ClickHouse/Doris/Snowflake/DuckDB).

## 1. Ziel / Produktvision
Eine moderne, erweiterbare DB Engine, die OLTP + OLAP + Vektor + Time-Series in einem Kern vereint (HTAP+AI), mit:
- Embedded-Start (wie SQLite/DuckDB) und horizontaler Skalierung (wie FoundationDB/Spanner)
- PostgreSQL-Kompatibilität als Adoption-Hebel
- Offener Lizenz (Apache-2.0 / MIT / PG-License) – kein SSPL/BSL-Lock (Learning aus Redis, Mongo, Cockroach, Scylla, Arango)

Nicht-Ziel V1: 100% Feature-Parität mit allen 10 Kategorien. Ziel V1: solider Single-Node-Kern + Cluster-Pfad.

## 2. Funktionale Anforderungen

### F1 Datenmodelle (Multi-Model, aber geschichtet)
- F1.1 Relational als Basis: Tabellen, PK/FK, Secondary Index, JSONB (PG-kompatibel)
- F1.2 KV-API nativ (Get/Put/Delete/Scan/Iterator) als unterste Schicht (Vorbild RocksDB/FoundationDB)
- F1.3 Document: JSON/BSON, Multikey/GIN-Index, SQL++-ähnliche Abfrage, kein reines MQL-Silo
- F1.4 Time-Series: `TIMESTAMP`-Partitionierung, Chunks/Hypertable, Retention, Downsampling/Continuous Aggregates, `SAMPLE BY`/`ASOF JOIN`, Prometheus Remote Write + Line Protocol Ingest
- F1.5 Graph (V2): Property Graph, CSR-Adjazenz + Spaltenstore, GQL (ISO/IEC 39075:2024) ab Tag 1, Co-Location-Strategie (SmartGraphs-Ansatz)
- F1.6 Vektor first-class: `VECTOR/HALFVEC/BIT/SPARSE`, filterbarer HNSW + IVF-PQ + DiskANN, Hybrid Search dense+sparse+BM25+RRF, Quantisierung RQ/PQ/BQ + Disk-Tiering

### F2 Storage Engine (pluggable, 2 Engines + 1 Log)
- F2.1 Row-Store: CoW B+Tree (LMDB/Redwood-Vorbild) für OLTP Point-Lookups, 16KB Pages, Zero-Copy mmap Reads
- F2.2 Columnar-Store: Immutable Parts + MergeTree/Parquet + Object-Storage (S3), Dict/RLE/ZSTD, Zonemaps/Bloom/MinMax, Vektorisierung 2048+ SIMD (Vorbild ClickHouse/DuckDB/Influx IOx)
- F2.3 LSM/KV-Layer für Write-Heavy Ingest: WAL + MemTable -> SST, leveled/universal Compaction wählbar, Tombstone-GC (Vorbild RocksDB/Cassandra)
- F2.4 Ein einziges WAL + Checkpoint + Snapshots für alle Engines (Learning PG: WAL+Logical Decoding nicht verdoppeln, MySQL Redo+Binlog vereinen) mit CDC-Slots (LSN/GTID-Wait, `WAIT FOR LSN`)
- F2.5 Single-Writer pro Shard + MVCC (Undo-Log statt Heap-Bloat, Learning InnoDB vs PG Vacuum), Snapshot-Isolation default, Serializable opt-in (SSI)

### F3 Transaktionen & Konsistenz
- F3.1 ACID default Single-Shard/Doc, Multi-Shard opt-in (Percolator/2PC)
- F3.2 Isolation: Read Committed, Repeatable Read, Snapshot, Serializable (SSI/OCC+MVCC wie FDB)
- F3.3 Tunable Consistency pro Query: `ONE / QUORUM / ALL / LOCAL_QUORUM` + Strong/Stale/Follower-Reads (Learning Cassandra + Distributed SQL)
- F3.4 Externe Konsistenz ohne Atomuhren: HLC + TSO (PD-Oracle) + Commit-Wait, Lock-freie RO-Snapshots global
- F3.5 Tx-Limits explizit dokumentieren (z.B. 10MB/5s wie FDB), Single-Statement Atomic DDL + Online-DDL (`INSTANT/INPLACE`, `REPACK CONCURRENTLY`)

### F4 Indexe & Query-Optimizer
- F4.1 B-Tree (row), ART (in-memory/embedded), GIN/GiST/BRIN, Inverted/FTS, HNSW/IVF (vektor), CSR (graph), TTL/Geo
- F4.2 CBO (Nereids-like), Hypergraph-Joins, Runtime-Filter, Pipeline-Engine, Worst-Case-Optimal-Joins für Graph/OLAP
- F4.3 Time/Vektor-Pushdown: Partition-Pruning, Order/Filter-Pushdown in Index, `ef/lists/probes` zur Laufzeit tunbar

### F5 Replikation / Sharding / HA
- F5.1 Schichtentrennung: SQL -> Txn -> Distribution -> Replication -> Storage (Cockroach-Modell)
- F5.2 Shard = Consensus-Gruppe: Raft pro Range/Tablet (512MB bzw. 96MB/5GB Tablet dynamisch), Auto-Split/Merge, Placement-Rules
- F5.3 RF=3 + `LOCAL_QUORUM` default, Active-Active Multi-DC via Hinted Handoff + Read-Repair, async XDCR + sync Option
- F5.4 Trennung Compute/Storage + stateless Router/Querier/Ingester/Compactor, Shared Storage (S3) + lokaler Cache (Learning Influx3/Snowflake/ClickHouse Cloud)
- F5.5 Failover in-Core (kein Patroni-Zwang), RPO 0, RTO <30s (lokal <3s), Leader-Affinität + Follower-Reads

### F6 Schnittstellen
- F6.1 Primär: PostgreSQL Wire (libpq) + MySQL-Protokoll-Subset, Standard-SQL + Time-Extensions + Vektor-Ops (`<->,<#>,<=>`) + `MATCH` (GQL)
- F6.2 Sekundär: RESP (Redis-kompatibel für KV/Cache-Migration), REST/gRPC + Arrow FlightSQL + QWP/Parquet-Export (220M rows/s Ziel), S3/Parquet/Iceberg Direkt-Query
- F6.3 SDKs: C/C++/Python/Rust/Go/TS/Java, Embedded-Lib <30MB (DuckDB-Maß), WASM-Build
- F6.4 Observability: `pg_stat_statements`-Äquivalent, OpenTelemetry, Prometheus-Endpoint

## 3. Nicht-funktionale Anforderungen

- N1 Performance SLOs (Single-Node, NVMe, 16 Cores):
  - Point-Lookup p99 <1ms (KV), <5ms (SQL PK)
  - Ingest >500k rows/s (row), >5M rows/s (time-series/QWP, QuestDB-Maß)
  - OLAP Scan >5GB/s/core vektorisiert, TPC-H SF10 <30s embedded
  - Vektor: HNSW Recall@10 >0.95 bei p95 <10ms bis 5M Vektoren, danach sharded linear
  - Write-Amplification <3x, Read-Amplification via Bloom/Cache <1.2x
- N2 Skalierbarkeit: vertikal 1 Core->100+ Cores (Morsel-Parallelismus, Thread-per-Core + Fibers, kein Single-Thread-Bottleneck), horizontal 1->100 Nodes linear read, write mit Quorum-RTT dokumentiert
- N3 Verfügbarkeit: 99.99% regional, 99.999% multi-region, kein Split-Brain (Jepsen/Raft-Simulationstests Pflicht wie FDB)
- N4 Ressourcen: Embedded <100MB RAM idle, Cloud-Native mit Tiering (RAM->NVMe->S3), 50% kleiner als Parquet (Lance-Maß), Kompression 10-100x für TS
- N5 Bedienbarkeit: Zero-Ops Start (`single binary`, `initdb` in <10s), VFS-Abstraktion, Auto-Tuning für Compaction/Cache statt Manual-Tuning (RocksDB-Anti-Pattern vermeiden)
- N6 Sicherheit: TLS 1.3 + PQ-TLS-Ready, Auth OAuth2/WebAuthn + RBAC + Row-Level-Security, Audit-Log, Verschlüsselung at-rest (KMS) + per-Table Keys, PII-Masking
- N7 Compliance/Lizenz: Apache-2.0 (Kern) + PG-License-kompatibel, kein SSPL/BSL im Kern, Enterprise-Features klar getrennt, CLA/DCO geregelt

## 4. Architektur-Leitplanken (aus Learnings)
1. Versionen von Heap trennen (Undo-Log), Autovacuum/Compaction-Observability ab Tag 1
2. Erweiterbarkeit = Ökosystem: Extension-API (PG-Modell) > Storage-Engine-API
3. Storage/Compute trennen, Log zentral, Compaction async im Hintergrund
4. OLTP (Zeiger/Row) und OLAP (vektorisiert/columnar) als zwei Engines, ein Log (HTAP-Lernsatz Graph + TiFlash)
5. Simulationstests (deterministisch, FDB-Vorbild) für Raft/Txn vor Performance-Tuning
6. SQL-first + RESP/PGWire-Kompatibilität für Adoption, kein proprietäres Query-Silo

## 5. MVP-Phasen
- Phase 0 – Embedded KV+Row (M1-3): B+Tree + WAL + MVCC + PGWire read/write, Single-Writer ACID, Benchmark vs SQLite
- Phase 1 – Columnar + Index (M4-6): Parquet-Parts, Vektorisierung, GIN/HNSW-basic, Hybrid Filter, Benchmark vs DuckDB/pgvector <5M
- Phase 2 – Distributed (M7-12): Raft-Sharding, TSO/HLC, Follower-Reads, K8s-Operator, Jepsen-Pass, Benchmark vs TiDB/Cockroach Single-DC
- Phase 3 – Spezial (M12+): TS-Retention/Downsampling, Graph-CSR+GQL, Object-Storage-Tiering, Redis/Cassandra-Compat-Layer

## 6. Akzeptanz / Verifikation
- A1 ACID-Tests (Hermitage) + Jepsen (partition, clock-skew) grün
- A2 TPC-C (OLTP), TPC-H SF10 (OLAP), LDBC (Graph, falls V2), ANN-Benchmarks (Recall/Latenz), TSBS (Ingest) dokumentiert
- A3 Crash-Recovery <30s bei 100GB, kein Datenverlust bei Kill -9 (WAL-Replay-Test)
- A4 Upgrade Online-DDL ohne Downtime, Rollback-Fähigkeit
- A5 Lizenz-Check (FOSSA/REUSE), SBOM, SLSA-Build

## 7. Offene Entscheidungen (für Product-Owner)
- O1 Primär-Dialekt: PG 100% vs PG+MySQL-Dual? Empfehlung: PG-first (Ökosystem pgvector/PostGIS/FDW)
- O2 Vektor-Default: HNSW RAM-first vs DiskANN-first? Empfehlung: filterbarer HNSW + PQ/BQ + Auto-Spill (Qdrant-Kompromiss)
- O3 Graph in V1 oder V2? Empfehlung V2 (Kuzu-embedded zuerst, verteilt später)
- O4 Cloud-Monetarisierung ohne Lizenzfalle: Managed-Hosting + Enterprise (RBAC/Cold-Storage) proprietär, Kern Apache-2.0 (Valkey-Lesson)
