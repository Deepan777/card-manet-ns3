"""Generate the manuscript's data figures (F2-F5) as vector PDFs.

Every plotted value is computed here from the run records via q1_common and
written beside the figure as a JSON sidecar. Figure 1 is a TikZ schematic in
manuscript/fig_concept.tex, not a data figure.

  F2  every condition: timely PDR of each renewal policy relative to stock AODV
  F3  delivery vs channel control cost, one facet per scenario family (+ SCR-2)
  F3_mechanism  evidence-lag ECDF and pooled interventions on SCR (main text)
  F4  scaling family, fixed-area density panel, with paired 95% CIs
  F5  forest plot: each ablation vs SCR, per condition, with 95% CIs

Palette: validated with the dataviz validator (light mode) -- F4 uses slots
1-4 (#2a78d6 #eb6834 #1baf7a #eda100): all hard gates pass; aqua and yellow are
below 3:1 contrast, so F4 carries direct labels and distinct markers.

Run:  python analysis/make_q1_figures.py
"""

import json
import statistics as st

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from q1_common import (ABLATIONS, POLICIES, REF, ROOT, boot, conditions, load,  # noqa: E402
                       mean_of, paired, val)

FIG = ROOT / "figures" / "q1"
INK, MUTED, GRID = "#0b0b0b", "#52514e", "#d9d8d4"
SLOT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
ACCENT = SLOT[0]
# Conceptual display names; implementation labels stay as data keys (Table 2 maps them).
SHORT = {"RREP-RESET": "Route-\nlevel", "TIME-BUCKET": "Clock", "SCR": "Service-\nlevel",
         "PERSISTENT-BACKOFF": "Service +\nbackoff"}
LEG = {"RREP-RESET": "Route-level", "TIME-BUCKET": "Clock", "SCR": "Service-level",
       "PERSISTENT-BACKOFF": "Service + backoff"}
ABL_LABEL = {"NO-PROBE": "no escape path", "ONE-BLOCK": "one block (K=1)",
             "NO-DEADLINE": "deadline ignored", "NO-NODE-CAP": "no node cap",
             "RESET-ESCALATION": "reach reset on acq."}
WIDTH = 6.3
MARKERS = ["o", "s", "^", "D"]
# Display names of the scenario families (campaign ids c1, c4, stage8 in the records).
FAMILY = {"C1": "Bottleneck", "C4": "Scaling", "S8": "Mobility"}

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


def f2(camps):
    fig, axes = plt.subplots(1, 3, figsize=(WIDTH, 2.6), sharey=True)
    data = {}
    for ax, tag in zip(axes, ("C1", "C4", "S8")):
        title = FAMILY[tag]
        rows = camps[tag]
        conds = conditions(rows)
        full, lines = 0, []
        for c in conds:
            ys = [st.mean(paired(rows, p, REF, c)) for p in POLICIES]
            ok = all(ys[i] > ys[i + 1] for i in range(3))
            full += ok
            lines.append({"condition": list(c), "diff_vs_stock": ys, "full_order": ok})
        for ln in sorted(lines, key=lambda l: l["full_order"], reverse=True):
            ok = ln["full_order"]
            ax.plot(range(4), ln["diff_vs_stock"], color=MUTED if ok else ACCENT,
                    lw=0.8 if ok else 1.6, alpha=0.55 if ok else 1.0,
                    marker="o", ms=2.5 if ok else 3.5, zorder=2 if ok else 3)
        ax.axhline(0, color=INK, lw=0.8, ls=(0, (4, 2)), zorder=1)
        ax.set_xticks(range(4))
        ax.set_xticklabels([SHORT[p] for p in POLICIES], fontsize=6.8)
        ax.set_title("%s (%d conditions)" % (title, len(conds)), loc="left")
        ax.text(0.98, 0.97, "full order %d/%d" % (full, len(conds)), transform=ax.transAxes,
                ha="right", va="top", fontsize=7, color=INK)
        grid(ax)
        data[tag] = lines
    axes[0].set_ylabel("Timely PDR minus stock AODV")
    axes[0].text(3.0, 0.012, "stock AODV", fontsize=6.8, color=INK, ha="right", va="bottom")
    h1, = axes[1].plot([], [], color=MUTED, lw=0.8, marker="o", ms=2.5)
    h2, = axes[1].plot([], [], color=ACCENT, lw=1.6, marker="o", ms=3.5)
    fig.legend([h1, h2], ["one condition; full four-policy order holds",
                          "one condition; clock $\\geq$ route-level"],
               loc="lower center", ncol=2, fontsize=6.8, handlelength=1.8,
               bbox_to_anchor=(0.5, -0.02))
    fig.tight_layout(w_pad=0.8, rect=(0, 0.07, 1, 1))
    save(fig, "F2_per_condition", data)


