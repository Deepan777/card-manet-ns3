#!/usr/bin/env python3
"""
Section 6 (ablation study) from the Stage 8 campaign.

Each ablation removes ONE component from SCR, so the comparison that answers
"what does this component do?" is ablation vs SCR, not ablation vs stock AODV.
Both are reported.

Runs safely on a PARTIAL campaign. Stage 8's manifest is interleaved across
algorithms (2026-09-19), so any prefix of it is a balanced sample; this script
reports how many paired cells each row rests on and refuses to compare a row
with too few.

Two ablations carry real weight for the paper's argument and their outcome is
not predetermined:

  NO-DEADLINE       drops the deadline requirement from the evidence rule. If
                    SCR recovers, the collapse reported in Section 5.3 is about
                    THIS deadline, not about confirmed-service gating, and that
                    claim must be weakened.
  RESET-ESCALATION  resets the reach index on route acquisition while keeping
                    the debt. Already measured on C1 healthy, where it was
                    WORSE than SCR (0.062 vs 0.077).
"""

import collections
import json
import pathlib
import statistics as st
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CAMP = "stage8_campaign"
MIN_CELLS = 2          # below this a row is reported as too thin, never averaged

ABLATIONS = {
    "NO-PROBE":         "rho = 0: no probe tokens, so an exhausted allowance is terminal",
    "ONE-BLOCK":        "K = 1: confirmation needs one good block instead of two",
    "NO-DEADLINE":      "receiver ignores the deadline when scoring a block",
    "NO-NODE-CAP":      "per-node aggregate limiter G removed",
    "RESET-ESCALATION": "reach index reset on route acquisition; debt retained",
}
COND_FIELDS = ("speed", "ratePps", "numFlows", "num_nodes", "area_side_m",
               "speed_mps", "rate_pps", "num_flows", "payload_bytes")


def load():
    base = ROOT / "checkpoints" / CAMP
    if not base.is_dir():
        sys.exit("no Stage 8 checkpoints yet")
    man = {r["run_id"] for r in
           json.loads((ROOT / "configs" / (CAMP + ".json")).read_text())["runs"]}
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
        if "scr_enabled=1" in kv.get("effective_parameters", ""):
            assert "fork_rev=5" in kv["effective_parameters"], "pre-R032 run: " + d.name
        runs.append(kv)
    return runs


def timely(kv):
    g = float(kv.get("generated", 0) or 0)
    return float(kv.get("ontime_true", 0) or 0) / g if g else 0.0


def ctrl(kv):
    g = float(kv.get("generated", 0) or 0)
    return float(kv.get("ctrl_bytes_sent", 0) or 0) / g if g else 0.0


def cells(runs, value):
    out = collections.defaultdict(dict)
    for kv in runs:
        cond = tuple((f, kv[f]) for f in COND_FIELDS if f in kv)
        out[cond].setdefault(kv["algorithm"], {})[kv.get("rng_run")] = value(kv)
    return out


def paired(table, arm, ref):
    diffs = []
    for by_algo in table.values():
        a, b = by_algo.get(arm), by_algo.get(ref)
        if not a or not b:
            continue
        common = sorted(set(a) & set(b))
        if common:
            diffs.append(st.mean(a[s] - b[s] for s in common))
    return diffs


def row(table_t, table_c, arm, ref):
    dt, dc = paired(table_t, arm, ref), paired(table_c, arm, ref)
    if len(dt) < MIN_CELLS:
        return None
    return len(dt), st.mean(dt), (st.mean(dc) if dc else float("nan"))


