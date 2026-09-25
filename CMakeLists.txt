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
