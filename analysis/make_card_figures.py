"""Generate the CARD data figures as vector PDFs, with JSON sidecars (R053-R055).

  F_card_scenarios  every scenario: timely PDR minus stock AODV, paired by seed,
                    95% bootstrap CI, for route-level renewal, CARD and the two
                    cause-blind policies; one panel per family
  F_card_tradeoff   delivery against control cost relative to stock AODV:
                    family means (large) and scenarios (small)

Figure 2 (the CARD schematic) is TikZ in manuscript/fig_card.tex. Palette and
styling follow make_q1_figures.py.

Run:  python analysis/make_card_figures.py
"""

import json
import statistics as st

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from q1_common import ROOT, boot, conditions, load, load_card, paired, val  # noqa: E402

FIG = ROOT / "figures" / "q1"
INK, MUTED, GRID = "#0b0b0b", "#52514e", "#d9d8d4"
SLOT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
WIDTH = 6.3
FAMILY = [("C1", "c1_outage", "Bottleneck"), ("C4", "c4_scaling", "Scaling"),
          ("S8", "stage8_campaign", "Mobility")]
SERIES = [("CLAF-AODV", "CLAF-AODV (2023)", MUTED, "P"),
          ("TAAODV", "TAAODV (2025)", "#9b9a95", "X"),
          ("RREP-RESET", "Route-level renewal", SLOT[1], "s"),
          ("CARD-NO-CAUSE", "Cause-blind reach", SLOT[2], "^"),
          ("CARD-EXEMPT-ALL", "Every break exempt", SLOT[3], "D"),
          ("CARD", "CARD", SLOT[0], "o")]

plt.rcParams.update({
    "pdf.fonttype": 42, "ps.fonttype": 42, "font.size": 8, "axes.titlesize": 8.5,
    "axes.labelsize": 8, "xtick.labelsize": 7, "ytick.labelsize": 7.5,
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


def label(tag, c):
    """Short scenario label for the x axis."""
    if tag == "C1":
        return {"healthy": "healthy", "single": "single", "repeated": "repeat",
                "long": "long", "cold": "cold"}.get(c[0], c[0]) + ("+st" if c[1] == "1" else "")
    if tag == "S8":
        v, r, f = c
        return "%sm/s %spps %sfl" % (v, r, f)
    n, a, f, p = c
    return "N%s %sm %sfl %sB" % (n, int(float(a)), f, p)


def rows_for(camp):
    r = load(camp)
    r.update(load_card(camp))
    return r


def fam_data():
    out = {}
    for tag, camp, name in FAMILY:
        rows = rows_for(camp)
        conds = [c for c in conditions(rows) if any(a == "CARD" and cc == c for (a, cc, _) in rows)]
        out[tag] = (name, rows, conds)
    return out


def f_scenarios(D):
    fig, axes = plt.subplots(3, 1, figsize=(WIDTH, 6.4))
    data = {}
    for ax, (tag, _, _) in zip(axes, FAMILY):
        name, rows, conds = D[tag]
        data[tag] = {}
        if not conds:
            ax.set_visible(False)
            continue
        k = len(SERIES)
        for j, (arm, lab, col, mk) in enumerate(SERIES):
            xs, ys, lo, hi = [], [], [], []
            for i, c in enumerate(conds):
                d = paired(rows, arm, "AODV-STOCK", c)
                if len(d) < 3:
                    continue
                l, h = boot(d)
                m = st.mean(d)
                xs.append(i + (j - (k - 1) / 2) * 0.14)
                ys.append(m)
                lo.append(m - l)
                hi.append(h - m)
                data[tag].setdefault(arm, []).append({"condition": list(c), "mean": m,
                                                      "lo": l, "hi": h, "n": len(d)})
            if xs:
                ax.errorbar(xs, ys, yerr=[lo, hi], fmt=mk, ms=4.2 if arm == "CARD" else 3.6,
                            color=col, ecolor=col, elinewidth=0.9, capsize=0,
                            mec=col, mew=0.6,
                            zorder=4 if arm == "CARD" else 3, label=lab)
        ax.axhline(0, color=INK, lw=0.8, ls=(0, (4, 2)), zorder=1)
        ax.set_xticks(range(len(conds)))
        ax.set_xticklabels([label(tag, c) for c in conds], rotation=30 if tag != "C1" else 0,
                           ha="right" if tag != "C1" else "center", fontsize=6.3)
        ax.set_title("%s (%d scenarios)" % (name, len(conds)), loc="left")
        ax.set_ylabel("Timely PDR minus\nstock AODV")
        ax.grid(axis="y", color=GRID, linewidth=0.5)
        ax.set_axisbelow(True)
    h, l = axes[0].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=6, fontsize=7, bbox_to_anchor=(0.5, -0.01),
               handletextpad=0.3, columnspacing=1.2)
    fig.tight_layout(h_pad=1.0, rect=(0, 0.035, 1, 1))
    save(fig, "F_card_scenarios", data)


def f_tradeoff(D):
    fig, axes = plt.subplots(1, 3, figsize=(WIDTH, 2.5), sharey=True)
    data = {}
    for ax, (tag, _, _) in zip(axes, FAMILY):
        name, rows, conds = D[tag]
        data[tag] = {}
        for arm, lab, col, mk in SERIES:
            pts = []
            for c in conds:
                d = paired(rows, arm, "AODV-STOCK", c)
                db = paired(rows, arm, "AODV-STOCK", c, "bytes_per_pkt")
                ref = [val(kv, "bytes_per_pkt") for (a, cc, s), kv in rows.items()
                       if a == "AODV-STOCK" and cc == c]
                if len(d) < 3 or not ref:
                    continue
                pts.append((100 * st.mean(db) / st.mean(ref), st.mean(d)))
            if not pts:
                continue
            data[tag][arm] = pts
            ax.scatter([p[0] for p in pts], [p[1] for p in pts], s=9, color=col, marker=mk,
                       alpha=0.45, linewidths=0, zorder=2)
            mx, my = st.mean(p[0] for p in pts), st.mean(p[1] for p in pts)
            ax.scatter([mx], [my], s=48 if arm == "CARD" else 36, color=col, marker=mk,
                       edgecolors="white", linewidths=0.6, zorder=4, label=lab)
        ax.scatter([0], [0], s=30, color=INK, marker="s", zorder=5, label="Stock AODV")
        ax.axhline(0, color=GRID, lw=0.6, zorder=1)
        ax.axvline(0, color=GRID, lw=0.6, zorder=1)
        ax.set_title(name, loc="left")
        ax.set_xlabel("Control bytes vs stock (%)")
    axes[0].set_ylabel("Timely PDR minus stock AODV")
    h, l = axes[0].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=7, fontsize=6.5, bbox_to_anchor=(0.5, -0.04),
               handletextpad=0.2, columnspacing=1.0)
    fig.tight_layout(w_pad=0.6, rect=(0, 0.08, 1, 1))
    save(fig, "F_card_tradeoff", data)


def main():
    D = fam_data()
    if not any(D[t][2] for t, _, _ in FAMILY):
        print("no CARD runs yet")
        return
    f_scenarios(D)
    f_tradeoff(D)


if __name__ == "__main__":
    main()
