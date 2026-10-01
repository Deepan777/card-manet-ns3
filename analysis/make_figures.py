#!/usr/bin/env python3
"""
Stage 9 figure generation with evidence sidecars.

Specification section 7 requires that every figure ship a sidecar recording the
raw run IDs, data hashes, the analysis command and the code version, so that no
number in the manuscript is untraceable.

Figures are emitted as SVG using only the standard library, so the analysis
environment needs no plotting dependency and the output is diffable text.
"""

import argparse
import hashlib
import json
import pathlib
import statistics as st
import subprocess
import sys
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parent.parent
CHECKPOINTS = ROOT / "checkpoints"
FIGURES = ROOT / "figures"


def sha256_text(s):
    return hashlib.sha256(s.encode("utf-8")).hexdigest()


def code_version():
    """Hash the SCR sources so a figure records exactly which code produced it."""
    h = hashlib.sha256()
    for p in sorted((ROOT / "ns3").rglob("*.cc")) + sorted((ROOT / "ns3").rglob("*.h")):
        h.update(p.read_bytes())
    return h.hexdigest()[:16]


# SINGLE SOURCE OF TRUTH. These were duplicated here until 2026-09-13 and the
# copies immediately drifted -- the audit caught it. A run must belong to the
# same experimental cell in every tool, so the definition lives in exactly one
# place and is imported. make_figures.py lives beside analyze_campaign.py, so
# the script directory is already on sys.path.
from analyze_campaign import CONDITION_FIELDS, condition_key, condition_label  # noqa: F401


def load_campaign(campaign):
    base = CHECKPOINTS / campaign
    rows = []
    for d in sorted(base.iterdir()):
        if not d.is_dir() or not (d / "DONE.json").exists():
            continue
        s = d / "out_summary.csv"
        if not s.exists():
            continue
        kv = {"_run_id": d.name}
        for line in s.read_text(errors="replace").splitlines()[1:]:
            if "," in line:
                k, _, v = line.partition(",")
                kv[k.strip()] = v.strip().strip('"')
        rows.append(kv)
    return rows


