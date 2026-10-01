"""Recompute every quantitative claim in the manuscript from the run records.

Phase-1 audit artefact for the Q1 reconstruction. It does not trust the
manuscript, the ledger or any earlier analysis output: every number is derived
here from checkpoints/<campaign>/<run>/out_summary.csv (fork_rev 5 only), and
written to analysis_out/audit_q1_evidence.json so CLAIM_EVIDENCE_MATRIX.csv can
cite a single reproducible source.

Run:  python analysis/audit_q1_evidence.py
"""

import json
import math
import os
import pathlib
import random
import statistics as st
from collections import defaultdict

ROOT = pathlib.Path(os.environ.get("SCR_ROOT", pathlib.Path(__file__).resolve().parent.parent))
CK = ROOT / "checkpoints"
OUT = ROOT / "analysis_out" / "audit_q1_evidence.json"

ARMS = ["RREP-RESET", "TIME-BUCKET", "SCR", "PERSISTENT-BACKOFF"]   # claimed order
REF = "AODV-STOCK"
ABL = ["NO-PROBE", "ONE-BLOCK", "NO-DEADLINE", "NO-NODE-CAP", "RESET-ESCALATION"]
CAMPAIGNS = {"C1": "c1_outage", "C4": "c4_scaling", "S8": "stage8_campaign"}
# Scenario parameters that define a condition. Anything varying within a
# campaign is part of the key; seed and algorithm never are.
IGNORE = {"key", "algorithm", "effective_parameters", "rng_seed", "rng_run"}


def load(camp):
    rows = []
    for d in (CK / camp).iterdir():
        s = d / "out_summary.csv"
        if not (d.is_dir() and s.exists() and (d / "DONE.json").exists()):
            continue
        kv = {}
        for line in s.read_text(errors="replace").splitlines()[1:]:
            k, _, v = line.partition(",")
            kv[k.strip()] = v.strip().strip('"')
        ep = kv.get("effective_parameters", "")
        rev = next((int(t.split("=")[1]) for t in ep.split() if t.startswith("fork_rev=")), None)
        kv["_rev"] = rev
        rows.append(kv)
    return rows


def f(kv, k):
    try:
        return float(kv[k])
    except (KeyError, ValueError):
        return None


def cond_fields(rows):
    """Fields whose value varies across runs, excluding outcomes."""
    outcome_like = {"generated", "received", "duplicates", "late", "ontime_true",
                    "timely_pdr", "no_route_drops"}
    cands = set()
    for r in rows:
        cands |= set(r)
    fixed = []
    for k in sorted(cands):
        if k in IGNORE or k.startswith("_"):
            continue
        vals = {r.get(k) for r in rows}
        # a scenario parameter takes few values; outcomes take many
        if 1 < len(vals) <= 6 and k not in outcome_like and not any(
                x in k for x in ("ctrl_", "radio_", "deny_", "scr_", "report", "service_",
                                 "source_", "rreq_", "transport")):
            fixed.append(k)
    return fixed


def boot(d, n=10000, seed=20260905):
    rng = random.Random(seed)
    m = sorted(sum(rng.choice(d) for _ in d) / len(d) for _ in range(n))
    return m[int(0.025 * n)], m[int(0.975 * n) - 1]


def pct(a, b):
    return 100.0 * (a / b - 1.0)


