#!/usr/bin/env python3
# s123: TPC-H-Support-Matrix — CSVs laden + alle 22 Queries fahren.
# Laedt alle 8 Tabellen (Batches, Quote-Escapes) und faehrt jede Query
# einzeln per CLI (--exec), mit Timeout. Ergebnis: OK/FAIL + ms + Hash.
"""Load TPC-H CSVs into dbengine and run all 22 queries (support matrix)."""
import argparse
import csv
import hashlib
import json
import os
import re
import subprocess
import time

TABLES = ["customer", "lineitem", "nation", "orders", "part", "partsupp",
          "region", "supplier"]
BATCH = 200
QTIMEOUT = 120


def sql_lit(v):
    if v is None or v == "":
        return "NULL"
    if re.fullmatch(r"-?\d+", v):
        return v
    if re.fullmatch(r"-?\d+\.\d+", v):
        return v
    return "'" + v.replace("'", "''") + "'"


def coltype(name, sample):
    if re.fullmatch(r"-?\d+", sample or "0"):
        return "INT"
    if re.fullmatch(r"-?\d+\.\d+", sample or "0"):
        return "DOUBLE"
    return "TEXT"


def run(db, sql, timeout=QTIMEOUT):
    t0 = time.perf_counter()
    try:
        p = subprocess.run([db[0], db[1], "--exec", sql], capture_output=True,
                           text=True, timeout=timeout)
        ms = (time.perf_counter() - t0) * 1000.0
        if p.returncode != 0:
            err = (p.stderr.strip().splitlines() or ["exit 1"])[:3]
            return False, ms, "rc=%d %s" % (p.returncode, " | ".join(err))
        h = hashlib.sha256(p.stdout.encode()).hexdigest()[:16]
        return True, ms, "hash=" + h
    except subprocess.TimeoutExpired:
        return False, timeout * 1000.0, "TIMEOUT"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="/tmp/tpchdata")
    ap.add_argument("--db", default="/tmp/tpchm.db")
    ap.add_argument("--queries", default="tools/tpch/queries")
    ap.add_argument("--out", default="/tmp/matrix.json")
    ap.add_argument("--dbengine", default="./build/dbengine")
    ap.add_argument("--skip-load", action="store_true")
    args = ap.parse_args()
    repo = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    exe = os.path.join(repo, args.dbengine.lstrip("./"))
    db = [exe, args.db]
    res = {"tables": {}, "queries": {}}
    if not args.skip_load:
        for t in TABLES:
            path = os.path.join(args.data, t + ".csv")
            with open(path, newline="") as f:
                rd = csv.reader(f)
                header = next(rd)
                first = None
                rows = []
                for r in rd:
                    if first is None:
                        first = r
                    rows.append(r)
            types = [coltype(h, first[i]) for i, h in enumerate(header)]
            t0 = time.perf_counter()
            ok, _, msg = run(db, "CREATE TABLE %s (%s)" % (
                t, ", ".join(f"{h} {ty}" for h, ty in zip(header, types))),
                timeout=60)
            if not ok:
                res["tables"][t] = {"ok": False, "msg": msg}
                continue
            n = 0
            fail = None
            for i in range(0, len(rows), BATCH):
                chunk = rows[i:i + BATCH]
                vals = ",".join(
                    "(" + ",".join(sql_lit(v) for v in r) + ")" for r in chunk)
                ok, _, msg = run(db, f"INSERT INTO {t} VALUES {vals}",
                                 timeout=300)
                if not ok:
                    fail = msg
                    break
                n += len(chunk)
            ms = (time.perf_counter() - t0) * 1000.0
            res["tables"][t] = {"ok": fail is None, "rows": n,
                                "ms": round(ms, 1),
                                **({"msg": fail} if fail else {})}
            print(f"load {t}: rows={n} ms={ms:.0f} {fail or 'ok'}", flush=True)
    for q in range(1, 23):
        name = f"q{q:02d}"
        with open(os.path.join(args.queries, name + ".sql")) as f:
            sql = f.read()
        # qgen-Suffix entfernen (unser Parser kennt kein LIMIT -1).
        sql = re.sub(r"(?im)^\s*limit\s+-1\s*;?\s*$", "", sql).strip()
        ok, ms, msg = run(db, sql)
        res["queries"][name] = {"ok": ok, "ms": round(ms, 1), "msg": msg}
        print(f"{name}: {'OK' if ok else 'FAIL'} ms={ms:.0f} {msg[:90]}",
              flush=True)
    with open(args.out, "w") as f:
        json.dump(res, f, indent=1)
    print(f"wrote {args.out}", flush=True)


if __name__ == "__main__":
    main()
