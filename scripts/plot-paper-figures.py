#!/usr/bin/env python3
"""Generate paper figures from NAS_Test benchmark CSVs (stdlib only, SVG out).

Usage (from repo root):
    python scripts/plot-paper-figures.py

Outputs:
    NAS_Test/figures/fig1_architecture.svg
    NAS_Test/figures/fig2_cold_qd.svg
    NAS_Test/figures/fig3_ratio_heatmap.svg
    NAS_Test/figures/fig4_cpu_tput.svg
    NAS_Test/figures/fig5_http.svg
"""

import csv
import math
from pathlib import Path
from statistics import median

ROOT = Path(__file__).resolve().parents[1]
NT = ROOT / "NAS_Test"
DATA = NT / "data"
OUT = NT / "figures"

BLUE = "#1f77b4"
RED = "#d62728"
GREEN = "#2ca02c"
ORANGE = "#e07b00"
GRAY = "#666666"


def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class SVG:
    def __init__(self, w, h, bg="#ffffff"):
        self.w, self.h = w, h
        self.parts = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
            f'viewBox="0 0 {w} {h}">',
            f'<rect width="{w}" height="{h}" fill="{bg}"/>',
            '<defs><marker id="arrow" markerWidth="10" markerHeight="10" '
            'refX="9" refY="3" orient="auto"><path d="M0,0 L9,3 L0,6 z" '
            'fill="#444"/></marker></defs>',
        ]

    def add(self, s):
        self.parts.append(s)

    def line(self, x1, y1, x2, y2, stroke="#333", width=1.4, dash=None,
             marker=False):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        m = ' marker-end="url(#arrow)"' if marker else ""
        self.add(f'<line x1="{x1:.2f}" y1="{y1:.2f}" x2="{x2:.2f}" '
                 f'y2="{y2:.2f}" stroke="{stroke}" stroke-width="{width}"'
                 f'{d}{m}/>')

    def rect(self, x, y, w, h, fill="none", stroke="#555", width=1.2,
             dash=None, rx=4):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<rect x="{x:.2f}" y="{y:.2f}" width="{w:.2f}" '
                 f'height="{h:.2f}" rx="{rx}" fill="{fill}" stroke="{stroke}" '
                 f'stroke-width="{width}"{d}/>')

    def text(self, x, y, s, size=13, anchor="start", fill="#222",
             weight="normal", rotate=None,
             family="Arial, Helvetica, sans-serif"):
        tr = f' transform="rotate({rotate} {x:.2f} {y:.2f})"' if rotate else ""
        self.add(f'<text x="{x:.2f}" y="{y:.2f}" font-family="{family}" '
                 f'font-size="{size}" fill="{fill}" font-weight="{weight}" '
                 f'text-anchor="{anchor}"{tr}>{esc(s)}</text>')

    def circle(self, x, y, r=4.2, fill=BLUE, stroke="none", width=1.5):
        self.add(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{r}" fill="{fill}" '
                 f'stroke="{stroke}" stroke-width="{width}"/>')

    def square(self, x, y, r=4.2, fill=BLUE, stroke="none", width=1.5):
        self.add(f'<rect x="{x-r:.2f}" y="{y-r:.2f}" width="{2*r}" '
                 f'height="{2*r}" fill="{fill}" stroke="{stroke}" '
                 f'stroke-width="{width}"/>')

    def save(self, path):
        self.parts.append("</svg>")
        path.write_text("\n".join(self.parts), encoding="utf-8")
        print(f"  wrote {path.relative_to(ROOT)}")


