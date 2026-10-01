"""Revision audit: recompute the CARD paper's headline estimands from the run records.

Reads the original per-run records (checkpoints/, read only) through analysis/q1_common.py
and writes, under paper/:
  tables/tab_card_main_rev.tex    Table 3 with explicit estimands (pooled means, scenario-mean
                                  paired difference, and BOTH byte aggregations)
  tables/tab_card_classifier_rev.tex  classifier scores with run-clustered bootstrap intervals
                                  (POST HOC: added during the revision)
  scripts/out/numerical_recompute.json  every recomputed value

Nothing in checkpoints/ or analysis/ is modified. The classifier step needs the per-break logs
(out_card_breaks.csv, distributed separately from this repository); without them the script stops there.
Usage (from the project root): python paper/scripts/verify_numbers.py
"""

import csv
import json
import math
import pathlib
import random
import statistics as st
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "analysis"))
from q1_common import boot, conditions, load, load_card, paired, val  # noqa: E402

REV = ROOT / "paper"
FAMILIES = [("C1", "c1_outage", "Bottleneck"), ("C4", "c4_scaling", "Scaling"),
            ("S8", "stage8_campaign", "Mobility")]
ARMS = ["AODV-STOCK", "CLAF-AODV", "TAAODV", "RREP-RESET", "CARD", "CARD-NO-CAUSE",
        "CARD-EXEMPT-ALL", "CARD-NO-EXEMPT", "CARD-NO-REACH", "CARD-CLAF"]
LABEL = {"AODV-STOCK": "Stock AODV", "CLAF-AODV": "CLAF-AODV", "TAAODV": "TAAODV",
         "RREP-RESET": "Fixed narrow start", "CARD": "CARD", "CARD-NO-CAUSE": "Fixed hop-count start",
         "CARD-EXEMPT-ALL": "Exempt-all", "CARD-NO-EXEMPT": "Radius-only",
         "CARD-NO-REACH": "Exemption-only", "CARD-CLAF": "CARD-CLAF"}
MAIN = ["AODV-STOCK", "CLAF-AODV", "TAAODV", "RREP-RESET", "CARD", "CARD-CLAF"]
RANGE_M, HORIZON_S = 141.0, 1.0
B = chr(92)


def rows_for(camp):
    r = load(camp)
    r.update(load_card(camp))
    return r


def family(tag, camp):
    rows = rows_for(camp)
    conds = [c for c in conditions(rows) if any(a == "CARD" and cc == c for (a, cc, _) in rows)]
    out = {"conditions": len(conds), "arms": {}}
    for arm in ARMS:
        per_cond_n = {c: sum(1 for (a, cc, _) in rows if a == arm and cc == c) for c in conds}
        if not any(per_cond_n.values()):
            continue
        t = [val(kv, "timely") for (a, c, _), kv in rows.items() if a == arm and c in conds]
        b = [val(kv, "bytes_per_pkt") for (a, c, _), kv in rows.items() if a == arm and c in conds]
        A = {"runs": len(t), "seeds_per_scenario": sorted(set(per_cond_n.values())),
             "pooled_timely": st.mean(t), "pooled_bytes_per_pkt": st.mean(b)}
        if arm != "AODV-STOCK":
            d_means, rel_means, ci = [], [], []
            stock_b, arm_b = [], []
            paired_complete = True
            for c in conds:
                d = paired(rows, arm, "AODV-STOCK", c)
                db = paired(rows, arm, "AODV-STOCK", c, "bytes_per_pkt")
                rb = [val(kv, "bytes_per_pkt") for (a, cc, _), kv in rows.items()
                      if a == "AODV-STOCK" and cc == c]
                if len(d) != per_cond_n[c]:
                    paired_complete = False
                if len(d) < 3:
                    continue
                lo, hi = boot(d)
                d_means.append(st.mean(d))
                ci.append((lo, hi))
                rel_means.append(100 * st.mean(db) / st.mean(rb))
            A["delta_mean_of_scenario_means"] = st.mean(d_means)
            A["better"] = sum(lo > 0 for lo, _ in ci)
            A["worse"] = sum(hi < 0 for _, hi in ci)
            A["bytes_rel_mean_of_scenario_pct"] = st.mean(rel_means)
            A["paired_complete"] = paired_complete
        out["arms"][arm] = A
    stock = out["arms"]["AODV-STOCK"]
    for arm, A in out["arms"].items():
        A["bytes_rel_ratio_of_pooled_pct"] = 100 * (A["pooled_bytes_per_pkt"] / stock["pooled_bytes_per_pkt"] - 1)
        A["timely_pooled_diff_pp"] = 100 * (A["pooled_timely"] - stock["pooled_timely"])
        A["timely_rel_ratio_of_pooled_pct"] = 100 * (A["pooled_timely"] / stock["pooled_timely"] - 1)
    return out


def cluster_boot(per_run, num_key, den_keys, n=10000, seed=20260929):
    """Ratio sum(num)/sum(den) with a percentile bootstrap over RUNS (breaks within a run are
    dependent, so the run is the resampling unit)."""
    def ratio(sample):
        num = sum(r[num_key] for r in sample)
        den = sum(sum(r[k] for k in den_keys) for r in sample)
        return num / den if den else float("nan")
    est = ratio(per_run)
    rng = random.Random(seed)
    k = len(per_run)
    bs = sorted(ratio([per_run[rng.randrange(k)] for _ in range(k)]) for _ in range(n))
    return est, bs[int(0.025 * n)], bs[int(0.975 * n) - 1]


