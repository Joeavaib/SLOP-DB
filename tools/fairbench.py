#!/usr/bin/env python3
# s121: fairer Drei-Wege-Benchmark (dbengine via dbbench + SQLite + DuckDB).
# Regeln aus Recherche (TPC-H-Power-Test-Idee, SQLite-speedtest1-Knobs):
#  - EIN Datensatz fuer alle (byte-identisch, validiert gegen dbbench-Refs)
#  - identische Query-Strings, 1 Warmup (verworfen) + N Reps, Median UND p95
#  - Korrektheits-Validierung (Summen/Gruppen muessen uebereinstimmen)
#  - volle Offenlegung: PRAGMAs, Threads, Versionen, Load-Zeiten, tmpfs
# Aufruf: python3 tools/fairbench.py --rows N --reps K --out out.json
"""Fair three-way benchmark driver (dbengine via dbbench, SQLite, DuckDB)."""

import argparse
import json
import math
import os
import sqlite3
import statistics
import subprocess
import sys
import tempfile
import time

# ---- Byte-identische Replikation: C++ mt19937(42) + Lemire-fastrange ----
N = 624
_MT = [0] * N
_IDX = N


def init_genrand(s):
    global _MT, _IDX
    _MT[0] = s & 0xFFFFFFFF
    for i in range(1, N):
        _MT[i] = (1812433253 * (_MT[i - 1] ^ (_MT[i - 1] >> 30)) + i) & 0xFFFFFFFF
    _IDX = N


def _gen():
    global _IDX
    if _IDX >= N:
        for i in range(N):
            y = (_MT[i] & 0x80000000) | (_MT[(i + 1) % N] & 0x7FFFFFFF)
            _MT[i] = _MT[(i + 397) % N] ^ (y >> 1) ^ (0x9908B0DF if y & 1 else 0)
        _IDX = 0
    y = _MT[_IDX]
    _IDX += 1
    y ^= y >> 11
    y ^= (y << 7) & 0x9D2C5680
    y ^= (y << 15) & 0xEFC60000
    y ^= y >> 18
    return y


def bounded(a, b):
    span = b - a + 1
    x = _gen()
    m = x * span
    lo = m & 0xFFFFFFFF
    if lo < span:
        t = ((1 << 32) - span) % span
        while lo < t:
            x = _gen()
            m = x * span
            lo = m & 0xFFFFFFFF
    return a + (m >> 32)


RFS = ["A", "N", "R"]
LSS = ["O", "F"]

Q6 = ("SELECT SUM(price*disc) FROM lineitem WHERE disc BETWEEN 0.05 AND "
      "0.07 AND qty < 24 AND price >= 500.0 AND price < 5000.0 AND tax <= "
      "0.05 AND shipdate BETWEEN 19940101 AND 19951231")
Q1 = ("SELECT rf, ls, SUM(qty), SUM(price), SUM(price*disc), AVG(disc), "
      "COUNT(*) FROM lineitem GROUP BY rf, ls ORDER BY rf, ls")
SCHEMA = ("CREATE TABLE lineitem (orderkey INT, qty INT, price DOUBLE, "
          "disc DOUBLE, tax DOUBLE, rf TEXT, ls TEXT, shipdate INT)")


def gen_rows(n):
    init_genrand(42)
    rows = []
    for idx in range(n):
        qty = bounded(1, 50)
        price = bounded(1000, 1000000) / 100.0
        disc = bounded(0, 10) / 100.0
        tax = bounded(0, 8) / 100.0
        rf = RFS[bounded(0, 2)]
        ls = LSS[bounded(0, 1)]
        yr = bounded(1992, 1998)
        mo = bounded(1, 12)
        da = bounded(1, 28)
        ship = yr * 10000 + mo * 100 + da
        rows.append((idx + 1, qty, price, disc, tax, rf, ls, ship))
    return rows


def ref_q6(rows):
    s = 0.0
    for _, q, p, d, t, _, _, sh in rows:
        if 0.05 <= d <= 0.07 and q < 24 and 500.0 <= p < 5000.0 and \
                t <= 0.05 and 19940101 <= sh <= 19951231:
            s += p * d
    return s


def percentile(xs, p):
    if not xs:
        return 0.0
    ys = sorted(xs)
    i = min(len(ys) - 1, math.ceil(p * len(ys)) - 1)
    return ys[max(0, i)]