def f3(camps, scr2):
    """Delivery against control cost. Colours and markers match F4; a shared legend
    replaces point labels, which collided where campaign means sit close together."""
    fig, axes = plt.subplots(1, 3, figsize=(WIDTH, 2.55), sharey=True)
    data = {}
    for ax, tag in zip(axes, ("C1", "C4", "S8")):
        rows = camps[tag]
        pts = {}
        arms = POLICIES + (["SCR2"] if tag == "C1" else [])
        src = {**rows, **scr2} if tag == "C1" else rows
        for p in arms:
            dt, db = [], []
            for c in conditions(rows):
                d = paired(src, p, REF, c)
                if not d:
                    continue
                dt.append(st.mean(d))
                sb = mean_of(src, REF, c, "bytes_per_pkt")
                db.append(100 * (mean_of(src, p, c, "bytes_per_pkt") / sb - 1))
            t = [val(kv, "timely") for (a, c, s), kv in src.items() if a == p]
            b = [val(kv, "bytes_per_pkt") for (a, c, s), kv in src.items() if a == p]
            st_t = st.mean(val(kv, "timely") for (a, c, s), kv in src.items() if a == REF)
            st_b = st.mean(val(kv, "bytes_per_pkt") for (a, c, s), kv in src.items() if a == REF)
            x, y = 100 * (st.mean(b) / st_b - 1), st.mean(t) - st_t
            pts[p] = {"bytes_rel_pct": x, "timely_diff": y, "per_condition": list(zip(db, dt))}
            if p == "SCR2":
                ax.scatter([x], [y], s=30, facecolor="white", edgecolor=INK, linewidths=1.0, zorder=5)
                continue
            i = POLICIES.index(p)
            ax.scatter(db, dt, s=7, color=SLOT[i], alpha=0.35, linewidths=0, zorder=2)
            ax.scatter([x], [y], s=34, color=SLOT[i], marker=MARKERS[i], edgecolor=INK,
                       linewidths=0.6, zorder=4)
        ax.axhline(0, color=MUTED, lw=0.6)
        ax.axvline(0, color=MUTED, lw=0.6)
        ax.scatter([0], [0], s=28, marker="s", color=INK, zorder=5)
        ax.set_title(FAMILY[tag], loc="left")
        grid(ax, "both")
        data[tag] = pts
    axes[0].set_ylabel("Timely PDR minus stock AODV")
    hs = [axes[0].scatter([], [], s=34, color=SLOT[i], marker=MARKERS[i], edgecolor=INK, linewidths=0.6)
          for i in range(4)]
    hs.append(axes[0].scatter([], [], s=30, facecolor="white", edgecolor=INK, linewidths=1.0))
    hs.append(axes[0].scatter([], [], s=28, marker="s", color=INK))
    fig.legend(hs, [LEG[p] for p in POLICIES] + ["service-level, no gate", "stock AODV"],
               loc="lower center", ncol=6, fontsize=6.8, handletextpad=0.3, columnspacing=1.1,
               bbox_to_anchor=(0.5, -0.03))
    fig.supxlabel("Control bytes per packet relative to stock AODV (%)", fontsize=8, y=0.075)
    fig.tight_layout(w_pad=0.8, rect=(0, 0.1, 1, 1))
    save(fig, "F3_tradeoff", data)


