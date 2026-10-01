#!/usr/bin/env python3
"""
Paper figures from the completed C1 and C4 campaigns (fork_rev 5).

Every figure is generated from the run summaries on disk, never from numbers
typed into this file, and each is written with a sidecar JSON recording the
values behind it. A figure that cannot be traced back to its runs is a figure
that cannot be defended in review.

Pre-R032 runs are rejected outright rather than plotted.
"""

import collections
import json
import pathlib
import re
import statistics as st

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "figures"
OUT.mkdir(exist_ok=True)

REF = "AODV-STOCK"

# Ordered by how hard the renewal evidence is to obtain. This ordering is the
# paper's central axis, so it is declared once and reused by every figure.
LADDER = ["RREP-RESET", "TIME-BUCKET", "SCR", "PERSISTENT-BACKOFF"]
LADDER_LABEL = ["renew on\nroute acquired", "renew on\nelapsed time",
                "renew on confirmed\non-time delivery", "never\nrenew"]
COND_FIELDS = ("fault", "cold_stagger", "num_nodes", "area_side_m", "speed_mps",
               "num_flows", "rate_pps", "payload_bytes")


def load(campaign):
    """Load the campaign's own manifest runs. Stale directories are ignored."""
    base = ROOT / "checkpoints" / campaign
    man = {r["run_id"] for r in
           json.loads((ROOT / "configs" / (campaign + ".json")).read_text())["runs"]}
    runs = []
    for d in sorted(base.iterdir()):
        if not d.is_dir() or d.name not in man or not (d / "DONE.json").exists():
            continue
        s = d / "out_summary.csv"
        if not s.exists():
            continue
        kv = {}
        for line in s.read_text(errors="replace").splitlines():
            if "," in line:
                k, _, v = line.partition(",")
                kv[k.strip()] = v.strip().strip('"')
        params = kv.get("effective_parameters", "")
        if "scr_enabled=1" in params:
            m = re.search(r"fork_rev=(\d+)", params)
            assert m and int(m.group(1)) >= 5, "pre-R032 run in %s: %s" % (campaign, d.name)
        runs.append(kv)
    return runs


def timely(kv):
    g = float(kv["generated"])
    return float(kv["ontime_true"]) / g if g else 0.0


def ctrl(kv):
    g = float(kv["generated"])
    return float(kv.get("ctrl_bytes_sent", 0) or 0) / g if g else 0.0


def paired(runs, value):
    """Mean paired difference vs the reference, per algorithm, over conditions."""
    cells = collections.defaultdict(dict)
    for kv in runs:
        cond = tuple((f, kv[f]) for f in COND_FIELDS if f in kv)
        cells[cond].setdefault(kv["algorithm"], {})[kv["rng_run"]] = value(kv)
    out = collections.defaultdict(list)
    used = 0
    for by_algo in cells.values():
        if REF not in by_algo:
            continue
        used += 1
        ref = by_algo[REF]
        for algo, seeds in by_algo.items():
            common = sorted(set(seeds) & set(ref))
            if common:
                out[algo].append(st.mean(seeds[s] - ref[s] for s in common))
    return out, used


def sidecar(name, meta):
    (OUT / (name + ".sidecar.json")).write_text(json.dumps(meta, indent=2), encoding="utf-8")


def fig_ladder(data):
    fig, ax = plt.subplots(figsize=(7.4, 4.1))
    width = 0.36
    xs = list(range(len(LADDER)))
    for i, (camp, label, colour) in enumerate(
            [("c1_outage", "C1: induced link outage", "#2b6cb0"),
             ("c4_scaling", "C4: mobile, scale and density", "#c05621")]):
        d = data[camp]["timely"]
        vals = [st.mean(d[a]) if d.get(a) else 0.0 for a in LADDER]
        ax.bar([x + (i - 0.5) * width for x in xs], vals, width, label=label, color=colour)
    ax.axhline(0, color="black", lw=1)
    ax.set_xticks(xs)
    ax.set_xticklabels(LADDER_LABEL, fontsize=8)
    ax.set_ylabel("timely PDR, mean paired\ndifference vs stock AODV")
    ax.set_title("Delivery falls monotonically as the renewal evidence gets harder to obtain",
                 fontsize=10)
    ax.legend(fontsize=8, loc="lower left")
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(OUT / "F2_evidence_ladder.svg")
    fig.savefig(str(OUT / "F2_evidence_ladder.svg").replace(".svg", ".pdf"))
    plt.close(fig)
    sidecar("F2_evidence_ladder", {
        "figure": "evidence ladder, both campaigns",
        "reference": REF,
        "arms": LADDER,
        "c1_per_condition": {a: data["c1_outage"]["timely"].get(a, []) for a in LADDER},
        "c4_per_condition": {a: data["c4_scaling"]["timely"].get(a, []) for a in LADDER},
        "note": "each listed value is one condition's mean paired difference; "
                "bars are the mean over conditions",
    })


