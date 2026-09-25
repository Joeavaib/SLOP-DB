# dbengine

Minimal C++20 scaffold for a small embedded database engine.

## Layout

- `include/dbengine/` – public headers (`version.h`)
- `src/` – executables / library sources (`main.cpp`)
- `tests/` – CTest smoke test
- `CMakeLists.txt` – C++20, `-Wall -Wextra`, CTest

## Prerequisites

- CMake >= 3.20
- C++20 compiler (GCC 11+, Clang 14+)

## Build

```sh
cmake -S . -B build
cmake --build build -j4
```

## Test

```sh
ctest --test-dir build --output-on-failure
```

## Run

```sh
./build/dbengine
# dbengine version 0.1.0
```

## License

Apache-2.0, see `LICENSE`.