def svg_bar_chart(title, ylabel, labels, means, errs, path, width=900, height=460):
    """Minimal dependency-free grouped bar chart with error bars."""
    left, bottom, top, right = 90, 90, 50, 30
    plot_w = width - left - right
    plot_h = height - top - bottom
    vmax = max((m + (e or 0)) for m, e in zip(means, errs)) if means else 1.0
    vmax = vmax * 1.15 if vmax > 0 else 1.0

    n = len(labels)
    slot = plot_w / max(n, 1)
    bw = slot * 0.6

    def y(v):
        return top + plot_h - (v / vmax) * plot_h

    parts = []
    parts.append('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
                 'viewBox="0 0 %d %d" font-family="DejaVu Sans, Arial, sans-serif">'
                 % (width, height, width, height))
    parts.append('<rect width="%d" height="%d" fill="#ffffff"/>' % (width, height))
    parts.append('<text x="%d" y="28" font-size="17" font-weight="600">%s</text>'
                 % (left, title))

    # y axis with gridlines
    for i in range(6):
        v = vmax * i / 5
        yy = y(v)
        parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#e3e3e3"/>'
                     % (left, yy, left + plot_w, yy))
        parts.append('<text x="%d" y="%.1f" font-size="11" text-anchor="end" '
                     'fill="#444">%.4g</text>' % (left - 8, yy + 4, v))
    parts.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#333"/>'
                 % (left, top, left, top + plot_h))
    parts.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#333"/>'
                 % (left, top + plot_h, left + plot_w, top + plot_h))
    parts.append('<text x="18" y="%d" font-size="12" fill="#333" '
                 'transform="rotate(-90 18 %d)">%s</text>'
                 % (top + plot_h / 2, top + plot_h / 2, ylabel))

    for i, (lab, m, e) in enumerate(zip(labels, means, errs)):
        x = left + i * slot + (slot - bw) / 2
        yy = y(m)
        parts.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="#4a6fa5"/>'
                     % (x, yy, bw, top + plot_h - yy))
        if e:
            cx = x + bw / 2
            parts.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#222"/>'
                         % (cx, y(m - e), cx, y(m + e)))
            parts.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#222"/>'
                         % (cx - 6, y(m + e), cx + 6, y(m + e)))
            parts.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#222"/>'
                         % (cx - 6, y(m - e), cx + 6, y(m - e)))
        parts.append('<text x="%.1f" y="%d" font-size="10" text-anchor="end" fill="#333" '
                     'transform="rotate(-35 %.1f %d)">%s</text>'
                     % (x + bw / 2, top + plot_h + 16, x + bw / 2, top + plot_h + 16, lab))

    parts.append("</svg>")
    path.write_text("\n".join(parts), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--campaign", required=True)
    ap.add_argument("--figure", default="F1")
    ap.add_argument("--condition", default=None,
                    help="experimental cell to plot; required when the campaign "
                         "has more than one, since pooling them is meaningless")
    args = ap.parse_args()

    FIGURES.mkdir(exist_ok=True)
    rows = load_campaign(args.campaign)
    if not rows:
        sys.exit("no completed runs in " + args.campaign)

    # A campaign with several conditions cannot be reduced to one bar per
    # algorithm: that averages different experiments together. Refuse instead.
    present = sorted({condition_label(condition_key(kv)) for kv in rows})
    if len(present) > 1 and not args.condition:
        sys.exit(chr(10).join(
            ["%s has %d experimental conditions; pooling them into one bar per "
             "algorithm would average different experiments." % (args.campaign, len(present)),
             "Choose one with --condition:"] + ["   " + c for c in present]))
    if args.condition:
        rows = [kv for kv in rows if condition_label(condition_key(kv)) == args.condition]
        if not rows:
            sys.exit("no completed runs match --condition " + repr(args.condition))
    cond_label = args.condition or present[0]

    # Group control cost per generated packet by algorithm, within the cell.
    by_algo = {}
    run_ids = {}
    for kv in rows:
        algo = kv.get("algorithm", "?")
        gen = float(kv.get("generated", 0) or 0)
        rreq = float(kv.get("source_rreq_originated", 0) or 0)
        if gen <= 0:
            continue
        by_algo.setdefault(algo, []).append(rreq / gen)
        run_ids.setdefault(algo, []).append(kv["_run_id"])

    labels = sorted(by_algo)
    means = [st.mean(by_algo[a]) for a in labels]
    # Error bar = 1.96 * SEM, matching the registered interval convention.
    errs = []
    for a in labels:
        v = by_algo[a]
        errs.append(1.96 * st.stdev(v) / (len(v) ** 0.5) if len(v) > 1 else 0.0)

    fig_path = FIGURES / ("%s_control_cost.svg" % args.figure)
    svg_bar_chart(
        "%s  Source route-discovery cost by algorithm -- %s" % (args.figure, cond_label),
        "source RREQs per generated packet",
        labels, means, errs, fig_path)

    # ---- evidence sidecar -------------------------------------------------
    sidecar = {
        "figure": args.figure,
        "file": fig_path.name,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "campaign": args.campaign,
        "condition": cond_label,
        "analysis_command": " ".join([sys.executable.split("\\")[-1]] + sys.argv),
        "code_version_sha256_16": code_version(),
        "metric": "source_rreq_originated / generated",
        "error_bars": "1.96 * SEM",
        "algorithms": {},
        "figure_sha256": sha256_text(fig_path.read_text(encoding="utf-8")),
    }
    for a in labels:
        sidecar["algorithms"][a] = {
            "n_runs": len(by_algo[a]),
            "mean": st.mean(by_algo[a]),
            "values": by_algo[a],
            "run_ids": run_ids[a],
        }
    side_path = FIGURES / ("%s_control_cost.sidecar.json" % args.figure)
    side_path.write_text(json.dumps(sidecar, indent=2), encoding="utf-8")

    print("wrote %s" % fig_path)
    print("wrote %s" % side_path)
    for a, m, e in zip(labels, means, errs):
        print("  %-20s %.5f +/- %.5f  (n=%d)" % (a, m, e, len(by_algo[a])))


if __name__ == "__main__":
    main()
