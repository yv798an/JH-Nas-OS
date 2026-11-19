#!/usr/bin/env python3
"""Summarize board-side decomposition data (timing CSV + procfs snapshots).

Inputs (produced by the instrumented build + run-board-tests.sh):
    NAS_Test/data/ab-handoff-{yield,condvar}-{sda1,sdb1}/io-{cold,hot}.csv
        - new columns: submit_ms,handoff_ms,queue_ms,reader_cpu_s,consumer_cpu_s
    .../proc-{sync,uring}-t{1,2,4,8}.{before,after}.csv

Outputs:
    NAS_Test/decomp-time.csv
    NAS_Test/decomp-threads.csv

Usage:
    python scripts/analyze-decomp.py
    python scripts/analyze-decomp.py --mode condvar
"""

import argparse
import csv
from pathlib import Path
from statistics import median

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "NAS_Test" / "data"


def med(rows, key):
    vals = [float(r[key]) for r in rows if r.get(key) not in (None, "")]
    return median(vals) if vals else None


def fmt(v, nd=2):
    return "—" if v is None else f"{v:.{nd}f}"


def load_io(path, cache, scheme):
    if not path.exists():
        return None
    with path.open(newline="", encoding="utf-8") as fh:
        rows = [r for r in csv.DictReader(fh)
                if int(r["block_kib"]) == 64 and r["scheme"] == scheme
                and r["cache"] == cache]
    if not rows:
        return None
    wall = med(rows, "wall_s")
    submit = med(rows, "submit_ms")
    handoff = med(rows, "handoff_ms")
    queue = med(rows, "queue_ms")
    reader_cpu = med(rows, "reader_cpu_s")
    consumer_cpu = med(rows, "consumer_cpu_s")
    cpu = med(rows, "cpu_s")
    if wall is None:
        return None
    wall_ms = wall * 1000.0
    return {
        "wall_s": wall,
        "cpu_s": cpu,
        "submit_ms": submit,
        "handoff_ms": handoff,
        "queue_ms": queue,
        "reader_cpu_s": reader_cpu,
        "consumer_cpu_s": consumer_cpu,
        "submit_pct": (submit or 0.0) / wall_ms * 100.0,
        "handoff_pct": (handoff or 0.0) / wall_ms * 100.0,
        "queue_pct": (queue or 0.0) / wall_ms * 100.0,
        "reader_cpu_pct": (reader_cpu or 0.0) / wall * 100.0,
        "consumer_cpu_pct": (consumer_cpu or 0.0) / wall * 100.0,
    }


