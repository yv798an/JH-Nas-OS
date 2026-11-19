#!/usr/bin/env python3
"""Export NAS_Test/figures/*.svg to vector PDF (and optional EPS).

Usage:
    python scripts/export-figures.py            # PDF
    python scripts/export-figures.py --eps      # PDF + EPS (if converter exists)

Preference order:
    rsvg-convert -> inkscape -> cairosvg -> Chromium/Edge print-to-pdf.

The Chromium fallback inlines the SVG into an HTML page with an exact
`@page` size, so the PDF stays vector (no rasterized screenshot).
"""

import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "NAS_Test" / "figures"
OUT = SRC / "pdf"


def find_chromium():
    for name in ("msedge", "microsoft-edge", "chromium", "chromium-browser",
                 "google-chrome", "google-chrome-stable", "chrome"):
        p = shutil.which(name)
        if p:
            return p
    candidates = [
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    ]
    for c in candidates:
        if Path(c).exists():
            return c
    return None


def svg_size(path):
    text = path.read_text(encoding="utf-8")
    m = re.search(r'<svg[^>]*\bwidth="([0-9.]+)"[^>]*\bheight="([0-9.]+)"',
                  text)
    if not m:
        raise RuntimeError(f"cannot read size from {path}")
    return m.group(1), m.group(2)


def export_chromium(chromium, svg, pdf):
    w, h = svg_size(svg)
    text = svg.read_text(encoding="utf-8")
    html = f"""<!doctype html>
<html><head><meta charset="utf-8"><style>
@page {{ size: {w}px {h}px; margin: 0; }}
html, body {{ margin: 0; padding: 0; background: #fff; }}
svg {{ display: block; }}
</style></head><body>{text}</body></html>"""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        page = tmp / "page.html"
        page.write_text(html, encoding="utf-8")
        profile = tmp / "profile"
        cmd = [chromium, "--headless", "--disable-gpu",
               "--no-pdf-header-footer",
               f"--user-data-dir={profile}",
               f"--print-to-pdf={pdf}", page.as_uri()]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)


def export_one(svg, pdf, eps):
    tools = []
    if shutil.which("rsvg-convert"):
        tools.append("rsvg")
    if shutil.which("inkscape"):
        tools.append("inkscape")
    if shutil.which("cairosvg"):
        tools.append("cairosvg")

    if "rsvg" in tools:
        subprocess.run(["rsvg-convert", "-f", "pdf", "-o", str(pdf), str(svg)],
                       check=True)
        if eps:
            subprocess.run(["rsvg-convert", "-f", "eps", "-o",
                            str(pdf.with_suffix(".eps")), str(svg)],
                           check=True)
        return "rsvg-convert"
    if "inkscape" in tools:
        subprocess.run(["inkscape", str(svg), "--export-type=pdf",
                        f"--export-filename={pdf}"], check=True)
        if eps:
            subprocess.run(["inkscape", str(svg), "--export-type=eps",
                            f"--export-filename={pdf.with_suffix('.eps')}"],
                           check=True)
        return "inkscape"
    if "cairosvg" in tools:
        subprocess.run(["cairosvg", str(svg), "-o", str(pdf)], check=True)
        if eps:
            subprocess.run(["cairosvg", str(svg), "-o",
                            str(pdf.with_suffix(".eps"))], check=True)
        return "cairosvg"

    chromium = find_chromium()
    if chromium:
        export_chromium(chromium, svg, pdf)
        return Path(chromium).name
    raise RuntimeError("no converter found: install rsvg-convert, inkscape, "
                       "cairosvg, or Chrome/Edge")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eps", action="store_true", help="also export EPS")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    svgs = sorted(SRC.glob("*.svg"))
    if not svgs:
        print("no SVG files")
        return
    for svg in svgs:
        pdf = OUT / (svg.stem + ".pdf")
        tool = export_one(svg, pdf, args.eps)
        size = pdf.stat().st_size if pdf.exists() else 0
        print(f"  {svg.name:32s} -> pdf/{pdf.name:32s} "
              f"({size} bytes, {tool})")


if __name__ == "__main__":
    main()