def fig_tradeoff(data):
    fig, axes = plt.subplots(1, 2, figsize=(9.6, 4.3))
    for ax, camp, title in [(axes[0], "c1_outage", "C1: induced link outage"),
                            (axes[1], "c4_scaling", "C4: mobile, scale and density")]:
        td, cd = data[camp]["timely"], data[camp]["ctrl"]
        for a in LADDER + ["AODV-FB", "OLSR", "DSDV"]:
            if not td.get(a) or not cd.get(a):
                continue
            x, y = st.mean(cd[a]), st.mean(td[a])
            ax.scatter(x, y, s=44, color="#c05621" if a in LADDER else "#718096")
            ax.annotate(a, (x, y), fontsize=7, xytext=(4, 4), textcoords="offset points")
        ax.scatter(0, 0, marker="*", s=170, color="black")
        ax.annotate("stock AODV", (0, 0), fontsize=7, xytext=(4, -11),
                    textcoords="offset points")
        ax.axhline(0, color="black", lw=0.8)
        ax.axvline(0, color="black", lw=0.8)
        ax.set_xlabel("control bytes per packet,\ndifference vs stock")
        ax.set_title(title, fontsize=10)
        ax.grid(alpha=0.3)
    axes[0].set_ylabel("timely PDR, difference vs stock")
    fig.suptitle("Upper left is better: more on-time delivery for less control traffic",
                 fontsize=10)
    fig.tight_layout()
    fig.savefig(OUT / "F3_delivery_vs_overhead.svg")
    fig.savefig(str(OUT / "F3_delivery_vs_overhead.svg").replace(".svg", ".pdf"))
    plt.close(fig)
    sidecar("F3_delivery_vs_overhead", {
        "figure": "delivery against control cost, both campaigns",
        "reference": REF,
        "c1": {a: [st.mean(data["c1_outage"]["ctrl"][a]),
                   st.mean(data["c1_outage"]["timely"][a])]
               for a in data["c1_outage"]["timely"] if data["c1_outage"]["ctrl"].get(a)},
        "c4": {a: [st.mean(data["c4_scaling"]["ctrl"][a]),
                   st.mean(data["c4_scaling"]["timely"][a])]
               for a in data["c4_scaling"]["timely"] if data["c4_scaling"]["ctrl"].get(a)},
    })


def fig_density(runs_c4):
    """The scope condition: the advantage erodes as density rises."""
    by_n = collections.defaultdict(lambda: collections.defaultdict(dict))
    for kv in runs_c4:
        if kv.get("area_side_m") != "700":       # density panel holds area fixed
            continue
        by_n[int(kv["num_nodes"])].setdefault(kv["algorithm"], {})[kv["rng_run"]] = timely(kv)
    ns = sorted(by_n)
    fig, ax = plt.subplots(figsize=(6.8, 4.1))
    series = {}
    for a, colour in [("RREP-RESET", "#2b6cb0"), ("TIME-BUCKET", "#38a169"),
                      ("SCR", "#c05621"), ("PERSISTENT-BACKOFF", "#805ad5")]:
        ys = []
        for n in ns:
            ref, arm = by_n[n].get(REF, {}), by_n[n].get(a, {})
            common = sorted(set(ref) & set(arm))
            ys.append(st.mean(arm[s] - ref[s] for s in common) if common else float("nan"))
        series[a] = ys
        ax.plot(ns, ys, marker="o", label=a, color=colour)
    ax.axhline(0, color="black", lw=1)
    ax.set_xlabel("nodes in a fixed 700 m area (rising density)")
    ax.set_ylabel("timely PDR, difference vs stock AODV")
    ax.set_title("Rationing discovery stops paying as density rises", fontsize=10)
    ax.set_xticks(ns)
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(OUT / "F4_density_scope.svg")
    fig.savefig(str(OUT / "F4_density_scope.svg").replace(".svg", ".pdf"))
    plt.close(fig)
    sidecar("F4_density_scope", {"figure": "density panel, area fixed at 700 m",
                                 "nodes": ns, "series": series, "reference": REF})


def main():
    data, runs = {}, {}
    for camp in ("c1_outage", "c4_scaling"):
        r = load(camp)
        runs[camp] = r
        t, n = paired(r, timely)
        c, _ = paired(r, ctrl)
        data[camp] = {"timely": t, "ctrl": c, "conditions": n}
        print("%-12s %4d runs, %2d conditions" % (camp, len(r), n))

    fig_ladder(data)
    fig_tradeoff(data)
    fig_density(runs["c4_scaling"])
    print("wrote: " + ", ".join(sorted(p.name for p in OUT.glob("F[234]*.svg"))))

    for camp in ("c1_outage", "c4_scaling"):
        print("\n%s  mean paired diff (timely / ctrl bytes per packet):" % camp)
        for a in LADDER:
            if data[camp]["timely"].get(a):
                print("   %-20s %+7.3f  %+8.2f"
                      % (a, st.mean(data[camp]["timely"][a]), st.mean(data[camp]["ctrl"][a])))


if __name__ == "__main__":
    main()
