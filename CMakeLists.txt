cmake_minimum_required(VERSION 3.20)
project(dbengine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(dbengine src/main.cpp)
target_include_directories(dbengine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

enable_testing()

add_executable(smoke tests/test_smoke.cpp)
target_include_directories(smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(smoke PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME smoke COMMAND smoke)

# s06-sql: Mini-SQL + PGWire-Stub
add_library(sql STATIC src/sql/parser.cpp src/server/pgwire.cpp)
target_include_directories(sql PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(sql PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_sql tests/test_sql.cpp)
target_link_libraries(test_sql PRIVATE sql)
target_compile_options(test_sql PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME sql COMMAND test_sql)

add_library(pager src/storage/pager.cpp)
target_include_directories(pager PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(pager PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_pager tests/test_pager.cpp)
target_link_libraries(test_pager PRIVATE pager)
target_compile_options(test_pager PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME pager COMMAND test_pager)

# s10-columnar: Immutable Parts + Pruning, kein OLTP-Retrofit
add_library(columnar src/columnar/store.cpp)
target_include_directories(columnar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(columnar PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_columnar tests/test_columnar.cpp)
target_include_directories(test_columnar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_columnar PRIVATE columnar)
target_compile_options(test_columnar PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME columnar COMMAND test_columnar)

# s07-index: SecondaryIndex (B-Tree via multimap) + TTL + ART/GIN-Stubs
add_library(dbindex STATIC src/index/btree.cpp)
target_include_directories(dbindex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(dbindex PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_index tests/test_index.cpp)
target_link_libraries(test_index PRIVATE dbindex)
target_include_directories(test_index PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_test(NAME index COMMAND test_index)

# s09-hybrid: BM25-lite + dense-Fusion (RRF/Weighted), unabhaengig von s08
add_library(hybrid STATIC src/search/hybrid.cpp)
target_include_directories(hybrid PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(hybrid PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_hybrid tests/test_hybrid.cpp)
target_link_libraries(test_hybrid PRIVATE hybrid)
target_include_directories(test_hybrid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_test(NAME hybrid COMMAND test_hybrid)

# s03-wal: REDO-Log + Checkpoint + Recovery (Crash-Safety via flush/fsync)
add_library(dbengine_wal STATIC src/storage/wal.cpp)
target_include_directories(dbengine_wal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(dbengine_wal PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_wal tests/test_wal.cpp)
target_link_libraries(test_wal PRIVATE dbengine_wal)
target_include_directories(test_wal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_test(NAME wal COMMAND test_wal)

# s08-vector: filterbarer HNSW-lite (Layer0, M=16, ef runtime-tunbar) + Brute-Force
add_library(dbengine_vector STATIC src/vector/hnsw.cpp)
target_include_directories(dbengine_vector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(dbengine_vector PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_vector tests/test_vector.cpp)
target_link_libraries(test_vector PRIVATE dbengine_vector)
target_include_directories(test_vector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_vector PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME vector COMMAND test_vector)

# s05-kv: native KV-API (Get/Put/Delete/Scan/Iterator, WriteBatch atomar, Snapshot)
add_library(dbengine_kv STATIC src/kv/kv.cpp)
target_include_directories(dbengine_kv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(dbengine_kv PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_kv tests/test_kv.cpp)
target_link_libraries(test_kv PRIVATE dbengine_kv)
target_include_directories(test_kv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_kv PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME kv COMMAND test_kv)

# s04-mvcc: Snapshot-Isolation, Undo-Log-Ketten, Single-Writer pro Shard
add_library(dbengine_mvcc STATIC src/txn/mvcc.cpp)
target_include_directories(dbengine_mvcc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(dbengine_mvcc PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_mvcc tests/test_mvcc.cpp)
target_link_libraries(test_mvcc PRIVATE dbengine_mvcc)
target_include_directories(test_mvcc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_mvcc PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME mvcc COMMAND test_mvcc)

# s13-raft: Shard/Tablet + RaftGroup-Sim (3 Nodes in-process, kein Netzwerk)
add_library(dbengine_raft STATIC src/raft/shard.cpp)
target_include_directories(dbengine_raft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(dbengine_raft PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_raft tests/test_raft.cpp)
target_link_libraries(test_raft PRIVATE dbengine_raft)
target_include_directories(test_raft PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_raft PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME raft COMMAND test_raft)

# s12-pgserver: TCPServer (POSIX, 127.0.0.1 ephemeral) + pgwire codec
add_library(pgserver STATIC src/server/pgserver.cpp)
target_include_directories(pgserver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pgserver PUBLIC sql executor dbengine_mvcc dbengine_kv dbengine_wal)
target_compile_options(pgserver PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_pgserver tests/test_pgserver.cpp)
target_link_libraries(test_pgserver PRIVATE pgserver)
target_include_directories(test_pgserver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_pgserver PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME pgserver COMMAND test_pgserver)

# s14-clock: TSO/HLC + Commit-Wait + monoton Snapshots
add_library(dbengine_clock STATIC src/txn/clock.cpp)
target_include_directories(dbengine_clock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(dbengine_clock PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_clock tests/test_clock.cpp)
target_link_libraries(test_clock PRIVATE dbengine_clock)
target_include_directories(test_clock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_clock PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME clock COMMAND test_clock)

# s15-jsonb: Binaer-JSON Typ, GIN-Inverted-Index, Operatoren ->/->>/@>
add_library(dbengine_jsonb STATIC src/sql/jsonb.cpp)
target_include_directories(dbengine_jsonb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(dbengine_jsonb PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_jsonb tests/test_jsonb.cpp)
target_link_libraries(test_jsonb PRIVATE dbengine_jsonb)
target_include_directories(test_jsonb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_jsonb PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME jsonb COMMAND test_jsonb)

# s11-executor: SQL-Executor auf KV+MVCC+WAL (Parser, TableRegistry, Snapshot-Reads)
add_library(executor STATIC src/sql/executor.cpp)
target_include_directories(executor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(executor PUBLIC sql dbengine_kv dbengine_mvcc dbengine_wal)
target_compile_options(executor PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_executor tests/test_executor.cpp)
target_link_libraries(test_executor PRIVATE executor)
target_include_directories(test_executor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_executor PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME executor COMMAND test_executor)

# s18-bench: Bench-Harness (KV-Throughput, SQL Q1, ANN Recall/Latenz, CSV)
add_executable(dbbench tools/bench.cpp)
target_include_directories(dbbench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dbbench PRIVATE dbengine_kv dbengine_vector columnar dbengine_wal)
target_compile_options(dbbench PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_bench tests/test_bench.cpp)
target_include_directories(test_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_bench PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME bench COMMAND dbbench --smoke)
add_test(NAME bench_csv COMMAND test_bench)

# s19-chaos: deterministischer Crash-Fuzz (Seed 42, 10k Ops, WAL-torn-inject).
# ASAN/UBSAN nur fuer dieses Target (kein globaler Sanitizer).
add_executable(test_chaos tests/test_chaos.cpp)
target_link_libraries(test_chaos PRIVATE dbengine_kv dbengine_wal)
target_include_directories(test_chaos PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_chaos PRIVATE -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g)
target_link_options(test_chaos PRIVATE -fsanitize=address,undefined)

add_test(NAME chaos COMMAND test_chaos)

# s17-ts: Hypertable-Chunks + Retention + Continuous-Agg + ASOF-Join-Stub
add_library(dbengine_ts STATIC src/ts/chunks.cpp)
target_include_directories(dbengine_ts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(dbengine_ts PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_chunks tests/test_chunks.cpp)
target_link_libraries(test_chunks PRIVATE dbengine_ts)
target_include_directories(test_chunks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_chunks PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME chunks COMMAND test_chunks)

# s16-quant: SQ8 echt + PQ-Stub(2) + DiskSpill(mmap) + ef-Autotune
add_library(dbengine_quant STATIC src/vector/quant.cpp)
target_include_directories(dbengine_quant PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dbengine_quant PUBLIC dbengine_vector)
target_compile_options(dbengine_quant PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_quant tests/test_quant.cpp)
target_link_libraries(test_quant PRIVATE dbengine_quant dbengine_vector)
target_include_directories(test_quant PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_quant PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME quant COMMAND test_quant)

# s20-docs: Doku-Guard (Existenz + Nicht-Leer + Markdown-Titel, STL-only).
add_executable(test_docs tests/test_docs.cpp)
target_compile_definitions(test_docs PRIVATE DBENGINE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(test_docs PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME docs COMMAND test_docs)

# metrics: Prometheus-Textformat ueber TCP (127.0.0.1 ephemeral) + dbmetrics --selfcheck/--port
add_executable(dbmetrics src/server/metrics.cpp)
target_include_directories(dbmetrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dbmetrics PRIVATE dbengine_wal dbengine_kv dbengine_raft columnar dbengine_vector)
target_compile_options(dbmetrics PRIVATE -Wall -Wextra -Wpedantic)