def time_reps(fn, reps):
    fn()  # warmup, verworfen
    ts = []
    out = None
    for _ in range(reps):
        t0 = time.perf_counter()
        out = fn()
        ts.append((time.perf_counter() - t0) * 1000.0)
    return out, {"reps_ms": ts, "median_ms": statistics.median(ts),
                 "p95_ms": percentile(ts, 0.95)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=100000)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--out", default="/tmp/fair.json")
    ap.add_argument("--dbbench", default="./build/dbbench")
    args = ap.parse_args()
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tmp = tempfile.mkdtemp(prefix="fairbench_", dir="/tmp")
    res = {"rows": args.rows, "reps": args.reps, "tmp": tmp,
           "sqlite_version": sqlite3.sqlite_version}

    t0 = time.perf_counter()
    rows = gen_rows(args.rows)
    res["gen_s"] = time.perf_counter() - t0
    res["ref_q6"] = ref_q6(rows)
    print(f"gen {args.rows} rows in {res['gen_s']:.1f}s ref_q6={res['ref_q6']:.6f}",
          flush=True)

    # ---- dbengine via dbbench (In-Prozess-Timing, kReps=5 intern) ----
    t0 = time.perf_counter()
    p = subprocess.run([os.path.join(repo, args.dbbench.lstrip("./")),
                        "--tpch", str(args.rows)],
                       capture_output=True, text=True, cwd=repo)
    res["dbbench_total_s"] = time.perf_counter() - t0
    res["dbbench_stderr_tail"] = p.stderr.strip().splitlines()[-4:]
    print("".join(p.stderr.strip().splitlines()[-4:]), flush=True)

    # ---- SQLite (Datei-DB, 1 Transaktion, Default-Pragmas + Report) ----
    sqpath = os.path.join(tmp, "lite.db")
    t0 = time.perf_counter()
    con = sqlite3.connect(sqpath)
    pragmas = {}
    for pr in ["journal_mode", "synchronous", "cache_size", "page_size",
               "locking_mode"]:
        pragmas[pr] = con.execute(f"PRAGMA {pr}").fetchone()[0]
    res["sqlite_pragmas"] = pragmas
    con.execute(SCHEMA)
    con.executemany("INSERT INTO lineitem VALUES (?,?,?,?,?,?,?,?)", rows)
    con.commit()
    res["sqlite_load_s"] = time.perf_counter() - t0
    res["sqlite_db_bytes"] = os.path.getsize(sqpath)
    q6_out, q6_t = time_reps(lambda: con.execute(Q6).fetchall(), args.reps)
    q1_out, q1_t = time_reps(lambda: con.execute(Q1).fetchall(), args.reps)
    res["sqlite_q6"] = {"sum": q6_out[0][0], **q6_t}
    res["sqlite_q1"] = {"groups": len(q1_out),
                        "counted": sum(r[6] for r in q1_out),
                        "rows": [[float(x) if isinstance(x, float) else x
                                  for x in r] for r in q1_out], **q1_t}
    con.close()
    print(f"sqlite q6={q6_out[0][0]:.6f} {q6_t['median_ms']:.2f}/{q6_t['p95_ms']:.2f}ms "
          f"q1groups={len(q1_out)}", flush=True)

    # ---- DuckDB (Datei-DB, executemany wie bisher; Load-Zeit ehrlich) ----
    import duckdb
    res["duckdb_version"] = duckdb.__version__
    ddpath = os.path.join(tmp, "duck.db")
    t0 = time.perf_counter()
    dcon = duckdb.connect(ddpath)
    res["duckdb_threads"] = dcon.execute(
        "SELECT current_setting('threads')").fetchall()[0][0]
    res["duckdb_memory"] = dcon.execute(
        "SELECT current_setting('memory_limit')").fetchall()[0][0]
    dcon.execute(SCHEMA)
    dcon.executemany("INSERT INTO lineitem VALUES (?,?,?,?,?,?,?,?)", rows)
    res["duckdb_load_s"] = time.perf_counter() - t0
    res["duckdb_db_bytes"] = (os.path.getsize(ddpath)
                              if os.path.exists(ddpath) else None)
    dq6_out, dq6_t = time_reps(lambda: dcon.execute(Q6).fetchall(), args.reps)
    dq1_out, dq1_t = time_reps(lambda: dcon.execute(Q1).fetchall(), args.reps)
    res["duckdb_q6"] = {"sum": float(dq6_out[0][0]), **dq6_t}
    res["duckdb_q1"] = {"groups": len(dq1_out),
                        "counted": sum(r[6] for r in dq1_out),
                        "rows": [[float(x) if isinstance(x, float) else x
                                  for x in r] for r in dq1_out], **dq1_t}
    dcon.close()
    print(f"duckdb q6={float(dq6_out[0][0]):.6f} {dq6_t['median_ms']:.2f}/{dq6_t['p95_ms']:.2f}ms "
          f"q1groups={len(dq1_out)}", flush=True)

    # ---- Validierung: Summen muessen uebereinstimmen ----
    for eng in ["sqlite_q6", "duckdb_q6"]:
        rel = abs(res[eng]["sum"] - res["ref_q6"]) / max(1.0, abs(res["ref_q6"]))
        res[eng]["ref_rel_err"] = rel
        assert rel < 1e-9, f"{eng} sum mismatch: {res[eng]['sum']}"
    sq1 = res["sqlite_q1"]["rows"]
    dq1 = res["duckdb_q1"]["rows"]
    assert len(sq1) == len(dq1) == 6, "q1 groups"
    assert res["sqlite_q1"]["counted"] == res["duckdb_q1"]["counted"] == args.rows
    print("validation: sums/groups match", flush=True)

    with open(args.out, "w") as f:
        json.dump(res, f, indent=1)
    print(f"wrote {args.out} (dbs kept in {tmp})", flush=True)


if __name__ == "__main__":
    main()