def f4(c4):
    panel = [c for c in conditions(c4) if c[1] == "700" and c[2] == "8" and c[3] == "512"]
    fig, ax = plt.subplots(figsize=(WIDTH * 0.62, 2.6))
    markers = MARKERS
    data = {}
    for i, p in enumerate(POLICIES):
        xs, ys, lo, hi = [], [], [], []
        for c in panel:
            d = paired(c4, p, REF, c)
            a, b = boot(d)
            xs.append(float(c[0]) + (i - 1.5) * 2.2)
            ys.append(st.mean(d)); lo.append(st.mean(d) - a); hi.append(b - st.mean(d))
        ax.errorbar(xs, ys, yerr=[lo, hi], color=SLOT[i], marker=markers[i], ms=4, lw=1.4,
                    elinewidth=0.8, capsize=2, zorder=3)
        ax.annotate(LEG[p], (xs[-1], ys[-1]), xytext=(6, 0),
                    textcoords="offset points", va="center", fontsize=6.8, color=INK)
        data[p] = {"nodes": [int(float(c[0])) for c in panel], "diff": ys,
                   "ci": [[y - l, y + h] for y, l, h in zip(ys, lo, hi)]}
    ax.axhline(0, color=INK, lw=0.8, ls=(0, (4, 2)))
    ax.set_xticks([30, 60, 90, 120])
    ax.set_xlim(20, 150)
    ax.set_xlabel("Nodes in a fixed 700 m $\\times$ 700 m area")
    ax.set_ylabel("Timely PDR minus stock AODV")
    grid(ax)
    fig.tight_layout()
    save(fig, "F4_density", data)


def f5(abl_rows, s8):
    conds = []
    for c in conditions(abl_rows):
        if all(len(paired(abl_rows, x, "SCR", c)) >= 30 for x in ABLATIONS):
            conds.append(c)
    rows, data = [], {}
    for x in ABLATIONS:
        for c in conds:
            d = paired(abl_rows, x, "SCR", c)
            a, b = boot(d)
            rows.append((x, c, st.mean(d), a, b))
            data.setdefault(x, []).append({"condition": list(c), "mean": st.mean(d), "ci": [a, b]})
    h = 0.7 + 0.13 * len(rows) + 0.2 * len(ABLATIONS)
    fig, ax = plt.subplots(figsize=(WIDTH * 0.72, h))
    y, ticks, labels, heads = 0.0, [], [], []
    for x in ABLATIONS:
        heads.append((y, x))
        y -= 0.7
        for (xx, c, m, a, b) in [r for r in rows if r[0] == x]:
            col = ACCENT if x == "NO-PROBE" else INK
            ax.plot([a, b], [y, y], color=col, lw=1.1, solid_capstyle="round", zorder=3)
            ax.scatter([m], [y], s=16, color=col, zorder=4)
            ticks.append(y)
            labels.append("%s m/s, %s pkt/s, %s flows" % c)
            y -= 1.0
        y -= 0.4
    # Axis limits from the data, so no interval can fall outside the plot.
    xlo = min(min(r[3] for r in rows), 0.0) - 0.02
    xhi = max(max(r[4] for r in rows), 0.0) + 0.02
    for yy, x in heads:
        ax.text(xlo + 0.005, yy, ABL_LABEL[x], fontsize=7.4, fontweight="bold", color=INK, va="center")
    ax.axvline(0, color=INK, lw=0.8, ls=(0, (4, 2)))
    ax.set_yticks(ticks)
    ax.set_yticklabels(labels, fontsize=6.6)
    ax.set_xlim(xlo, xhi)
    ax.set_ylim(y + 0.6, 0.6)
    ax.set_xlabel("Timely PDR: ablation minus SCR (paired, 95% CI)")
    ax.spines["left"].set_visible(False)
    ax.tick_params(axis="y", length=0)
    grid(ax, "x")
    fig.tight_layout()
    save(fig, "F5_ablations_forest", {"conditions": [list(c) for c in conds], "ablations": data})


def f3m():
    """Mechanism figure. (a) How soon a confirmation could exist after service resumes,
    under stock AODV, by family (evidence_lag.py). (b) Every intervention on SCR on one
    axis, grouped by what it changes, with SCR's own deficit for scale (make_q1_tables.py)."""
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
    ticks.append(y); labels.append("for scale: service-level minus stock"); heads.append(len(labels) - 1)
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
    ax2.set_title("(b) Altering service-level renewal", loc="left")
    grid(ax2, "x")
    fig.tight_layout(w_pad=1.2)
    save(fig, "F3_mechanism", {"evidence_lag": data_a, "interventions": data_b,
                                "scr_minus_stock": iv["scr_minus_stock"]})


def main():
    camps = {"C1": load("c1_outage"), "C4": load("c4_scaling"), "S8": load("stage8_campaign")}
    f2(camps)
    f3(camps, load("c1_scr2_supplement"))
    f4(camps["C4"])
    f5({**camps["S8"], **load("stage9_ablation_sweep")}, camps["S8"])
    f3m()


if __name__ == "__main__":
    main()
