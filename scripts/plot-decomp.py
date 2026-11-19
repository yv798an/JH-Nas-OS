#!/usr/bin/env python3
"""Draw the board-side decomposition figures from analyze-decomp.py output.

Inputs:
    NAS_Test/decomp-time.csv
    NAS_Test/decomp-threads.csv
Outputs:
    NAS_Test/figures/fig6_time_breakdown.svg
    NAS_Test/figures/fig7_context_switches.svg

Usage:
    python scripts/analyze-decomp.py
    python scripts/plot-decomp.py
"""

import csv
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NT = ROOT / "NAS_Test"
FIG = NT / "figures"

BLUE = "#1f77b4"
ORANGE = "#e07b00"
RED = "#d62728"
GREEN = "#2ca02c"
GRAY = "#9e9e9e"
PURPLE = "#7b4fa0"


def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class SVG:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.parts = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
            f'viewBox="0 0 {w} {h}">',
            f'<rect width="{w}" height="{h}" fill="#ffffff"/>',
        ]

    def rect(self, x, y, w, h, fill, stroke="none", sw=0.8):
        self.parts.append(
            f'<rect x="{x:.2f}" y="{y:.2f}" width="{w:.2f}" height="{h:.2f}" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>')

    def text(self, x, y, s, size=12, anchor="start", fill="#222",
             weight="normal", rotate=None):
        tr = f' transform="rotate({rotate} {x:.2f} {y:.2f})"' if rotate else ""
        self.parts.append(
            f'<text x="{x:.2f}" y="{y:.2f}" font-family="Arial, Helvetica, '
            f'sans-serif" font-size="{size}" fill="{fill}" '
            f'font-weight="{weight}" text-anchor="{anchor}"{tr}>{esc(s)}</text>')

    def line(self, x1, y1, x2, y2, stroke="#333", width=1.0):
        self.parts.append(
            f'<line x1="{x1:.2f}" y1="{y1:.2f}" x2="{x2:.2f}" '
            f'y2="{y2:.2f}" stroke="{stroke}" stroke-width="{width}"/>')

    def save(self, path):
        self.parts.append("</svg>")
        path.write_text("\n".join(self.parts), encoding="utf-8")
        print(f"  wrote {path.relative_to(ROOT)}")


def read_csv(path):
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def num(row, key, default=0.0):
    try:
        return float(row.get(key, default))
    except (TypeError, ValueError):
        return default


def stacked_bar(svg, x, y, w, h, segments):
    yy = y + h
    for _, pct, color in segments:
        hh = max(0.0, min(100.0, pct)) / 100.0 * h
        yy -= hh
        svg.rect(x, yy, w, hh, fill=color, stroke="#ffffff")


def legend(svg, x, y, items, dx=150):
    for i, (label, color) in enumerate(items):
        xx = x + i * dx
        svg.rect(xx, y - 10, 12, 12, fill=color, stroke="#ffffff")
        svg.text(xx + 18, y + 1, label, size=12)