def classifier(prefix):
    per_run = []
    if not any((ROOT / "checkpoints" / "card_campaign").glob("*/out_card_breaks.csv")):
        sys.exit("classifier step needs checkpoints/card_campaign/*/out_card_breaks.csv (event-log archive); "
                 "the values computed from it are in paper/scripts/out/numerical_recompute.json")
    for d in sorted((ROOT / "checkpoints" / "card_campaign").iterdir()):
        name = d.name
        fam = "c1_" if name.startswith("c1_") else ("c4_" if name.startswith("c4_") else "c_")
        if fam != prefix or "_CARD_s" not in name or not (d / "DONE.json").exists():
            continue
        f = d / "out_card_breaks.csv"
        if not f.exists():
            continue
        c = {"tp": 0, "fp": 0, "fn": 0, "tn": 0, "unknown": 0}
        with open(f, newline="") as fh:
            for r in csv.DictReader(fh):
                try:
                    dist, vr = float(r["dist_m"]), float(r["radial_mps"])
                except ValueError:
                    continue
                if math.isnan(dist):
                    continue
                dep = dist > RANGE_M or dist + max(vr, 0.0) * HORIZON_S > RANGE_M
                v = r["verdict"]
                if v == "2":
                    c["unknown"] += 1
                elif v == "1":
                    c["tp" if dep else "fp"] += 1
                else:
                    c["fn" if dep else "tn"] += 1
        per_run.append(c)
    if not per_run:
        return None
    res = {"runs": len(per_run), "breaks": sum(sum(r.values()) for r in per_run)}
    for name, num, den in (("recall_departure", "tp", ("tp", "fn")),
                           ("precision_topology", "tp", ("tp", "fp")),
                           ("precision_transient", "tn", ("tn", "fn"))):
        if sum(sum(r[k] for k in den) for r in per_run) == 0:
            res[name] = None
            continue
        res[name] = cluster_boot(per_run, num, den)
    return res


def f3(x):
    return ("$%+.3f$" % x).replace("+", "{+}")


def main():
    J = {"families": {}, "classifier": {}}
    lines = [B + "begin{tabular}{@{}llrrrrrr@{}}", B + "toprule",
             "Family & Policy & PDR$_{" + B + "mathrm{t}}$ & $" + B + "Delta$ & better/worse & B/pkt & "
             "$" + B + "Delta$B$_{" + B + "mathrm{scen}}$ & $" + B + "Delta$B$_{" + B + "mathrm{pool}}$ " + B + B,
             B + "midrule"]
    for tag, camp, name in FAMILIES:
        F = family(tag, camp)
        J["families"][tag] = F
        first = True
        for arm in MAIN:
            A = F["arms"].get(arm)
            if not A:
                continue
            if arm == "AODV-STOCK":
                cells = ["--", "--", "%.1f" % A["pooled_bytes_per_pkt"], "--", "--"]
            else:
                cells = [f3(A["delta_mean_of_scenario_means"]), "%d/%d" % (A["better"], A["worse"]),
                         "%.1f" % A["pooled_bytes_per_pkt"],
                         ("%+.0f" % A["bytes_rel_mean_of_scenario_pct"]).replace("+", "{+}").replace("-", "$-$") + B + "%",
                         ("%+.0f" % A["bytes_rel_ratio_of_pooled_pct"]).replace("+", "{+}").replace("-", "$-$") + B + "%"]
            lines.append("%s & %s & %.3f & %s %s%s" % (("%s (%d)" % (name, F["conditions"])) if first else "",
                                                       LABEL[arm], A["pooled_timely"], " & ".join(cells), B, B))
            first = False
        lines.append(B + "addlinespace")
    lines[-1:] = [B + "bottomrule", B + "end{tabular}"]
    (REV / "tables").mkdir(parents=True, exist_ok=True)
    (REV / "tables" / "tab_card_main_rev.tex").write_text(
        "%% generated by paper/scripts/verify_numbers.py -- do not edit\n" + "\n".join(lines) + "\n",
        encoding="utf-8")

    cl = [B + "begin{tabular}{@{}lrrrrr@{}}", B + "toprule",
          "Family & runs & breaks & recall (departures) & precision (departure class) & "
          "precision (transient class) " + B + B, B + "midrule"]
    for tag, prefix, name in (("C1", "c1_", "Bottleneck"), ("C4", "c4_", "Scaling"), ("S8", "c_", "Mobility")):
        c = classifier(prefix)
        J["classifier"][tag] = c
        if not c:
            continue
        fmt = lambda x: ("%.3f [%.3f, %.3f]" % x) if x else "--"
        cl.append("%s & %d & %s & %s & %s & %s %s%s" % (name, c["runs"], "{:,}".format(c["breaks"]).replace(",", "{,}"),
                                                       fmt(c["recall_departure"]), fmt(c["precision_topology"]),
                                                       fmt(c["precision_transient"]), B, B))
    cl += [B + "bottomrule", B + "end{tabular}"]
    (REV / "tables" / "tab_card_classifier_rev.tex").write_text(
        "%% generated by paper/scripts/verify_numbers.py -- do not edit\n" + "\n".join(cl) + "\n",
        encoding="utf-8")
    (REV / "scripts" / "out").mkdir(parents=True, exist_ok=True)
    (REV / "scripts" / "out" / "numerical_recompute.json").write_text(json.dumps(J, indent=1, default=float))
    print("wrote tab_card_main_rev.tex, tab_card_classifier_rev.tex, numerical_recompute.json")


if __name__ == "__main__":
    main()