class Chart:
    def __init__(self, svg, L, R, T, B, xmin, xmax, ymin, ymax,
                 xlog=False, ylog=False):
        self.s, self.L, self.R, self.T, self.B = svg, L, R, T, B
        self.xmin, self.xmax = xmin, xmax
        self.ymin, self.ymax = ymin, ymax
        self.xlog, self.ylog = xlog, ylog

    def X(self, v):
        if self.xlog:
            a, b = math.log10(self.xmin), math.log10(self.xmax)
            return self.L + (math.log10(v) - a) / (b - a) * (self.R - self.L)
        return self.L + (v - self.xmin) / (self.xmax - self.xmin) * (self.R - self.L)

    def Y(self, v):
        if self.ylog:
            a, b = math.log10(self.ymin), math.log10(self.ymax)
            return self.B - (math.log10(v) - a) / (b - a) * (self.B - self.T)
        return self.B - (v - self.ymin) / (self.ymax - self.ymin) * (self.B - self.T)

    def frame(self, xticks, yticks, xlabel, ylabel, title=None,
              xticklabels=None):
        s = self.s
        for t in yticks:
            y = self.Y(t)
            s.line(self.L, y, self.R, y, stroke="#ececec", width=1.0)
        for t in xticks:
            x = self.X(t)
            s.line(x, self.T, x, self.B, stroke="#ececec", width=1.0)
        s.line(self.L, self.B, self.R, self.B, stroke="#333", width=1.5)
        s.line(self.L, self.T, self.L, self.B, stroke="#333", width=1.5)
        for i, t in enumerate(xticks):
            x = self.X(t)
            s.line(x, self.B, x, self.B + 5, stroke="#333", width=1.2)
            lab = xticklabels[i] if xticklabels else f"{t:g}"
            s.text(x, self.B + 20, lab, size=11, anchor="middle")
        for t in yticks:
            y = self.Y(t)
            s.line(self.L - 5, y, self.L, y, stroke="#333", width=1.2)
            s.text(self.L - 9, y + 4, f"{t:g}", size=11, anchor="end")
        s.text((self.L + self.R) / 2, self.B + 44, xlabel, size=12.5,
               anchor="middle")
        s.text(self.L - 58, (self.T + self.B) / 2, ylabel, size=12.5,
               anchor="middle", rotate=-90)
        if title:
            s.text((self.L + self.R) / 2, self.T - 16, title, size=14,
                   anchor="middle", weight="bold")

    def polyline(self, pts, color=BLUE, width=2.2, dash=None, marker="o",
                 msize=4.0, filled=True):
        if not pts:
            return
        d = f' stroke-dasharray="{dash}"' if dash else ""
        p = " ".join(f"{self.X(x):.2f},{self.Y(y):.2f}" for x, y in pts)
        self.s.add(f'<polyline points="{p}" fill="none" stroke="{color}" '
                   f'stroke-width="{width}"{d} stroke-linejoin="round" '
                   f'stroke-linecap="round"/>')
        for x, y in pts:
            self.marker(x, y, color, marker, msize, filled)

    def hline(self, y, color=GRAY, width=1.6, dash="7,5", label=None):
        yy = self.Y(y)
        self.s.line(self.L, yy, self.R, yy, stroke=color, width=width, dash=dash)
        if label:
            self.s.text(self.R - 4, yy - 6, label, size=10.5, anchor="end",
                        fill=color)

    def marker(self, x, y, color, shape="o", size=4.0, filled=True):
        px, py = self.X(x), self.Y(y)
        if shape == "s":
            if filled:
                self.s.square(px, py, size, fill=color)
            else:
                self.s.square(px, py, size, fill="#ffffff", stroke=color,
                              width=1.8)
        else:
            if filled:
                self.s.circle(px, py, size, fill=color)
            else:
                self.s.circle(px, py, size, fill="#ffffff", stroke=color,
                              width=1.8)

    def scatter(self, pts, color=BLUE, shape="o", size=4.2, filled=True):
        for x, y in pts:
            self.marker(x, y, color, shape, size, filled)


def legend(svg, x, y, items, cols=1, dx=170, dy=22):
    for i, (label, color, kind) in enumerate(items):
        r, c = divmod(i, cols)
        xx, yy = x + c * dx, y + r * dy
        if kind == "dash":
            svg.line(xx, yy - 4, xx + 26, yy - 4, stroke=color, width=2.2,
                     dash="7,5")
        elif kind == "square":
            svg.square(xx + 13, yy - 4, 4.2, fill=color)
            svg.line(xx, yy - 4, xx + 26, yy - 4, stroke=color, width=2.0)
        elif kind == "open":
            svg.circle(xx + 13, yy - 4, 4.0, fill="#ffffff", stroke=color,
                       width=1.8)
            svg.line(xx, yy - 4, xx + 26, yy - 4, stroke=color, width=2.0)
        else:
            svg.circle(xx + 13, yy - 4, 4.2, fill=color)
            svg.line(xx, yy - 4, xx + 26, yy - 4, stroke=color, width=2.0)
        svg.text(xx + 33, yy, label, size=11.5)


