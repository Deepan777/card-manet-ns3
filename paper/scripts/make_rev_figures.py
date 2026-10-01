"""Revision figures (outputs under paper/): Fig. 4 with readable scenario identifiers, Fig. 5 with
explicit estimands, and the scenario key table. Reads the original run records (read only).

  paper/figures/F4_scenarios_rev.pdf   per-scenario mean paired difference in timely PDR from stock AODV
                                          with 95% percentile bootstrap intervals over seeds
  paper/figures/F5_tradeoff_rev.pdf    per-scenario relative change of mean counted control bytes (x) and
                                          mean paired timely-PDR difference (y); large markers are the
                                          unweighted means over the family's scenarios (the estimands of
                                          Table 3, Delta and Delta-B_scen)
  paper/tables/tab_scenario_key.tex

Usage (from the project root): python paper/scripts/make_rev_figures.py
"""

import json
import os
import pathlib
import statistics as st
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
os.environ.setdefault("MPLCONFIGDIR", str(ROOT / "paper" / "scripts" / "out" / "mplcache"))
sys.path.insert(0, str(ROOT / "analysis"))

import matplotlib  # noqa: E402
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from q1_common import boot, conditions, load, load_card, paired, val  # noqa: E402

OUT = ROOT / "paper" / "figures"
TAB = ROOT / "paper" / "tables"
FAMILY = [("C1", "c1_outage", "Bottleneck", "B"), ("C4", "c4_scaling", "Scaling", "S"),
          ("S8", "stage8_campaign", "Mobility", "M")]
# Okabe-Ito colours plus distinct marker shapes and fills, legible in greyscale.
SERIES = [("RREP-RESET", "Route-acquisition renewal", "#E69F00", "s", "none"),
          ("TAAODV", "TAAODV", "#999999", "v", "full"),
          ("CLAF-AODV", "CLAF-AODV", "#009E73", "^", "full"),
          ("CARD", "CARD", "#0072B2", "o", "full"),
          ("CARD-CLAF", "CARD-CLAF (composition)", "#CC79A7", "D", "none")]
BOTTLENECK_ORDER = [("healthy", "0", "healthy"), ("single", "0", "one outage"),
                    ("repeated", "0", "repeated outages"), ("long", "0", "long outage"),
                    ("cold", "0", "cold start"), ("cold", "1", "staggered cold start")]
plt.rcParams.update({"pdf.fonttype": 42, "font.size": 8, "axes.titlesize": 8.5, "axes.labelsize": 8,
                     "xtick.labelsize": 7.5, "ytick.labelsize": 7.5, "axes.spines.top": False,
                     "axes.spines.right": False, "legend.frameon": False, "font.family": "DejaVu Sans"})


def rows_for(camp):
    r = load(camp)
    r.update(load_card(camp))
    return r


def ordered(tag, rows):
    conds = [c for c in conditions(rows) if any(a == "CARD" and cc == c for (a, cc, _) in rows)]
    if tag == "C1":
        return [c for f, s, _ in BOTTLENECK_ORDER for c in conds if c[0] == f and c[1] == s]
    return conds