def figure(rows_vs_scr, n_runs):
    """Figure F5. Drawn only when several arms have enough paired cells; a bar
    chart built from one or two cells looks identical to a solid result and is
    worse than no figure at all."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    drawn = [(a, v) for a, v in rows_vs_scr.items() if v is not None]
    if len(drawn) < 3:
        print(chr(10) + "(figure F5 not drawn: only %d arm(s) have >= %d paired cells)"
              % (len(drawn), MIN_CELLS))
        return
    arms = [a for a, _ in drawn]
    vals = [v[1] for _, v in drawn]
    cells = [v[0] for _, v in drawn]
    fig, ax = plt.subplots(figsize=(7.2, 4.0))
    ax.bar(range(len(arms)), vals,
           color=["#c05621" if v < 0 else "#2b6cb0" for v in vals])
    ax.axhline(0, color="black", lw=1)
    ax.set_xticks(range(len(arms)))
    ax.set_xticklabels(arms, fontsize=7, rotation=20, ha="right")
    ax.set_ylabel("timely PDR, paired difference vs SCR")
    ax.set_title("Ablations: effect of removing one component from SCR", fontsize=10)
    for i, (v, c) in enumerate(zip(vals, cells)):
        ax.annotate("n=%d" % c, (i, v), fontsize=7, ha="center",
                    xytext=(0, 4 if v >= 0 else -12), textcoords="offset points")
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(ROOT / "figures" / "F5_ablations.svg")
    fig.savefig(str(ROOT / "figures" / "F5_ablations.svg").replace(".svg", ".pdf"))
    (ROOT / "figures" / "F5_ablations.sidecar.json").write_text(
        json.dumps({"figure": "ablations vs SCR",
                    "stage8_runs_at_generation": n_runs,
                    "arms": arms, "paired_cells": cells, "d_timely": vals}, indent=2),
        encoding="utf-8")
    plt.close(fig)
    print(chr(10) + "wrote figures/F5_ablations.svg")


def latex_table(rows_vs_scr, n_runs):
    """DISABLED 2026-09-23 (R044). This used to write manuscript/tab_ablation.tex.

    It must not. The fragment it produced averaged effect sizes ACROSS conditions
    and left the CI and sign-p columns EMPTY, because this report does not
    compute them. analysis/condition_ablation.py owns that file: it works from a
    single COMPLETE condition, with bootstrap intervals and exact sign tests.

    Both scripts writing the same path meant whichever ran last won. Running this
    one after the other silently replaced Section 6's table with a worse one whose
    headline number (-0.1468, averaged) contradicted the prose beside it (0.121,
    the complete condition) -- and shipped a table with two blank columns. That
    is exactly what happened on 2026-09-23.

    This report is still useful as a campaign-wide console summary. It simply has
    no business writing into the manuscript.
    """
    return




def main():
    runs = load()
    by_algo = collections.Counter(r["algorithm"] for r in runs)
    print("Stage 8: %d runs complete, %d algorithms present" % (len(runs), len(by_algo)))
    for a, n in sorted(by_algo.items()):
        print("   %-20s %4d" % (a, n))
    if not runs:
        return

    t_tab, c_tab = cells(runs, timely), cells(runs, ctrl)
    print("\nconditions with data: %d" % len(t_tab))

    for ref, title in (("SCR", "vs SCR  (what removing the component does)"),
                       ("AODV-STOCK", "vs stock AODV")):
        print("\n=== %s ===" % title)
        print("%-20s %6s %12s %14s" % ("arm", "cells", "d timely", "d ctrlB/pkt"))
        for arm in list(ABLATIONS) + (["SCR"] if ref == "AODV-STOCK" else []):
            r = row(t_tab, c_tab, arm, ref)
            if r is None:
                have = len(paired(t_tab, arm, ref))
                print("%-20s %6s  (too few paired cells: %d)" % (arm, "-", have))
                continue
            n, dt, dc = r
            print("%-20s %6d %+12.4f %+14.2f" % (arm, n, dt, dc))

    vs_scr = {a: row(t_tab, c_tab, a, "SCR") for a in ABLATIONS}
    figure(vs_scr, len(runs))
    latex_table(vs_scr, len(runs))

    print("\nInterpretation notes")
    for a, why in ABLATIONS.items():
        print("   %-18s %s" % (a, why))
    print("\nNO-DEADLINE is the one that can change Section 5.3: if it lifts SCR "
          "substantially,\nthe collapse is attributable to the deadline requirement rather "
          "than to gating on\nconfirmed service, and the claim must be narrowed accordingly.")


if __name__ == "__main__":
    main()