def parse_proc(path):
    """Return (meta dict, {tid: row}) from a proc-thread-stats.sh snapshot."""
    if not path.exists():
        return None, {}
    meta, rows = {}, {}
    with path.open(newline="", encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if line.startswith("#"):
                for part in line[1:].strip().split():
                    if "=" in part:
                        k, v = part.split("=", 1)
                        meta[k] = v
                continue
            if not line or line.startswith("tid,"):
                continue
            parts = line.split(",")
            if len(parts) < 6:
                continue
            tid, comm = parts[0], parts[1]
            try:
                rows[tid] = {
                    "comm": comm,
                    "utime": int(parts[2]),
                    "stime": int(parts[3]),
                    "vcsw": int(parts[4]),
                    "nivcsw": int(parts[5]),
                }
            except ValueError:
                continue
    return meta, rows


def proc_delta(before, after):
    mb, rb = parse_proc(before)
    ma, ra = parse_proc(after)
    if not rb or not ra:
        return None
    hz = float((ma or mb or {}).get("hz", 100))
    out = []
    for tid, b in rb.items():
        a = ra.get(tid)
        if not a:
            continue
        du = a["utime"] - b["utime"]
        ds = a["stime"] - b["stime"]
        dv = a["vcsw"] - b["vcsw"]
        dn = a["nivcsw"] - b["nivcsw"]
        out.append({
            "tid": tid,
            "comm": a["comm"],
            "cpu_s": (du + ds) / hz,
            "vcsw": dv,
            "nivcsw": dn,
        })
    total = sum(r["cpu_s"] for r in out)
    for r in out:
        r["cpu_share"] = (r["cpu_s"] / total * 100.0) if total > 0 else 0.0
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="condvar",
                    choices=["yield", "condvar"],
                    help="which handoff run to summarize")
    ap.add_argument("--scheme", default="uring_qd8",
                    help="scheme to use for the time breakdown (default uring_qd8)")
    args = ap.parse_args()

    time_rows = []
    print(f"== timing decomposition ({args.mode}, {args.scheme}) ==")
    print("| scenario | wall_s | submit_ms | handoff_ms | queue_ms | "
          "reader_cpu_s | consumer_cpu_s | submit% | handoff% | queue% |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for disk in ("sda1", "sdb1"):
        base = DATA / f"ab-handoff-{args.mode}-{disk}"
        for cache in ("cold", "hot"):
            d = load_io(base / f"io-{cache}.csv", cache, args.scheme)
            scenario = f"{cache}_{disk}"
            if not d:
                print(f"| {scenario} | — | — | — | — | — | — | — | — | — |")
                continue
            print(f"| {scenario} | {fmt(d['wall_s'])} | {fmt(d['submit_ms'],1)} | "
                  f"{fmt(d['handoff_ms'],1)} | {fmt(d['queue_ms'],1)} | "
                  f"{fmt(d['reader_cpu_s'])} | {fmt(d['consumer_cpu_s'])} | "
                  f"{fmt(d['submit_pct'],1)} | {fmt(d['handoff_pct'],1)} | "
                  f"{fmt(d['queue_pct'],1)} |")
            time_rows.append({
                "scenario": scenario,
                "wall_s": d["wall_s"],
                "submit_ms": d["submit_ms"],
                "handoff_ms": d["handoff_ms"],
                "queue_ms": d["queue_ms"],
                "reader_cpu_s": d["reader_cpu_s"],
                "consumer_cpu_s": d["consumer_cpu_s"],
                "submit_pct": d["submit_pct"],
                "handoff_pct": d["handoff_pct"],
                "queue_pct": d["queue_pct"],
                "reader_cpu_pct": d["reader_cpu_pct"],
                "consumer_cpu_pct": d["consumer_cpu_pct"],
            })

    thread_rows = []
    print("\n== procfs thread CPU / context switches (HTTP) ==")
    print("| scenario | comm | cpu_s | cpu_share% | vcsw | nivcsw |")
    print("|---|---|---:|---:|---:|---:|")
    for disk in ("sda1", "sdb1"):
        base = DATA / f"ab-handoff-{args.mode}-{disk}"
        for server in ("sync", "uring"):
            for t in (1, 4, 8):
                before = base / f"proc-{server}-t{t}.before.csv"
                after = base / f"proc-{server}-t{t}.after.csv"
                rows = proc_delta(before, after)
                if not rows:
                    continue
                scenario = f"http_{server}_t{t}_{disk}"
                for r in sorted(rows, key=lambda x: -x["cpu_s"]):
                    if r["comm"] not in ("nas-http", "nas-reader",
                                         "nas-server", "io-consumer"):
                        continue
                    print(f"| {scenario} | {r['comm']} | {fmt(r['cpu_s'],3)} | "
                          f"{fmt(r['cpu_share'],1)} | {r['vcsw']} | {r['nivcsw']} |")
                    thread_rows.append({
                        "scenario": scenario,
                        "comm": r["comm"],
                        "cpu_s": r["cpu_s"],
                        "cpu_share": r["cpu_share"],
                        "vcsw": r["vcsw"],
                        "nivcsw": r["nivcsw"],
                    })

    out_time = ROOT / "NAS_Test" / "decomp-time.csv"
    out_threads = ROOT / "NAS_Test" / "decomp-threads.csv"
    if time_rows:
        with out_time.open("w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=list(time_rows[0].keys()))
            w.writeheader()
            w.writerows(time_rows)
        print(f"\nwrote {out_time.relative_to(ROOT)}")
    if thread_rows:
        with out_threads.open("w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=list(thread_rows[0].keys()))
            w.writeheader()
            w.writerows(thread_rows)
        print(f"wrote {out_threads.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