def load_io(dirname):
    out = {}
    for cache in ("cold", "hot"):
        path = DATA / dirname / f"io-{cache}.csv"
        acc = {}
        with path.open(newline="", encoding="utf-8") as fh:
            for row in csv.DictReader(fh):
                key = (int(row["block_kib"]), row["scheme"])
                acc.setdefault(key, []).append(
                    (float(row["throughput_mib_s"]),
                     float(row["cpu_s_per_gib"])))
        out[cache] = {
            k: (median(v[0] for v in vals), median(v[1] for v in vals))
            for k, vals in acc.items()
        }
    return out


def qd_of(scheme):
    return 0 if scheme == "sync" else int(scheme.split("qd")[1])


def load_http(dirname):
    tput, cpu = {}, {}
    for t in (1, 2, 4, 8):
        for mode in ("sync", "uring"):
            path = DATA / dirname / f"http-{mode}-t{t}.csv"
            with path.open(newline="", encoding="utf-8") as fh:
                rows = list(csv.DictReader(fh))
            tput[(mode, t)] = float(rows[0]["throughput_mib_s"])
    with (DATA / dirname / "http-server-cpu.txt").open(
            newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            cpu[(row["mode"], int(row["threads"]))] = float(
                row["server_cpu_pct"])
    return tput, cpu


# ---------------------------------------------------------------- fig 1
def fig1():
    s = SVG(1200, 520)
    s.text(600, 30, "Configurable download I/O backend", size=17,
           anchor="middle", weight="bold")

    def box(x, y, w, h, lines, fill="#f7f8fa", dash=None, size=13):
        s.rect(x, y, w, h, fill=fill, dash=dash)
        for i, line in enumerate(lines):
            s.text(x + w / 2, y + h / 2 + (i - (len(lines) - 1) / 2) * 17 + 5,
                   line, size=size, anchor="middle")

    box(40, 60, 150, 56, ["HTTP client"])
    box(240, 60, 190, 56, ["cpp-httplib", "adapter"])
    box(480, 60, 140, 56, ["Router"])
    box(670, 60, 230, 56, ["FileApi", "handleDownload"])
    for x1, x2 in ((190, 240), (430, 480), (620, 670)):
        s.line(x1, 88, x2, 88, marker=True)

    s.line(785, 116, 785, 150)
    s.line(300, 150, 900, 150)
    s.line(300, 150, 300, 185, marker=True)
    s.line(900, 150, 900, 185, marker=True)

    s.text(300, 142, "io.uring = false (default)", size=11, anchor="middle",
           fill=GRAY)
    box(150, 190, 300, 110,
        ["Synchronous path", "std::ifstream", "64 KiB chunks + BodySink"])

    s.text(900, 142, "io.uring = true & size >= threshold", size=11,
           anchor="middle", fill=GRAY)
    box(730, 190, 340, 170,
        ["Asynchronous path", "AsyncFileReader (reader thread)",
         "IoUring (liburing)", "BufferPool (depth + 4)", "SPSC ring queue"])

    s.line(730, 275, 450, 275, stroke=RED, width=1.6, dash="6,4", marker=True)
    s.text(590, 266, "fallback: ring/start failure or offset != 0",
           size=10.5, anchor="middle", fill=RED)

    box(440, 420, 320, 58, ["BodySink  ->  HTTP socket"])
    s.line(300, 300, 300, 390)
    s.line(300, 390, 600, 390)
    s.line(600, 390, 600, 420, marker=True)
    s.line(900, 360, 900, 390)
    s.line(900, 390, 600, 390)

    s.save(OUT / "fig1_architecture.svg")


# ---------------------------------------------------------------- fig 2
def fig2(sda1, sdb1):
    s = SVG(1100, 440)
    panels = [
        ("sda1 (USB 3.0, ~170 MiB/s cold)", sda1, 150, 250,
         [150, 175, 200, 225, 250]),
        ("sdb1 (USB 2.0, ~19 MiB/s cold)", sdb1, 17, 21,
         [17, 18, 19, 20, 21]),
    ]
    for i, (title, data, ymin, ymax, yticks) in enumerate(panels):
        L = 85 + i * 555
        R, T, B = L + 450, 75, 345
        ch = Chart(s, L, R, T, B, 1, 64, ymin, ymax, xlog=True)
        ch.frame([1, 4, 8, 16, 32, 64], yticks,
                 "Queue depth (QD)", "Throughput (MiB/s)", title)
        sync = data["cold"][(64, "sync")][0]
        ch.hline(sync, color=GRAY, dash="7,5", label=f"sync {sync:.1f}")
        pts = sorted(
            (qd_of(scheme), tput)
            for (block, scheme), (tput, _cpu) in data["cold"].items()
            if block == 64 and scheme != "sync"
        )
        ch.polyline(pts, color=BLUE, marker="o")
        if i == 1:
            s.text(R - 6, T + 18,
                   f"QD1-QD64: {min(p[1] for p in pts):.1f}-"
                   f"{max(p[1] for p in pts):.1f} MiB/s",
                   size=10.5, anchor="end", fill=BLUE)
    legend(s, 700, 415,
           [("sync (baseline)", GRAY, "dash"),
            ("io_uring (64 KiB)", BLUE, "line")], cols=2)
    s.save(OUT / "fig2_cold_qd.svg")


# ---------------------------------------------------------------- fig 3
def ratio_color(r):
    """Map ratio 0.60..1.50 to a monotonic viridis-like gradient.

    Returns (rgb string, 1 if white text is needed else 0).
    """
    t = (min(max(r, 0.60), 1.50) - 0.60) / 0.90
    stops = [
        (0.00, (68, 1, 84)),
        (0.25, (59, 82, 139)),
        (0.50, (33, 145, 140)),
        (0.75, (94, 201, 98)),
        (1.00, (253, 231, 37)),
    ]
    c = stops[-1][1]
    for i in range(len(stops) - 1):
        t0, c0 = stops[i]
        t1, c1 = stops[i + 1]
        if t <= t1:
            f = (t - t0) / (t1 - t0)
            c = tuple(round(c0[j] + (c1[j] - c0[j]) * f) for j in range(3))
            break
    lum = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]
    return f"rgb({c[0]},{c[1]},{c[2]})", (1 if lum < 140 else 0)