def main():
    out = {"provenance": {}, "per_condition": {}, "monotone": {}, "tables": {},
           "relative": {}, "origination_vs_bytes": {}, "timescale": {},
           "no_deadline_s8": {}, "stage8_rrep_reset": {}, "ablation_gap": {}}
    data = {}
    for tag, camp in CAMPAIGNS.items():
        rows = load(camp)
        revs = defaultdict(int)
        for r in rows:
            revs[str(r["_rev"])] += 1
        # native arms do not enter the fork and carry no fork_rev requirement
        scr_rows = [r for r in rows if "scr_enabled=1" in r.get("effective_parameters", "")]
        bad = [r for r in scr_rows if (r["_rev"] or 0) < 5]
        out["provenance"][tag] = {"runs": len(rows), "fork_rev_counts": dict(revs),
                                  "scr_enabled_runs_below_rev5": len(bad)}
        cf = cond_fields(rows)
        out["provenance"][tag]["condition_fields"] = cf
        by = defaultdict(lambda: defaultdict(dict))
        for r in rows:
            c = tuple(r.get(k) for k in cf)
            by[c][r["algorithm"]][r["rng_run"]] = r
        data[tag] = (cf, by)

    # ---------------- per-condition means and monotonicity ----------------
    for tag, (cf, by) in data.items():
        conds = []
        for c in sorted(by, key=lambda c: tuple((x or "") for x in c)):
            a = by[c]
            mean = {}
            for alg in ARMS + [REF]:
                if alg in a:
                    v = [f(r, "ontime_true") / f(r, "generated") for r in a[alg].values()
                         if f(r, "generated")]
                    mean[alg] = (st.mean(v), len(v))
            vals = [mean[x][0] for x in ARMS if x in mean]
            gaps = [vals[i] - vals[i + 1] for i in range(len(vals) - 1)]
            conds.append({
                "condition": dict(zip(cf, c)),
                "means": {k: round(v[0], 4) for k, v in mean.items()},
                "n": {k: v[1] for k, v in mean.items()},
                "strictly_monotone": len(vals) == 4 and all(g > 0 for g in gaps),
                "min_adjacent_gap": round(min(gaps), 4) if gaps else None,
                "stock_rank_among_5": (sorted(list(mean), key=lambda k: -mean[k][0])
                                       .index(REF) + 1) if REF in mean else None,
            })
        out["per_condition"][tag] = conds
        out["monotone"][tag] = {"conditions": len(conds),
                                "strictly_monotone": sum(c["strictly_monotone"] for c in conds),
                                "smallest_gap_anywhere": min(c["min_adjacent_gap"] for c in conds)}

    # ---------------- Table 2 (paired diff vs stock) and Table 3 ----------------
    for tag, (cf, by) in data.items():
        t2, t3 = {}, {}
        for alg in ARMS + [REF]:
            cond_means, pooled_t, pooled_b = [], [], []
            for c, a in by.items():
                if alg not in a or REF not in a:
                    continue
                diffs = []
                for s, r in a[alg].items():
                    if s in a[REF] and f(r, "generated") and f(a[REF][s], "generated"):
                        diffs.append(f(r, "ontime_true") / f(r, "generated")
                                     - f(a[REF][s], "ontime_true") / f(a[REF][s], "generated"))
                if diffs:
                    cond_means.append(st.mean(diffs))
                for r in a[alg].values():
                    if f(r, "generated"):
                        pooled_t.append(f(r, "ontime_true") / f(r, "generated"))
                        pooled_b.append(f(r, "ctrl_bytes_sent") / f(r, "generated"))
            if alg != REF and cond_means:
                t2[alg] = {"mean_of_condition_paired_diffs": round(st.mean(cond_means), 4),
                           "conditions": len(cond_means),
                           "min": round(min(cond_means), 4), "max": round(max(cond_means), 4)}
            if pooled_t:
                t3[alg] = {"timely": round(st.mean(pooled_t), 4),
                           "bytes_per_pkt": round(st.mean(pooled_b), 2), "n": len(pooled_t)}
        out["tables"][tag] = {"table2_paired_diff_vs_stock": t2, "table3_absolute": t3}

    # ---------------- relative changes stated in prose ----------------
    t3 = {k: out["tables"][k]["table3_absolute"] for k in ("C1", "C4")}
    rel = {}
    for tag in ("C1", "C4"):
        s = t3[tag][REF]
        for alg in ARMS:
            a = t3[tag][alg]
            rel["%s %s timely" % (tag, alg)] = round(pct(a["timely"], s["timely"]), 1)
            rel["%s %s bytes/pkt" % (tag, alg)] = round(pct(a["bytes_per_pkt"], s["bytes_per_pkt"]), 1)
    out["relative"] = rel

    # C1 per condition: is "under induced link outage" accurate for the pooled figure?
    cf, by = data["C1"]
    c1pc = []
    for c, a in sorted(by.items()):
        if "RREP-RESET" in a and REF in a:
            def m(alg, key):
                return st.mean(f(r, key) / f(r, "generated") for r in a[alg].values())
            c1pc.append({"condition": dict(zip(cf, c)),
                         "RR_timely": round(m("RREP-RESET", "ontime_true"), 4),
                         "STOCK_timely": round(m(REF, "ontime_true"), 4),
                         "rel_timely_pct": round(pct(m("RREP-RESET", "ontime_true"),
                                                     m(REF, "ontime_true")), 1),
                         "rel_bytes_pct": round(pct(m("RREP-RESET", "ctrl_bytes_sent"),
                                                    m(REF, "ctrl_bytes_sent")), 1)})
    out["c1_rrep_reset_by_condition"] = c1pc

    # ---------------- origination count vs channel bytes ----------------
    for tag in ("C1", "C4", "S8"):
        cf, by = data[tag]
        res = {}
        for alg in ARMS:
            o, b, os_, bs = [], [], [], []
            for a in by.values():
                if alg in a and REF in a:
                    for s, r in a[alg].items():
                        if s in a[REF]:
                            o.append(f(r, "source_rreq_originated")); b.append(f(r, "ctrl_bytes_sent"))
                            os_.append(f(a[REF][s], "source_rreq_originated"))
                            bs.append(f(a[REF][s], "ctrl_bytes_sent"))
            if o and all(x is not None for x in o + os_):
                res[alg] = {"originations_pct": round(pct(sum(o), sum(os_)), 1),
                            "ctrl_bytes_pct": round(pct(sum(b), sum(bs)), 1)}
        out["origination_vs_bytes"][tag] = res

    # ---------------- timescale: matched-unit counts per run ----------------
    for tag in ("C1", "C4", "S8"):
        cf, by = data[tag]
        for alg in ("SCR", "RREP-RESET", REF):
            rs = [r for a in by.values() for r in a.get(alg, {}).values()]
            if not rs:
                continue
            def mean_of(k):
                v = [f(r, k) for r in rs if f(r, k) is not None]
                return round(st.mean(v), 1) if v else None
            out["timescale"]["%s %s" % (tag, alg)] = {
                "runs": len(rs),
                "service_confirmations_per_run": mean_of("service_confirmations"),
                "reports_accepted_per_run": mean_of("reports_accepted"),
                "ctrl_rerr_sent_per_run (messages, network-wide)": mean_of("ctrl_rerr_sent"),
                "source_rreq_originated_per_run": mean_of("source_rreq_originated"),
                "deny_no_allowance_per_run": mean_of("deny_no_allowance"),
                "deny_in_flight_per_run": mean_of("deny_in_flight"),
                "scr_denied_per_run": mean_of("scr_denied"),
            }

    # ---------------- NO-DEADLINE vs SCR, Stage 8, traceable at n=60 ----------------
    cf, by = data["S8"]
    nd = defaultdict(list)
    for c, a in by.items():
        if "NO-DEADLINE" in a and "SCR" in a:
            for s, r in a["NO-DEADLINE"].items():
                q = a["SCR"].get(s)
                if not q:
                    continue
                for k in ("late", "reports_accepted", "reports_rejected",
                          "service_confirmations", "ontime_true"):
                    if f(r, k) is not None and f(q, k) is not None:
                        nd[k].append(f(r, k) - f(q, k))
                nd["timely_pdr"].append(f(r, "ontime_true") / f(r, "generated")
                                        - f(q, "ontime_true") / f(q, "generated"))
    out["no_deadline_s8"] = {k: {"n_pairs": len(v), "mean_diff": round(st.mean(v), 4),
                                 "ci95": [round(x, 4) for x in boot(v)],
                                 "pairs_identical": sum(1 for x in v if x == 0)}
                             for k, v in nd.items()}

    # ---------------- Stage 8 RREP-RESET vs stock per condition ----------------
    rr = []
    for c, a in sorted(by.items()):
        if "RREP-RESET" in a and REF in a:
            d = [f(r, "ontime_true") / f(r, "generated")
                 - f(a[REF][s], "ontime_true") / f(a[REF][s], "generated")
                 for s, r in a["RREP-RESET"].items() if s in a[REF]]
            lo, hi = boot(d)
            rr.append({"condition": dict(zip(cf, c)), "n": len(d),
                       "mean_diff": round(st.mean(d), 4), "ci95": [round(lo, 4), round(hi, 4)],
                       "ci_excludes_zero": hi < 0 or lo > 0})
    out["stage8_rrep_reset"] = rr

    # ---------------- ablation interval widths vs the SCR-stock gap ----------------
    for c, a in by.items():
        if not all(x in a for x in ABL + ["SCR", REF]):
            continue
        key = ", ".join("%s=%s" % kv for kv in zip(cf, c))
        gap = st.mean(f(r, "ontime_true") / f(r, "generated")
                      - f(a[REF][s], "ontime_true") / f(a[REF][s], "generated")
                      for s, r in a["SCR"].items() if s in a[REF])
        ab = {}
        for x in ABL:
            d = [f(r, "ontime_true") / f(r, "generated")
                 - f(a["SCR"][s], "ontime_true") / f(a["SCR"][s], "generated")
                 for s, r in a[x].items() if s in a["SCR"]]
            lo, hi = boot(d)
            ab[x] = {"n": len(d), "mean": round(st.mean(d), 4), "ci95": [round(lo, 4), round(hi, 4)],
                     "max_abs_bound": round(max(abs(lo), abs(hi)), 4)}
        nulls = [v for k, v in ab.items() if k != "NO-PROBE"]
        widest = max(v["max_abs_bound"] for v in nulls)
        out["ablation_gap"][key] = {"scr_minus_stock": round(gap, 4), "ablations_vs_scr": ab,
                                    "widest_null_bound": widest,
                                    "gap_over_widest_bound": round(abs(gap) / widest, 1)}

    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(json.dumps(out, indent=1, default=str))
    print("wrote", OUT)
    for tag, m in out["monotone"].items():
        print("monotone %-3s %d/%d  smallest adjacent gap %.4f"
              % (tag, m["strictly_monotone"], m["conditions"], m["smallest_gap_anywhere"]))


if __name__ == "__main__":
    main()
