"""Fig. 4 of the manuscript (renewal mechanism), with the manuscript's policy names.

The plotting code is the function f3m of analysis/make_q1_figures.py, copied unchanged except for three display
labels ("service-level" -> "confirmation renewal") and the output folder. It reads only the stored analysis results
analysis_out/evidence_lag.json and analysis_out/q1_tables.json; nothing is recomputed.
Writes paper/figures/F3_mechanism.pdf and its JSON sidecar.
Usage (from the project root): python paper/scripts/make_mechanism_figure.py
"""

import json
import os
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
os.environ.setdefault("MPLCONFIGDIR", str(ROOT / "paper" / "scripts" / "out" / "mplcache"))

import matplotlib  # noqa: E402
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

FIG = ROOT / "paper" / "figures"
INK, MUTED, GRID = "#0b0b0b", "#52514e", "#d9d8d4"
WIDTH = 6.3
plt.rcParams.update({
    "pdf.fonttype": 42, "ps.fonttype": 42, "font.size": 8, "axes.titlesize": 8.5,
    "axes.labelsize": 8, "xtick.labelsize": 7.5, "ytick.labelsize": 7.5,
    "axes.edgecolor": MUTED, "axes.labelcolor": INK, "xtick.color": MUTED,
    "ytick.color": MUTED, "axes.linewidth": 0.6, "axes.spines.top": False,
    "axes.spines.right": False, "legend.frameon": False, "font.family": "DejaVu Sans",
})


def save(fig, name, data):
    FIG.mkdir(parents=True, exist_ok=True)
    fig.savefig(FIG / (name + ".pdf"), bbox_inches="tight")
    plt.close(fig)
    (FIG / (name + ".json")).write_text(json.dumps(data, indent=1, default=float))
    print("wrote", name)


def grid(ax, axis="y"):
    ax.grid(axis=axis, color=GRID, linewidth=0.5)
    ax.set_axisbelow(True)


def f3m():
    lag = json.loads((ROOT / "analysis_out" / "evidence_lag.json").read_text())["lag"]
    iv = json.loads((ROOT / "analysis_out" / "q1_tables.json").read_text())["interventions"]
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(WIDTH, 2.75), gridspec_kw={"width_ratios": [1, 1.45]})

    styles = {"C1": ("Bottleneck", INK, "-"), "S8": ("Mobility", MUTED, (0, (4, 2)))}
    data_a = {}
    for tag, (nm, col, ls) in styles.items():
        e = lag[tag]["AODV-STOCK"]["ecdf_ms"]
        xs = [v / 1000 for v in e if v is not None]
        ys = [i / 200 for i, v in enumerate(e) if v is not None]
        ax1.step(xs, ys, where="post", color=col, ls=ls, lw=1.4, label=nm)
        data_a[nm] = {"lag_s": xs, "fraction": ys,
                      "never_confirmable": lag[tag]["AODV-STOCK"]["never_confirmable_fraction"]}
    ax1.axvline(1.0, color=MUTED, lw=0.7, ls=(0, (1, 2)))
    ax1.text(1.06, 0.03, "$KMI$ at 20 pkt/s", fontsize=6.6, color=MUTED, rotation=90, va="bottom")
    ax1.set_xscale("log")
    ax1.set_xlim(0.3, 30)
    ax1.set_xticks([0.3, 1, 3, 10, 30])
    ax1.set_xticklabels(["0.3", "1", "3", "10", "30"])
    ax1.set_ylim(0, 1)
    ax1.set_xlabel("Delay after service resumes (s)")
    ax1.set_ylabel("Recoveries confirmable")
    ax1.legend(loc="upper left", fontsize=6.8, handlelength=2.2)
    ax1.set_title("(a) Evidence formation after recovery", loc="left")
    grid(ax1, "both")

    order = ["evidence rule relaxed", "dependency removed", "escape path removed", "other components"]
    rows = iv["rows"]
    y, ticks, labels, heads, data_b = 0.0, [], [], [], []
    for g in order:
        ticks.append(y); labels.append(g); heads.append(len(labels) - 1)
        y -= 0.8
        for r in [r for r in rows if r["group"] == g]:
            key = g in ("dependency removed", "escape path removed")
            col = INK if key else MUTED
            ax2.plot(r["ci"], [y, y], color=col, lw=1.3, solid_capstyle="round", zorder=3)
            ax2.scatter([r["mean"]], [y], s=22 if key else 14, color=col, zorder=4,
                        marker="s" if r["family"] == "Bottleneck" else "o")
            ticks.append(y)
            labels.append("%s (%s)" % (r["label"], r["family"].lower()))
            data_b.append(r)
            y -= 0.8
        y -= 0.25
    ticks.append(y); labels.append("for scale: confirmation renewal minus stock"); heads.append(len(labels) - 1)
    y -= 0.8
    for fam in ("Bottleneck", "Mobility"):
        r = iv["scr_minus_stock"][fam]
        ax2.plot(r["ci"], [y, y], color=GRID, lw=4.5, solid_capstyle="butt", zorder=2)
        ax2.scatter([r["mean"]], [y], s=14, color=MUTED, zorder=3, marker="s" if fam == "Bottleneck" else "o")
        ticks.append(y)
        labels.append("deficit (%s)" % fam.lower())
        y -= 0.8
    ax2.axvline(0, color=INK, lw=0.8, ls=(0, (4, 2)))
    ax2.set_yticks(ticks)
    ax2.set_yticklabels(labels, fontsize=6.6)
    for i in heads:
        ax2.get_yticklabels()[i].set_fontweight("bold")
        ax2.get_yticklabels()[i].set_color(INK)
    ax2.set_xlim(-0.42, 0.34)
    ax2.set_xticks([-0.4, -0.2, 0, 0.2])
    ax2.set_ylim(y + 0.4, 0.5)
    ax2.set_xlabel("Change in timely PDR (paired, 95% CI)")
    ax2.spines["left"].set_visible(False)
    ax2.tick_params(axis="y", length=0)
    ax2.set_title("(b) Altering confirmation renewal", loc="left")
    grid(ax2, "x")
    fig.tight_layout(w_pad=1.2)
    save(fig, "F3_mechanism", {"evidence_lag": data_a, "interventions": data_b,
                                "scr_minus_stock": iv["scr_minus_stock"]})


if __name__ == "__main__":
    f3m()
