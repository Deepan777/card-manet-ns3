"""R053: score CARD's break classifier against the true geometry of each break.

Every CARD run writes out_card_breaks.csv: one row per neighbour loss detected
by any node, with the verdict (0 transient, 1 topology change, 2 unknown), the
evidence it used (last received power, its trend over >= 1 s, age), and the
true separation and radial speed of the two nodes at that instant, logged by
the scenario from the mobility models. The protocol never sees the geometry.

Ground truth. The link cliff sits between 140 m and 142 m (stage2 fine distance
sweep, zero loss at 140 m, total loss at 142 m, no fading), so the range edge is
R = 141 m. A break is a DEPARTURE if the neighbour is beyond R at the break, or
will be within HORIZON seconds at its current radial speed. Everything else is
a loss with the neighbour still in range -- a MAC failure under contention or
an outage of a node that has not moved. HORIZON = 1 s is the hello interval:
the longest a departure can go unobserved.

Usage: python analysis/card_classifier.py <campaign> [<campaign> ...]
Writes analysis_out/card_classifier_<campaign>.json.
"""

import collections
import csv
import json
import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
RANGE_M = 141.0
HORIZON_S = 1.0


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return math.nan


def family_of(run_dir):
    done = json.loads((run_dir / "DONE.json").read_text())
    exe = done["argv"][0]
    if "scr-c1-outage" in exe:
        return "bottleneck"
    args = " ".join(done["argv"])
    return "scaling" if "--numNodes=" in args else "mobility"


def score(campaign):
    base = ROOT / "checkpoints" / campaign
    agg = collections.defaultdict(collections.Counter)
    runs = collections.Counter()
    for d in sorted(base.glob("*")):
        f = d / "out_card_breaks.csv"
        if not f.exists() or not (d / "DONE.json").exists():
            continue
        fam = family_of(d)
        runs[fam] += 1
        with open(f, newline="") as fh:
            for r in csv.DictReader(fh):
                dist, vr = fnum(r["dist_m"]), fnum(r["radial_mps"])
                if math.isnan(dist):
                    agg[fam]["no_geometry"] += 1
                    continue
                departed = dist > RANGE_M or dist + max(vr, 0.0) * HORIZON_S > RANGE_M
                v = r["verdict"]
                key = {"1": "topo", "0": "trans", "2": "unknown"}[v]
                agg[fam]["%s_%s" % (key, "departed" if departed else "inrange")] += 1
    out = {}
    for fam, c in agg.items():
        tp, fp = c["topo_departed"], c["topo_inrange"]
        fn, tn = c["trans_departed"], c["trans_inrange"]
        uk = c["unknown_departed"] + c["unknown_inrange"]
        total = tp + fp + fn + tn + uk
        out[fam] = {
            "runs": runs[fam], "breaks": total, **dict(c),
            "unknown_share": uk / total if total else None,
            "precision_topology": tp / (tp + fp) if tp + fp else None,
            "recall_topology": tp / (tp + fn) if tp + fn else None,
            "precision_transient": tn / (tn + fn) if tn + fn else None,
            "accuracy_classified": (tp + tn) / (tp + fp + fn + tn) if tp + fp + fn + tn else None,
            "departure_share": (tp + fn + c["unknown_departed"]) / total if total else None,
        }
    return out


def main():
    res = {}
    for campaign in sys.argv[1:]:
        res[campaign] = score(campaign)
        (ROOT / "analysis_out").mkdir(exist_ok=True)
        (ROOT / "analysis_out" / ("card_classifier_%s.json" % campaign)).write_text(
            json.dumps(res[campaign], indent=1))
        for fam, s in sorted(res[campaign].items()):
            print("%s / %s: runs=%d breaks=%d unknown=%.3f P_topo=%s R_topo=%s P_trans=%s acc=%s "
                  "departed=%.3f" % (
                      campaign, fam, s["runs"], s["breaks"], s["unknown_share"] or 0,
                      *("%.3f" % s[k] if s[k] is not None else "n/a"
                        for k in ("precision_topology", "recall_topology",
                                  "precision_transient", "accuracy_classified")),
                      s["departure_share"] or 0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
