"""R053: paired comparison of CARD arms against references, per cell.

Reads out_summary.csv from one or more campaign directories, keys every run by
(family, cell, seed, algorithm), and reports per cell the mean timely PDR,
control bytes per generated packet, and late packets, plus the paired mean
difference of each arm against each reference with a percentile bootstrap
interval (10,000 resamples, seed 20260905, as in the published analysis).

Usage:
  python analysis/card_compare.py --campaigns card_pilot
  python analysis/card_compare.py --campaigns card_campaign c1_outage c4_scaling stage8_campaign
Writes analysis_out/card_compare_<tag>.json.
"""

import argparse
import collections
import csv
import json
import pathlib
import random
import statistics as st

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARMS = ["CARD", "CARD-NO-CAUSE", "CARD-NO-REACH", "CARD-EXEMPT-ALL", "CARD-NO-EXEMPT"]
REFS = ["AODV-STOCK", "RREP-RESET", "TIME-BUCKET", "SCR", "AODV-FB"]


def summary(p):
    with open(p, newline="") as f:
        return {r[0]: r[1] for r in csv.reader(f) if len(r) >= 2}


def cell_of(done, s):
    exe = done["argv"][0]
    args = {a.split("=")[0].lstrip("-"): a.split("=", 1)[1] for a in done["argv"][1:] if "=" in a}
    if "scr-c1-outage" in exe:
        label = args["fault"] + ("_staggered" if args.get("coldStagger") == "1" else "")
        return "bottleneck", label
    if "numNodes" in args:
        return "scaling", "N%s_A%.0f_F%s_R%s_P%s" % (args["numNodes"], float(args["areaSide"]),
                                                   args["numFlows"], args["ratePps"],
                                                   args["payloadBytes"])
    return "mobility", "v%s_R%s_F%s" % (args["speed"], args["ratePps"], args["numFlows"])


def load(campaigns):
    runs = {}
    for c in campaigns:
        for d in (ROOT / "checkpoints" / c).glob("*"):
            if not (d / "DONE.json").exists() or not (d / "out_summary.csv").exists():
                continue
            done = json.loads((d / "DONE.json").read_text())
            if done.get("returncode", 1) != 0:
                continue
            s = summary(d / "out_summary.csv")
            ep = s.get("effective_parameters", "")
            algo = s.get("algorithm")
            # Provenance gate: SCR-family runs must come from rev 5 (pre-CARD
            # modes, identity-checked against rev 6) or rev 6 (CARD).
            # Provenance (R053/R054): pre-CARD modes from rev 5-7 (identity-
            # checked); CARD and CARD-NO-EXEMPT changed at rev 7 and count only
            # from rev 7; the other CARD variants are rev 6 or 7 (identical).
            if "fork_rev=" in ep and not any("fork_rev=%d" % r in ep for r in (5, 6, 7)):
                continue
            if algo in ("CARD", "CARD-NO-EXEMPT") and "fork_rev=7" not in ep:
                continue
            if algo in ARMS and not ("fork_rev=6" in ep or "fork_rev=7" in ep):
                continue
            fam, cell = cell_of(done, s)
            gen = float(s.get("generated", 0) or 0)
            runs[(fam, cell, int(s["rng_run"]), algo)] = {
                "tpdr": float(s["timely_pdr"]),
                "bpp": float(s.get("ctrl_bytes_sent", 0) or 0) / gen if gen else float("nan"),
                "late": float(s.get("late", 0) or 0),
                "rreq": float(s.get("source_rreq_originated", 0) or 0),
                "exempt": float(s.get("card_exempt_admitted", 0) or 0),
            }
    return runs


def boot(diffs, n=10000, seed=20260905):
    rng = random.Random(seed)
    k = len(diffs)
    means = sorted(sum(rng.choice(diffs) for _ in range(k)) / k for _ in range(n))
    return means[int(0.025 * n)], means[int(0.975 * n) - 1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--campaigns", nargs="+", required=True)
    ap.add_argument("--tag", default=None)
    ap.add_argument("--boot", type=int, default=10000)
    a = ap.parse_args()
    runs = load(a.campaigns)
    cells = sorted({(f, c) for f, c, _, _ in runs})
    out = {}
    for fam, cell in cells:
        algos = sorted({al for f, c, _, al in runs if (f, c) == (fam, cell)})
        rec = {"means": {}, "paired": {}}
        for al in algos:
            v = [r for (f, c, s, x), r in runs.items() if (f, c, x) == (fam, cell, al)]
            rec["means"][al] = {"n": len(v), **{m: st.mean(r[m] for r in v)
                                                for m in ("tpdr", "bpp", "late", "rreq", "exempt")}}
        for arm in [x for x in ARMS if x in algos]:
            for ref in [x for x in REFS + ARMS if x in algos and x != arm]:
                seeds = sorted({s for f, c, s, x in runs if (f, c, x) == (fam, cell, arm)} &
                               {s for f, c, s, x in runs if (f, c, x) == (fam, cell, ref)})
                if len(seeds) < 3:
                    continue
                res = {"n": len(seeds)}
                for m in ("tpdr", "bpp"):
                    d = [runs[(fam, cell, s, arm)][m] - runs[(fam, cell, s, ref)][m] for s in seeds]
                    lo, hi = boot(d, a.boot)
                    res[m] = {"mean": st.mean(d), "lo": lo, "hi": hi,
                              "wins": sum(x > 0 for x in d), "losses": sum(x < 0 for x in d)}
                rec["paired"]["%s-vs-%s" % (arm, ref)] = res
        out["%s/%s" % (fam, cell)] = rec
        line = "  ".join("%s %.3f/%.0f" % (al, rec["means"][al]["tpdr"], rec["means"][al]["bpp"])
                         for al in algos)
        print("%-12s %-28s %s" % (fam, cell, line))
        for k, r in rec["paired"].items():
            if k.endswith(("AODV-STOCK", "RREP-RESET")) and k.startswith("CARD-vs") or \
               k in ("CARD-vs-CARD-NO-CAUSE", "CARD-vs-CARD-EXEMPT-ALL"):
                print("    %-26s n=%2d dTPDR %+.4f [%+.4f,%+.4f] w/l %d/%d   dB/pkt %+.1f [%+.1f,%+.1f]" % (
                    k, r["n"], r["tpdr"]["mean"], r["tpdr"]["lo"], r["tpdr"]["hi"],
                    r["tpdr"]["wins"], r["tpdr"]["losses"],
                    r["bpp"]["mean"], r["bpp"]["lo"], r["bpp"]["hi"]))
    tag = a.tag or "_".join(a.campaigns)
    (ROOT / "analysis_out").mkdir(exist_ok=True)
    (ROOT / "analysis_out" / ("card_compare_%s.json" % tag)).write_text(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    main()
