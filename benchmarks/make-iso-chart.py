#!/usr/bin/env python3
"""Render pressure-isolation results → SVG → PNG.

The chart shows three endpoints (payments, products, analytics) on each
of two stacks (SpidoNG vs Actix), measured while a noisy neighbour
hammered /analytics with 256 connections. Two bars per endpoint: rps
and p99 latency.
"""
import subprocess
import sys
from pathlib import Path

# Numbers come from results/iso_20260515_183746_<stack>_attack_<ep>.log
# (re-runnable via run-isolation-simple.sh — see scripts/).
DATA = {
    "SpidoNG (priority + token bucket)": [
        ("/payments  (critical)",  22026, 1.21, 5.77),
        ("/products  (normal)",    18257, 1.49, 6.18),
        ("/analytics (best_effort, attacked)", 140041, 1.54, 6.44),
    ],
    "Actix + tokio-postgres (no admission control)": [
        ("/payments  (critical)",   9710, 3.15, 5.80),
        ("/products  (normal)",     8050, 3.83, 6.81),
        ("/analytics (attacked)",  74314, 3.28, 6.24),
    ],
}

PALETTE = [
    "#0ea5e9",   # critical/payments
    "#3b82f6",   # normal/products
    "#a855f7",   # best_effort/analytics
]


def render():
    W = 1100
    BAR_H = 38
    GAP = 14
    GROUP_GAP = 38
    PAD_L = 290
    PAD_R = 260
    PAD_T = 110
    PAD_B = 80
    plot_w = W - PAD_L - PAD_R

    rows = []
    for stack, items in DATA.items():
        for ep, rps, p50, p99 in items:
            rows.append((stack, ep, rps, p50, p99))

    H = PAD_T + PAD_B + len(rows) * (BAR_H + GAP) + GROUP_GAP

    max_rps = max(r[2] for r in rows)

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
        f'viewBox="0 0 {W} {H}" font-family="-apple-system,Segoe UI,sans-serif">'
    ]
    parts.append(f'<rect width="{W}" height="{H}" fill="#fafafa"/>')

    # Title
    parts.append(
        f'<text x="{W/2}" y="34" text-anchor="middle" font-size="22" '
        f'font-weight="700" fill="#111">Pressure isolation under attack</text>')
    parts.append(
        f'<text x="{W/2}" y="58" text-anchor="middle" font-size="13" '
        f'fill="#555">Noisy neighbour: 256-conn wrk hammering /analytics for 30s. '
        f'Meanwhile /payments + /products measured at 32 conn each.</text>')
    parts.append(
        f'<text x="{W/2}" y="78" text-anchor="middle" font-size="13" '
        f'fill="#555">Same hardware · same PG · 5k payments + 10k products + 10k events seed</text>')

    y = PAD_T
    last_stack = None
    for i, (stack, ep, rps, p50, p99) in enumerate(rows):
        if last_stack is not None and stack != last_stack:
            y += GROUP_GAP
        if stack != last_stack:
            parts.append(
                f'<text x="20" y="{y + 12}" font-size="14" font-weight="700" '
                f'fill="#111">{stack}</text>')
            last_stack = stack
            y += 20

        w = plot_w * rps / max_rps
        color = PALETTE[i % 3]

        # Endpoint label (left)
        parts.append(
            f'<text x="{PAD_L - 14}" y="{y + BAR_H/2 + 5}" text-anchor="end" '
            f'font-size="13" fill="#222" font-family="ui-monospace,monospace">{ep}</text>')
        # Bar
        parts.append(
            f'<rect x="{PAD_L}" y="{y}" width="{w:.1f}" height="{BAR_H}" '
            f'rx="5" fill="{color}" />')
        # Inside bar: RPS
        rps_text = f'{rps:,} req/s'
        if w > 220:
            parts.append(
                f'<text x="{PAD_L + 12}" y="{y + BAR_H/2 + 5}" font-size="16" '
                f'font-weight="700" fill="white">{rps_text}</text>')
        else:
            parts.append(
                f'<text x="{PAD_L + w + 8}" y="{y + BAR_H/2 + 5}" font-size="15" '
                f'font-weight="700" fill="#222">{rps_text}</text>')
        # Right: p50 + p99
        parts.append(
            f'<text x="{PAD_L + plot_w + 14}" y="{y + BAR_H/2 + 5}" '
            f'font-size="12" fill="#555">p50 {p50:.2f} ms · p99 {p99:.2f} ms</text>')
        y += BAR_H + GAP

    parts.append(
        f'<text x="{W/2}" y="{H - 32}" text-anchor="middle" font-size="12" '
        f'fill="#666">Under attack SpidoNG keeps /payments at <tspan font-weight="700">22k req/s</tspan>; '
        f'Actix drops to 9.7k. Token-bucket admission + per-endpoint priority do the work.</text>')
    parts.append('</svg>')
    return "\n".join(parts)


def main():
    out_dir = Path(__file__).resolve().parent / "results"
    out_dir.mkdir(parents=True, exist_ok=True)
    svg = out_dir / "pressure_isolation.svg"
    png = out_dir / "pressure_isolation.png"
    svg.write_text(render())
    print(f"wrote {svg}")
    subprocess.check_call([
        "convert", "-density", "144", "-background", "#fafafa",
        str(svg), str(png),
    ])
    print(f"wrote {png}")


if __name__ == "__main__":
    main()
