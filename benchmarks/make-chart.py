#!/usr/bin/env python3
"""Render benchmark TSV → SVG bar chart → PNG (via ImageMagick).

Usage:
    ./make-chart.py results/<timestamp>.tsv [chart.png]

Reads the TSV produced by run-all.sh, draws a horizontal bar chart of
RPS per stack (with p99 latency annotation), writes SVG, converts to
PNG with `convert`. No matplotlib dep — keeps the bench tooling minimal.
"""
import os
import subprocess
import sys
from pathlib import Path

BAR_COLORS = {
    "spidong_cached":  "#0ea5e9",   # sky blue (us)
    "spidong_nocache": "#3b82f6",   # blue (us, cache off)
    "actix":           "#dc2626",   # rust red
    "go-chi":          "#10b981",   # emerald
    "postgrest":       "#f59e0b",   # amber
    "express":         "#84cc16",   # lime (was red, now actix is red)
    "fastapi":         "#a855f7",   # purple
}
DISPLAY = {
    "spidong_cached":  "SpidoNG (cache on)",
    "spidong_nocache": "SpidoNG (cache off)",
    "actix":           "Actix + tokio-postgres (Rust)",
    "go-chi":          "Go + chi + pgx",
    "postgrest":       "PostgREST",
    "express":         "Express + pg (Node)",
    "fastapi":         "FastAPI + asyncpg (Python)",
}


def fmt_us(us: float) -> str:
    if us < 1000:    return f"{us:.0f} µs"
    if us < 1000_000: return f"{us/1000:.1f} ms"
    return f"{us/1000_000:.2f} s"


def render_svg(rows, title):
    W = 980
    BAR_H = 56
    GAP = 18
    PAD_L = 240
    PAD_R = 200
    PAD_T = 80
    PAD_B = 60
    plot_w = W - PAD_L - PAD_R
    H = PAD_T + PAD_B + len(rows) * (BAR_H + GAP)

    max_rps = max(r["rps"] for r in rows)

    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
             f'viewBox="0 0 {W} {H}" font-family="-apple-system,Segoe UI,sans-serif">']
    # Background
    parts.append(f'<rect width="{W}" height="{H}" fill="#fafafa"/>')
    # Title
    parts.append(
        f'<text x="{W/2}" y="32" text-anchor="middle" font-size="22" '
        f'font-weight="700" fill="#111">{title}</text>')
    parts.append(
        f'<text x="{W/2}" y="56" text-anchor="middle" font-size="13" '
        f'fill="#555">Same hardware · same PostgreSQL · same /products endpoint · '
        f'wrk -t8 -c64 -d10s</text>')

    for i, row in enumerate(rows):
        y = PAD_T + i * (BAR_H + GAP)
        w = plot_w * row["rps"] / max_rps
        color = BAR_COLORS.get(row["stack"], "#888")
        label = DISPLAY.get(row["stack"], row["stack"])

        # Stack name (left)
        parts.append(
            f'<text x="{PAD_L - 14}" y="{y + BAR_H/2 + 6}" text-anchor="end" '
            f'font-size="15" font-weight="600" fill="#222">{label}</text>')
        # Bar
        parts.append(
            f'<rect x="{PAD_L}" y="{y}" width="{w:.1f}" height="{BAR_H}" '
            f'rx="6" fill="{color}" />')
        # RPS label inside bar (or right of bar if too small)
        rps_text = f'{row["rps"]:,.0f} req/s'
        if w > 200:
            parts.append(
                f'<text x="{PAD_L + 14}" y="{y + BAR_H/2 - 4}" font-size="20" '
                f'font-weight="700" fill="white">{rps_text}</text>')
            parts.append(
                f'<text x="{PAD_L + 14}" y="{y + BAR_H/2 + 18}" font-size="11" '
                f'fill="rgba(255,255,255,0.85)">p50 {fmt_us(row["p50"])} · '
                f'p99 {fmt_us(row["p99"])}</text>')
        else:
            parts.append(
                f'<text x="{PAD_L + w + 12}" y="{y + BAR_H/2 - 4}" font-size="16" '
                f'font-weight="700" fill="#222">{rps_text}</text>')
            parts.append(
                f'<text x="{PAD_L + w + 12}" y="{y + BAR_H/2 + 16}" font-size="11" '
                f'fill="#555">p50 {fmt_us(row["p50"])} · p99 {fmt_us(row["p99"])}</text>')

    # Footer
    parts.append(
        f'<text x="{W/2}" y="{H - 24}" text-anchor="middle" font-size="11" '
        f'fill="#666">Higher bar = better. RPS = requests/sec sustained '
        f'over 10s. Latency = end-to-end inside wrk.</text>')
    parts.append('</svg>')
    return "\n".join(parts)


def parse_tsv(path):
    rows = []
    with open(path) as f:
        next(f)  # header
        for line in f:
            cells = line.strip().split("\t")
            if len(cells) < 5:
                continue
            rows.append({
                "stack": cells[0],
                "rps":   float(cells[1]),
                "p50":   float(cells[2]),
                "p90":   float(cells[3]),
                "p99":   float(cells[4]),
            })
    rows.sort(key=lambda r: r["rps"], reverse=True)
    return rows


def main():
    if len(sys.argv) < 2:
        print("usage: make-chart.py <tsv> [out.png]")
        sys.exit(1)
    tsv = Path(sys.argv[1])
    out_png = Path(sys.argv[2]) if len(sys.argv) > 2 else tsv.with_suffix(".png")
    out_svg = out_png.with_suffix(".svg")

    rows = parse_tsv(tsv)
    if not rows:
        print("no rows in TSV"); sys.exit(2)

    svg = render_svg(rows, "REST stack throughput @ same PG (10k seed products)")
    out_svg.write_text(svg)
    print(f"wrote {out_svg}")

    # SVG → PNG via ImageMagick. Density 144 gives crisp output for README.
    subprocess.check_call([
        "convert", "-density", "144", "-background", "#fafafa",
        str(out_svg), str(out_png)
    ])
    print(f"wrote {out_png}")


if __name__ == "__main__":
    main()