def key_rows(tag, conds, prefix):
    out = []
    for i, c in enumerate(conds, 1):
        if tag == "C1":
            desc = next(d for f, s, d in BOTTLENECK_ORDER if c[0] == f and c[1] == s)
            out.append(("%s%d" % (prefix, i), desc))
        elif tag == "C4":
            n, a, fl, p = c
            out.append(("%s%d" % (prefix, i), "%s nodes, %d\\,m square, %s flows, %s\\,B" % (n, round(float(a)), fl, p)))
        else:
            v, r, fl = c
            out.append(("%s%d" % (prefix, i), "%g\\,m/s, %g\\,pkt/s, %s flows" % (float(v), float(r), fl)))
    return out


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    data = {}
    keys = []
    fig, axes = plt.subplots(3, 1, figsize=(6.3, 6.6))
    for ax, (tag, camp, name, prefix) in zip(axes, FAMILY):
        rows = rows_for(camp)
        conds = ordered(tag, rows)
        keys.append((name, key_rows(tag, conds, prefix)))
        data[tag] = {"scenarios": [list(c) for c in conds], "series": {}}
        k = len(SERIES)
        for j, (arm, lab, col, mk, fill) in enumerate(SERIES):
            xs, ys, lo, hi = [], [], [], []
            for i, c in enumerate(conds):
                d = paired(rows, arm, "AODV-STOCK", c)
                if len(d) < 3:
                    continue
                l, h = boot(d)
                m = st.mean(d)
                xs.append(i + (j - (k - 1) / 2) * 0.15)
                ys.append(m)
                lo.append(m - l)
                hi.append(h - m)
                data[tag]["series"].setdefault(arm, []).append({"scenario": "%s%d" % (prefix, i + 1),
                                                                "mean": m, "lo": l, "hi": h, "n": len(d)})
            ax.errorbar(xs, ys, yerr=[lo, hi], fmt=mk, ms=4.0, color=col, ecolor=col, elinewidth=0.9,
                        mfc=(col if fill == "full" else "white"), mec=col, mew=0.9, capsize=0, label=lab,
                        zorder=4 if arm == "CARD" else 3)
        ax.axhline(0, color="black", lw=0.8, ls=(0, (4, 2)), zorder=1)
        ax.set_xticks(range(len(conds)))
        ax.set_xticklabels(["%s%d" % (prefix, i + 1) for i in range(len(conds))])
        ax.set_title("%s (%d scenarios, %s seeds each)" % (name, len(conds), "30" if tag == "S8" else "10"),
                     loc="left")
        ax.set_ylabel("Timely PDR difference\nvs stock AODV")
        ax.grid(axis="y", color="#dddddd", lw=0.5)
        ax.set_axisbelow(True)
    h, l = axes[0].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=5, fontsize=7, bbox_to_anchor=(0.5, -0.005),
               handletextpad=0.3, columnspacing=1.0)
    fig.tight_layout(h_pad=1.1, rect=(0, 0.035, 1, 1))
    fig.savefig(OUT / "F4_scenarios_rev.pdf", bbox_inches="tight")
    plt.close(fig)
    (OUT / "F4_scenarios_rev.json").write_text(json.dumps(data, indent=1))

    # Fig. 5: delivery against counted control bytes.
    fig, axes = plt.subplots(1, 3, figsize=(6.3, 2.6), sharey=True)
    d5 = {}
    for ax, (tag, camp, name, prefix) in zip(axes, FAMILY):
        rows = rows_for(camp)
        conds = ordered(tag, rows)
        d5[tag] = {}
        for arm, lab, col, mk, fill in SERIES:
            pts = []
            for c in conds:
                d = paired(rows, arm, "AODV-STOCK", c)
                db = paired(rows, arm, "AODV-STOCK", c, "bytes_per_pkt")
                ref = [val(kv, "bytes_per_pkt") for (a, cc, s), kv in rows.items() if a == "AODV-STOCK" and cc == c]
                if len(d) < 3:
                    continue
                pts.append((100 * st.mean(db) / st.mean(ref), st.mean(d)))
            d5[tag][arm] = pts
            ax.scatter([p[0] for p in pts], [p[1] for p in pts], s=10, marker=mk, color=col, alpha=0.4, linewidths=0)
            mx, my = st.mean(p[0] for p in pts), st.mean(p[1] for p in pts)
            ax.scatter([mx], [my], s=42, marker=mk, facecolors=(col if fill == "full" else "white"),
                       edgecolors=col, linewidths=1.0, zorder=4, label=lab)
        ax.scatter([0], [0], s=30, marker="s", color="black", zorder=5, label="Stock AODV")
        ax.axhline(0, color="#dddddd", lw=0.6, zorder=1)
        ax.axvline(0, color="#dddddd", lw=0.6, zorder=1)
        ax.set_title(name, loc="left")
        ax.set_xlabel("Counted control bytes,\nchange vs stock AODV (%)")
    axes[0].set_ylabel("Timely PDR difference\nvs stock AODV")
    h, l = axes[0].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=6, fontsize=6.8, bbox_to_anchor=(0.5, -0.07),
               handletextpad=0.2, columnspacing=0.9)
    fig.tight_layout(w_pad=0.6, rect=(0, 0.1, 1, 1))
    fig.savefig(OUT / "F5_tradeoff_rev.pdf", bbox_inches="tight")
    plt.close(fig)
    (OUT / "F5_tradeoff_rev.json").write_text(json.dumps(d5, indent=1))

    # Scenario key table (three columns of families).
    B = chr(92)
    lines = ["%% generated by paper/scripts/make_rev_figures.py -- do not edit",
             B + "begin{tabular}{@{}ll@{}}", B + "toprule", "ID & Configuration " + B + B, B + "midrule"]
    for name, rows_ in keys:
        lines.append(B + "multicolumn{2}{@{}l}{" + B + "emph{" + name + "}} " + B + B)
        for sid, desc in rows_:
            lines.append("%s & %s %s%s" % (sid, desc, B, B))
    lines += [B + "bottomrule", B + "end{tabular}"]
    (TAB / "tab_scenario_key.tex").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("wrote F4_scenarios_rev, F5_tradeoff_rev, tab_scenario_key.tex")


if __name__ == "__main__":
    main()
