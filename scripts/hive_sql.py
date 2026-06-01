#!/usr/bin/env python3
"""Run a sequence of Hive SQL statements against the MR3 HiveServer2 at :9852.

Usage:
    pixi run python scripts/hive_sql.py <sql_file>
    pixi run python scripts/hive_sql.py -e "SHOW DATABASES"

Statements are split on `;` at EOL and executed in order. Result rows are
printed for SELECT/SHOW/DESCRIBE/EXPLAIN; DDL just prints a status line.
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from pyhive import hive


HS2_HOST = "192.168.1.202"
HS2_PORT = 9852
HS2_USER = "erpage159"


def split_statements(sql_text: str) -> list[str]:
    cleaned = re.sub(r"--[^\n]*", "", sql_text)
    parts = [p.strip() for p in cleaned.split(";")]
    return [p for p in parts if p]


def is_resultful(stmt: str) -> bool:
    head = stmt.lstrip().split(None, 1)[0].upper() if stmt.strip() else ""
    return head in {"SELECT", "SHOW", "DESCRIBE", "DESC", "EXPLAIN", "WITH"}


def main() -> int:
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("sql_file", nargs="?", help="Path to .sql file")
    g.add_argument("-e", "--execute", help="Inline SQL (statements separated by ;)")
    ap.add_argument("--host", default=HS2_HOST)
    ap.add_argument("--port", type=int, default=HS2_PORT)
    ap.add_argument("--user", default=HS2_USER)
    args = ap.parse_args()

    if args.execute:
        sql_text = args.execute
    else:
        with open(args.sql_file) as f:
            sql_text = f.read()

    stmts = split_statements(sql_text)
    if not stmts:
        print("no statements", file=sys.stderr)
        return 1

    conn = hive.Connection(host=args.host, port=args.port, username=args.user)
    cur = conn.cursor()
    for i, stmt in enumerate(stmts, 1):
        head = stmt.lstrip().split("\n", 1)[0]
        print(f"\n=== [{i}/{len(stmts)}] {head[:120]} ===", flush=True)
        t0 = time.time()
        try:
            cur.execute(stmt)
        except Exception as e:
            print(f"ERROR after {time.time()-t0:.1f}s: {e}", flush=True)
            return 2
        elapsed = time.time() - t0
        if is_resultful(stmt):
            try:
                rows = cur.fetchall()
                cols = [d[0] for d in (cur.description or [])]
                print("  " + " | ".join(cols))
                for r in rows[:50]:
                    print("  " + " | ".join(str(v) for v in r))
                if len(rows) > 50:
                    print(f"  ... ({len(rows)-50} more rows)")
            except Exception as e:
                print(f"  (no resultset: {e})")
        print(f"  done in {elapsed:.1f}s", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
