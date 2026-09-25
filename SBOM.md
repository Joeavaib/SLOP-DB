# SBOM — dbengine 0.1.0 (s20-docs)

Stand: 2026-09-25. Lizenz Kern: Apache-2.0 (siehe `LICENSE`).

## Runtime-Dependencies

| Dep | Version | Lizenz | Bemerkung |
|---|---|---|---|
| STL (libstdc++ / libc++) | Toolchain (GCC 11+ / Clang 14+) | GPLv3+Runtime-Exception / Apache-2.0+LLVM | einzige C++-Dep |
| POSIX libc + pthreads + mmap/pread/fsync | glibc ≥ 2.34 | LGPL-2.1+ | WAL/Pager/TCPServer |
| CMake (Build-Tool, kein Runtime) | ≥ 3.20 | BSD-3 | `CMakeLists.txt` |

**Explizit keine externen Runtime-Deps:**

- kein FAISS, kein OpenBLAS/MKL, kein TBB
- kein gRPC/Protobuf, kein Arrow-Parquet-Lib (Arrow-Export ist Stub/eigenes Format)
- kein gtest/GoogleMock (Tests sind assert-basiert, STL-only)
- kein Boost, kein Abseil, kein fmt/spdlog, kein OpenSSL-Link in V1 (TLS 1.3 geplant, N6)

## Module → Deps (alle STL/POSIX-only)

- `storage/pager, storage/wal`: `<vector>/<string>/<mutex>/<filesystem>` + POSIX `open/pread/pwrite/fsync/mmap`
- `txn/mvcc, txn/clock`: `<atomic>/<mutex>/<map>/<optional>`
- `kv/kv`: STL-Container
- `sql/*, server/*`: STL + POSIX-Sockets (`pgserver`)
- `vector/hnsw, search/hybrid`: `<vector>/<cmath>/<unordered_map>` — HNSW-lite + BM25-lite handgeschrieben
- `columnar/store, index/btree`: STL
- `tests/test_docs`: `<fstream>/<filesystem>/<string>/<iostream>` — prüft nur Doku-Existenz

## Reproduktion / Audit

```sh
# keine Fetch-/Download-Schritte nötig:
cmake -S . -B build && cmake --build build -j4
ctest --test-dir build --output-on-failure
# Lizenz-Check: Kern Apache-2.0, keine SSPL/BSL im Baum (vgl. N7, A5)
grep -ri "faiss\|openblas\|grpc\|protobuf" --include="*.txt" --include="*.cpp" --include="*.h" . || echo "clean: kein FAISS/gRPC/Protobuf"
```

SLSA/FOSSA/REUSE-Härtung ist nach V1 geplant (A5); diese Datei ist die
manuelle SBOM-Vorstufe dazu.
