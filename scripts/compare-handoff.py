#!/usr/bin/env python3
"""Compare yield vs condvar A/B runs produced by scripts/ab-handoff.sh.

Reads NAS_Test/data/ab-handoff-{yield,condvar}-{sda1,sdb1}/ and writes:
    NAS_Test/ab-handoff-summary.md
    NAS_Test/ab-handoff-summary.csv
"""

import csv
from pathlib import Path
from statistics import median

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "NAS_Test" / "data"
OUT_MD = ROOT / "NAS_Test" / "ab-handoff-summary.md"
OUT_CSV = ROOT / "NAS_Test" / "ab-handoff-summary.csv"

MODES = ("yield", "condvar")
DISKS = ("sda1", "sdb1")
SCHEMES = ("sync", "uring_qd1", "uring_qd4", "uring_qd8",
           "uring_qd16", "uring_qd32", "uring_qd64")
THREADS = (1, 2, 4, 8)


def med(values):
    values = list(values)
    return median(values) if values else None


def fmt(v, nd=1):
    return "—" if v is None else f"{v:.{nd}f}"


def delta(new, old):
    if new is None or old in (None, 0):
        return None
    return (new - old) / old * 100.0


def pct(v):
    return "—" if v is None else f"{v:+.1f}%"


def load_io(mode, disk):
    path = DATA / f"ab-handoff-{mode}-{disk}" / "io-cold.csv"
    if not path.exists():
        return None
    acc = {}
    with path.open(newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            if int(row["block_kib"]) != 64:
                continue
            key = row["scheme"]
            acc.setdefault(key, {"tput": [], "cpu": []})
            acc[key]["tput"].append(float(row["throughput_mib_s"]))
            acc[key]["cpu"].append(float(row["cpu_s_per_gib"]))
    return {k: (med(v["tput"]), med(v["cpu"])) for k, v in acc.items()}


def load_http(mode, disk):
    base = DATA / f"ab-handoff-{mode}-{disk}"
    if not base.exists():
        return None, None
    tput = {}
    for t in THREADS:
        for server in ("sync", "uring"):
            path = base / f"http-{server}-t{t}.csv"
            if not path.exists():
                continue
            with path.open(newline="", encoding="utf-8") as fh:
                rows = list(csv.DictReader(fh))
            if rows:
                tput[(server, t)] = float(rows[0]["throughput_mib_s"])
    cpu = {}
    path = base / "http-server-cpu.txt"
    if path.exists():
        with path.open(newline="", encoding="utf-8") as fh:
            for row in csv.DictReader(fh):
                cpu[(row["mode"], int(row["threads"]))] = float(
                    row["server_cpu_pct"])
    return tput, cpu


def main():
    io = {(m, d): load_io(m, d) for m in MODES for d in DISKS}
    http_t = {(m, d): load_http(m, d)[0] for m in MODES for d in DISKS}
    http_c = {(m, d): load_http(m, d)[1] for m in MODES for d in DISKS}

    md = ["# Handoff A/B: yield vs condvar", ""]
    md.append("数据来源：`NAS_Test/data/ab-handoff-{yield,condvar}-{sda1,sdb1}/`。")
    md.append("")
    rows = []

    md.append("## 冷读（64 KiB，QD 1–64）")
    md.append("")
    md.append("| 盘 | 方案 | yield MiB/s | condvar MiB/s | Δ吞吐 | "
              "yield cpu-s/GiB | condvar cpu-s/GiB | ΔCPU |")
    md.append("|---|---|---:|---:|---:|---:|---:|---:|")
    for disk in DISKS:
        y = io[("yield", disk)]
        c = io[("condvar", disk)]
        if y is None or c is None:
            md.append(f"| {disk} | — | 数据缺失 | 数据缺失 | — | — | — | — |")
            continue
        for scheme in SCHEMES:
            yt, yc = y.get(scheme, (None, None))
            ct, cc = c.get(scheme, (None, None))
            md.append(
                f"| {disk} | {scheme} | {fmt(yt)} | {fmt(ct)} | "
                f"{pct(delta(ct, yt))} | {fmt(yc, 2)} | {fmt(cc, 2)} | "
                f"{pct(delta(cc, yc))} |")
            rows.append(["io_cold", disk, scheme, "throughput_mib_s",
                         fmt(yt), fmt(ct), pct(delta(ct, yt))])
            rows.append(["io_cold", disk, scheme, "cpu_s_per_gib",
                         fmt(yc, 2), fmt(cc, 2), pct(delta(cc, yc))])
    md.append("")

    md.append("## HTTP 吞吐（MiB/s）")
    md.append("")
    md.append("| 盘 | 线程 | yield sync | condvar sync | yield uring | "
              "condvar uring | Δuring |")
    md.append("|---|---:|---:|---:|---:|---:|---:|")
    for disk in DISKS:
        y = http_t[("yield", disk)]
        c = http_t[("condvar", disk)]
        if not y or not c:
            md.append(f"| {disk} | — | 数据缺失 | 数据缺失 | 数据缺失 | 数据缺失 | — |")
            continue
        for t in THREADS:
            ys = y.get(("sync", t))
            cs = c.get(("sync", t))
            yu = y.get(("uring", t))
            cu = c.get(("uring", t))
            md.append(
                f"| {disk} | {t} | {fmt(ys)} | {fmt(cs)} | {fmt(yu)} | "
                f"{fmt(cu)} | {pct(delta(cu, yu))} |")
            rows.append(["http", disk, f"t{t}", "sync_mib_s",
                         fmt(ys), fmt(cs), pct(delta(cs, ys))])
            rows.append(["http", disk, f"t{t}", "uring_mib_s",
                         fmt(yu), fmt(cu), pct(delta(cu, yu))])
    md.append("")

    md.append("## HTTP 服务端 CPU（%）")
    md.append("")
    md.append("| 盘 | 线程 | yield sync | condvar sync | yield uring | "
              "condvar uring | Δuring CPU |")
    md.append("|---|---:|---:|---:|---:|---:|---:|")
    for disk in DISKS:
        y = http_c[("yield", disk)]
        c = http_c[("condvar", disk)]
        if not y or not c:
            md.append(f"| {disk} | — | 数据缺失 | 数据缺失 | 数据缺失 | 数据缺失 | — |")
            continue
        for t in THREADS:
            ys = y.get(("sync", t))
            cs = c.get(("sync", t))
            yu = y.get(("uring", t))
            cu = c.get(("uring", t))
            md.append(
                f"| {disk} | {t} | {fmt(ys)} | {fmt(cs)} | {fmt(yu)} | "
                f"{fmt(cu)} | {pct(delta(cu, yu))} |")
            rows.append(["http_cpu", disk, f"t{t}", "sync_pct",
                         fmt(ys), fmt(cs), pct(delta(cs, ys))])
            rows.append(["http_cpu", disk, f"t{t}", "uring_pct",
                         fmt(yu), fmt(cu), pct(delta(cu, yu))])

    md.append("")
    md.append("> 判读：condvar 的目标是降低慢盘冷读与 HTTP 的 CPU 成本；"
              "sync 行是同一代码路径的对照，Δ 应接近 0。")
    OUT_MD.write_text("\n".join(md) + "\n", encoding="utf-8")

    with OUT_CSV.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["section", "disk", "item", "metric",
                    "yield", "condvar", "delta_pct"])
        w.writerows(rows)

    print(f"wrote {OUT_MD.relative_to(ROOT)}")
    print(f"wrote {OUT_CSV.relative_to(ROOT)}")
    print()
    print("\n".join(md))


if __name__ == "__main__":
    main()
