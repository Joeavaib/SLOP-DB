#!/usr/bin/env python3
# s122: TPC-H-Datengenerator — DuckDB dbgen (kanonische Verteilung) -> CSVs.
# DATE-Spalten werden als INT yyyymmdd exportiert (dbengine-Dialekt hat kein
# DATE; dokumentierte Adaptation, s. docs/TPCH-FULL.md). Zeilen deterministisch
# per PK sortiert. Manifest: Counts + SHA256.
"""Generate canonical TPC-H CSVs via DuckDB dbgen (INT dates)."""
import argparse
import hashlib
import json
import os

DATE_COLS = {
    "lineitem": ["l_shipdate", "l_commitdate", "l_receiptdate"],
    "orders": ["o_orderdate"],
}
TABLES = ["customer", "lineitem", "nation", "orders", "part", "partsupp",
          "region", "supplier"]
PK = {"customer": "c_custkey", "lineitem": "l_orderkey, l_linenumber",
      "nation": "n_nationkey", "orders": "o_orderkey", "part": "p_partkey",
      "partsupp": "ps_partkey, ps_suppkey", "region": "r_regionkey",
      "supplier": "s_suppkey"}


def ymd(col):
    return (f"(EXTRACT(year FROM {col})*10000+EXTRACT(month FROM {col})*100"
            f"+EXTRACT(day FROM {col}))::INTEGER AS {col}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sf", type=float, default=0.01)
    ap.add_argument("--out", default="/tmp/tpchdata")
    ap.add_argument("--manifest", action="store_true")
    args = ap.parse_args()
    import duckdb
    os.makedirs(args.out, exist_ok=True)
    con = duckdb.connect()
    con.execute(f"CALL dbgen(sf={args.sf})")
    manifest = {"sf": args.sf, "duckdb": duckdb.__version__, "tables": {}}
    for t in TABLES:
        cols = [c[0] for c in con.execute(f"DESCRIBE {t}").fetchall()]
        sel = [ymd(c) if c in DATE_COLS.get(t, []) else c for c in cols]
        path = os.path.join(args.out, f"{t}.csv")
        con.execute(
            f"COPY (SELECT {', '.join(sel)} FROM {t} ORDER BY {PK[t]}) "
            f"TO '{path}' (HEADER, DELIMITER ',')")
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        n = con.execute(f"SELECT count(*) FROM {t}").fetchall()[0][0]
        manifest["tables"][t] = {"rows": n, "sha256": h.hexdigest(),
                                 "bytes": os.path.getsize(path)}
        print(f"{t}: rows={n} sha256={h.hexdigest()[:16]}", flush=True)
    if args.manifest:
        mp = os.path.join(args.out, "MANIFEST.json")
        with open(mp, "w") as f:
            json.dump(manifest, f, indent=1)
        print(f"wrote {mp}", flush=True)


if __name__ == "__main__":
    main()