def fig3(sda1):
    blocks = [4, 8, 16, 32, 64, 128, 256, 512, 1024]
    qds = [1, 4, 8, 16, 32, 64]
    s = SVG(1100, 560)
    s.text(550, 30, "io_uring / sync throughput ratio (sda1, USB 3.0)",
           size=16, anchor="middle", weight="bold")
    for pi, cache in enumerate(("cold", "hot")):
        left = 100 + pi * 540
        top = 85
        cw, chh = 44, 38
        s.text(left + (len(qds) * cw) / 2, top - 12, f"sda1 {cache}",
               size=14, anchor="middle", weight="bold")
        for j, q in enumerate(qds):
            s.text(left + j * cw + cw / 2, top + 14, f"Q{q}", size=10.5,
                   anchor="middle", fill="#444")
        for i, b in enumerate(blocks):
            y = top + 22 + i * chh
            s.text(left - 12, y + chh / 2 + 4, f"{b}K", size=10.5,
                   anchor="end", fill="#444")
            sync = sda1[cache].get((b, "sync"))
            if not sync:
                continue
            for j, q in enumerate(qds):
                u = sda1[cache].get((b, f"uring_qd{q}"))
                if not u:
                    continue
                r = u[0] / sync[0]
                fill, white = ratio_color(r)
                x = left + j * cw
                s.rect(x, y, cw - 2, chh - 2, fill=fill, stroke="#ffffff",
                       width=1.0, rx=2)
                s.text(x + (cw - 2) / 2, y + chh / 2 + 4, f"{r:.2f}",
                       size=9.5, anchor="middle",
                       fill="#ffffff" if white else "#222")
    gy = 500
    s.text(100, gy - 8, "ratio:", size=11, fill="#444")
    for k in range(60):
        r = 0.6 + (1.5 - 0.6) * k / 59
        fill, _ = ratio_color(r)
        s.rect(150 + k * 5, gy - 18, 5, 12, fill=fill, stroke="none", rx=0)
    s.text(150, gy + 8, "0.60 (slower)", size=10, fill="#444")
    s.text(283, gy + 8, "1.00", size=10, anchor="middle", fill="#444")
    s.text(450, gy + 8, "1.50 (faster)", size=10, fill="#444")
    s.save(OUT / "fig3_ratio_heatmap.svg")


