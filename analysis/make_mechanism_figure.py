#!/usr/bin/env python3
"""
Figure F1: the per-pair episode state machine and the four renewal rules.

This is the one figure in the paper not generated from run data, because it
depicts the specification rather than a measurement. It is generated from code
rather than drawn by hand so that the transitions shown are the ones the text
describes, and so it can be regenerated if the mechanism changes.

The point the figure has to make is the asymmetry: all four arms share the same
states and the same spend path, and differ only in which arrow restores the
allowance.
"""

import pathlib

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "figures"
OUT.mkdir(exist_ok=True)

INK = "#1a202c"
MUTED = "#718096"
ACCENT = "#c05621"


def box(ax, x, y, w, h, label, sub, fc="#ffffff", ec=INK):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.02",
                                linewidth=1.4, edgecolor=ec, facecolor=fc))
    ax.text(x + w / 2, y + h * 0.62, label, ha="center", va="center",
            fontsize=10, color=INK, fontweight="bold")
    ax.text(x + w / 2, y + h * 0.26, sub, ha="center", va="center",
            fontsize=7.5, color=MUTED)


def arrow(ax, p, q, label, color=INK, rad=0.0, dy=0.06, style="-|>"):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle=style, mutation_scale=13,
                                 linewidth=1.3, color=color,
                                 connectionstyle="arc3,rad=%.2f" % rad))
    mx, my = (p[0] + q[0]) / 2, (p[1] + q[1]) / 2
    ax.text(mx, my + dy, label, ha="center", va="bottom", fontsize=7.6, color=color)


def main():
    fig, ax = plt.subplots(figsize=(9.2, 4.6))
    ax.set_xlim(0, 10)
    ax.set_ylim(0, 5)
    ax.axis("off")

    box(ax, 0.25, 2.6, 2.1, 1.0, "IDLE", "no demand")
    box(ax, 3.7, 2.6, 2.4, 1.0, "RECOVERING", "spending allowance")
    box(ax, 7.4, 2.6, 2.2, 1.0, "SERVING", "allowance restored")

    arrow(ax, (2.35, 3.1), (3.7, 3.1), "demand, no route\n(episode opens)")
    arrow(ax, (6.1, 3.1), (7.4, 3.1), "renewal rule fires", color=ACCENT)
    arrow(ax, (7.4, 2.75), (6.1, 2.75), "route lost\n(next episode)", rad=-0.0, dy=-0.42)

    # the spend path, shared by every arm
    ax.text(4.9, 2.35, "each origination:  B -= 1,  j += 1,  TTL = min(35, 2^(j+1))",
            ha="center", va="top", fontsize=8, color=MUTED)
    ax.text(4.9, 1.98, "B exhausted  ->  probe token (rho = 0.2/s) keeps the pair alive",
            ha="center", va="top", fontsize=8, color=MUTED)

    # what differs
    ax.text(0.25, 1.35, "What restores the allowance — the only difference between arms:",
            fontsize=8.6, color=INK, fontweight="bold")
    rules = [
        ("RREP-RESET", "a route reply completing an outstanding discovery", "immediate, weak"),
        ("TIME-BUCKET", "elapsed time (token bucket, no feedback)", "not evidence"),
        ("SCR", "K=2 consecutive good blocks, on-time, fresh, in receipts", "delayed, strong"),
        ("PERSISTENT-BACKOFF", "nothing; interval min(0.5*2^k, 16) s", "unobtainable"),
    ]
    y = 1.02
    for name, rule, cost in rules:
        ax.text(0.45, y, name, fontsize=8, color=ACCENT, fontweight="bold")
        ax.text(2.75, y, rule, fontsize=8, color=INK)
        ax.text(8.05, y, cost, fontsize=7.6, color=MUTED, style="italic")
        y -= 0.27

    ax.text(4.9, 4.45,
            "One implementation, four arms: identical states, identical spend path,"
            " different renewal rule",
            ha="center", fontsize=9.5, color=INK)

    fig.tight_layout()
    fig.savefig(OUT / "F1_mechanism.svg")
    fig.savefig(str(OUT / "F1_mechanism.svg").replace(".svg", ".pdf"))
    plt.close(fig)
    print("wrote figures/F1_mechanism.svg")


if __name__ == "__main__":
    main()