def fig6(rows):
    if not rows:
        print("  skip fig6: no decomp-time.csv")
        return
    s = SVG(1000, 500)
    s.text(500, 28, "Board-side time decomposition (64 KiB, QD=8)",
           size=17, anchor="middle", weight="bold")
    left, top, height = 90, 80, 320
    width, gap = 360, 40
    barw, inner = 40, 14
    for v in (0, 25, 50, 75, 100):
        y = top + height - v / 100.0 * height
        s.line(left, y, left + width, y, stroke="#e6e6e6")
        s.text(left - 10, y + 4, f"{v}%", size=11.5, anchor="end", fill="#555")
    s.line(left, top + height, left + width, top + height, stroke="#333")
    s.line(left, top, left, top + height, stroke="#333")

    for i, r in enumerate(rows[:4]):
        x = left + 30 + i * 92
        submit = num(r, "submit_pct")
        handoff = num(r, "handoff_pct")
        queue = min(num(r, "queue_pct"), handoff)
        wake = max(0.0, handoff - queue)
        processing = max(0.0, 100.0 - handoff)
        # reader: submit+I/O wait vs other
        stacked_bar(s, x, top, barw, height, [
            ("submit", submit, BLUE),
            ("other", max(0.0, 100.0 - submit), GRAY),
        ])
        # consumer: handoff wall split into queue / wakeup, plus processing
        stacked_bar(s, x + barw + inner, top, barw, height, [
            ("queue", queue, ORANGE),
            ("wake", wake, RED),
            ("processing", processing, GREEN),
        ])
        label = r["scenario"].replace("_", "\n")
        s.text(x + barw + inner / 2, top + height + 34, label, size=11,
               anchor="middle", fill="#333")
        s.text(x + barw / 2, top + height + 16, "R", size=11, anchor="middle",
               fill=BLUE)
        s.text(x + barw + inner + barw / 2, top + height + 16, "C", size=11,
               anchor="middle", fill=PURPLE)

    legend(s, 120, 455, [
        ("submit/I-O", BLUE), ("queue", ORANGE), ("handoff wait", RED),
        ("processing", GREEN), ("other", GRAY)], dx=145)
    s.text(500, 490, "R = reader thread, C = consumer thread; "
           "bars are percentages of run wall time", size=11, anchor="middle",
           fill="#666")
    s.save(FIG / "fig6_time_breakdown.svg")


def fig7(rows):
    rows = [r for r in rows if "_t8_" in r.get("scenario", "")]
    if not rows:
        print("  skip fig7: no t8 thread data")
        return
    groups = {}
    for r in rows:
        g = groups.setdefault(r["scenario"], {"vcsw": 0, "nivcsw": 0})
        g["vcsw"] += int(r["vcsw"])
        g["nivcsw"] += int(r["nivcsw"])

    s = SVG(1000, 440)
    s.text(500, 28, "HTTP t8 context switches (procfs, log scale)",
           size=17, anchor="middle", weight="bold")
    left, top, height = 100, 80, 280
    width = 780
    ymin, ymax = 50.0, 200000.0
    ticks = [50, 100, 500, 1000, 5000, 10000, 50000, 100000, 200000]

    def Y(v):
        v = max(v, ymin)
        t = (math.log10(v) - math.log10(ymin)) / (math.log10(ymax) -
                                                  math.log10(ymin))
        return top + height - t * height

    for v in ticks:
        y = Y(v)
        s.line(left, y, left + width, y, stroke="#e6e6e6")
        s.text(left - 10, y + 4, f"{v}", size=11.5, anchor="end", fill="#555")
    s.line(left, top + height, left + width, top + height, stroke="#333")
    s.line(left, top, left, top + height, stroke="#333")

    scenarios = sorted(groups.keys())
    barw, gap = 40, 8
    step = width / max(1, len(scenarios))
    for i, scenario in enumerate(scenarios):
        g = groups[scenario]
        x = left + i * step + (step - (2 * barw + gap)) / 2
        for j, (label, value, color) in enumerate([
                ("vcsw", g["vcsw"], BLUE), ("nivcsw", g["nivcsw"], RED)]):
            y = Y(value)
            s.rect(x + j * (barw + gap), y, barw, top + height - y,
                   fill=color, stroke="#ffffff")
            s.text(x + j * (barw + gap) + barw / 2, y - 5, f"{value}",
                   size=11, anchor="middle", fill=color)
        s.text(x + barw + gap / 2, top + height + 34,
               scenario.replace("http_", "").replace("_t8_", "\nt8 "),
               size=11, anchor="middle", fill="#333")
    legend(s, 250, 405, [("voluntary", BLUE), ("nonvoluntary", RED)],
           dx=170)
    s.save(FIG / "fig7_context_switches.svg")


def main():
    FIG.mkdir(parents=True, exist_ok=True)
    fig6(read_csv(NT / "decomp-time.csv"))
    fig7(read_csv(NT / "decomp-threads.csv"))


if __name__ == "__main__":
    main()