# ---------------------------------------------------------------- fig 4
def fig4(sda1, sdb1):
    s = SVG(1100, 460)
    panels = [
        ("cold (64 KiB)", "cold", 10, 260,
         [0.8, 1, 2, 5, 10, 20, 50], [20, 60, 100, 140, 180, 220, 260]),
        ("hot (64 KiB)", "hot", 600, 1400,
         [0.8, 1, 2, 5, 10, 20, 50], [600, 800, 1000, 1200, 1400]),
    ]
    for i, (title, cache, ymin, ymax, xticks, yticks) in enumerate(panels):
        L = 90 + i * 555
        R, T, B = L + 450, 75, 350
        ch = Chart(s, L, R, T, B, 0.7, 80, ymin, ymax, xlog=True)
        ch.frame(xticks, yticks, "CPU cost (cpu-s/GiB, log)",
                 "Throughput (MiB/s)", title,
                 xticklabels=["0.7", "1", "2", "5", "10", "20", "50"])
        for data, color, shape in ((sdb1, ORANGE, "s"), (sda1, BLUE, "o")):
            sync = data[cache].get((64, "sync"))
            if sync:
                ch.scatter([(sync[1], sync[0])], color=color, shape=shape,
                           size=5.5, filled=True)
            pts = [(cpu, tput)
                   for (block, scheme), (tput, cpu) in data[cache].items()
                   if block == 64 and scheme != "sync"]
            ch.scatter(pts, color=color, shape=shape, size=4.2, filled=False)
        if i == 0:
            s.text(R - 8, T + 22,
                   "sdb1 io_uring: 53-55 cpu-s/GiB @ ~19.9 MiB/s",
                   size=10, anchor="end", fill=ORANGE)
    legend(s, 650, 425,
           [("sda1 USB 3.0 sync", BLUE, "line"),
            ("sda1 USB 3.0 io_uring", BLUE, "open"),
            ("sdb1 USB 2.0 sync", ORANGE, "square"),
            ("sdb1 USB 2.0 io_uring", ORANGE, "open")], cols=2, dx=260)
    s.save(OUT / "fig4_cpu_tput.svg")


# ---------------------------------------------------------------- fig 5
def fig5(http):
    sda1_t, sda1_c = http["sda1"]
    sdb1_t, sdb1_c = http["sdb1"]
    s = SVG(1100, 460)
    threads = [1, 2, 4, 8]
    panels = [
        ("HTTP throughput", 140, 450,
         [150, 200, 250, 300, 350, 400, 450], sda1_t, sdb1_t,
         "Throughput (MiB/s)"),
        ("Server CPU", 0, 135, [0, 30, 60, 90, 120], sda1_c, sdb1_c,
         "Server CPU (%)"),
    ]
    for i, (title, ymin, ymax, yticks, a, b, ylabel) in enumerate(panels):
        L = 95 + i * 555
        R, T, B = L + 445, 95, 345
        ch = Chart(s, L, R, T, B, 1, 8, ymin, ymax)
        ch.frame(threads, yticks, "Client threads", ylabel, title,
                 xticklabels=["1", "2", "4", "8"])
        series = [
            (a, BLUE, "o", "sync"),
            (a, RED, "o", "uring"),
            (b, GREEN, "s", "sync"),
            (b, ORANGE, "s", "uring"),
        ]
        for src, color, shape, mode in series:
            ch.polyline([(t, src[(mode, t)]) for t in threads],
                        color=color, marker=shape, msize=4.2)
    legend(s, 270, 420,
           [("sda1 USB 3.0 sync", BLUE, "line"),
            ("sda1 USB 3.0 uring", RED, "line"),
            ("sdb1 USB 2.0 sync", GREEN, "square"),
            ("sdb1 USB 2.0 uring", ORANGE, "square")], cols=2, dx=280)
    s.save(OUT / "fig5_http.svg")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    sda1 = load_io("run-sda1-full")
    sdb1 = load_io("run-sdb1-full")
    http = {"sda1": load_http("run-sda1-full"),
            "sdb1": load_http("run-sdb1-http")}
    print("generating figures ...")
    fig1()
    fig2(sda1, sdb1)
    fig3(sda1)
    fig4(sda1, sdb1)
    fig5(http)
    print("done.")


if __name__ == "__main__":
    main()
