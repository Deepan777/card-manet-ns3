"""SCR-2 (allowance removed) against SCR and stock AODV on C1, fork_rev 5.

Tests the explanation "SCR fails because its limiter is too strict". If that
were the cause, removing the per-pair allowance should recover delivery toward
stock AODV. Pairs each SCR-2 run (checkpoints/c1_scr2_supplement) with the
published C1 SCR and AODV-STOCK runs of the same seed and condition
(checkpoints/c1_outage). Paired differences, percentile bootstrap over
seed-level differences, exact two-sided sign test.

Run:  python analysis/scr2_supplement.py
"""

import json
import math
import os
import pathlib
import random
import statistics as st
from collections import defaultdict

ROOT = pathlib.Path(os.environ.get("SCR_ROOT", pathlib.Path(__file__).resolve().parent.parent))
OUT = ROOT / "analysis_out" / "scr2_supplement.json"
BOOT_N, BOOT_SEED = 10000, 20260905


def load(camp):
    rows = {}
    for d in (ROOT / "checkpoints" / camp).iterdir():
        s = d / "out_summary.csv"
        if not (d.is_dir() and s.exists() and (d / "DONE.json").exists()):
            continue
        kv = {}
        for line in s.read_text(errors="replace").splitlines()[1:]:
            k, _, v = line.partition(",")
            kv[k.strip()] = v.strip().strip('"')
        ep = kv.get("effective_parameters", "")
        if "scr_enabled=1" in ep and "fork_rev=5" not in ep:
            continue                                   # provenance gate
        cond = "%s/stagger=%s" % (kv["fault"], kv["cold_stagger"])
        rows[(kv["algorithm"], cond, kv["rng_run"])] = kv
    return rows


def metric(kv, name):
    g = float(kv["generated"])
    if name == "timely":
        return float(kv["ontime_true"]) / g
    if name == "bytes_per_pkt":
        return float(kv["ctrl_bytes_sent"]) / g
    if name == "originations":
        return float(kv["source_rreq_originated"])
    return float(kv[name])


def boot(d):
    rng = random.Random(BOOT_SEED)
    m = sorted(sum(rng.choice(d) for _ in d) / len(d) for _ in range(BOOT_N))
    return m[int(0.025 * BOOT_N)], m[int(0.975 * BOOT_N) - 1]


def sign_p(d):
    pos, neg = sum(x > 0 for x in d), sum(x < 0 for x in d)
    n = pos + neg
    if n == 0:
        return 1.0
    k = min(pos, neg)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n)


def main():
    rows = {**load("c1_outage"), **load("c1_scr2_supplement")}
    conds = sorted({c for (_, c, _) in rows})
    seeds = sorted({s for (_, _, s) in rows})
    res = {"per_condition": {}, "pooled": {}}
    pooled = defaultdict(list)
    for cond in conds + ["ALL"]:
        cs = conds if cond == "ALL" else [cond]
        out = {}
        for ref in ("AODV-STOCK", "SCR"):
            for m in ("timely", "bytes_per_pkt", "originations", "service_confirmations"):
                d = [metric(rows[("SCR2", c, s)], m) - metric(rows[(ref, c, s)], m)
                     for c in cs for s in seeds
                     if ("SCR2", c, s) in rows and (ref, c, s) in rows]
                if len(d) < 2:
                    continue
                lo, hi = boot(d)
                out["SCR2 - %s: %s" % (ref, m)] = {
                    "n": len(d), "mean": round(st.mean(d), 4),
                    "ci95": [round(lo, 4), round(hi, 4)], "sign_p": round(sign_p(d), 5)}
        absm = {}
        for alg in ("AODV-STOCK", "SCR", "SCR2", "RREP-RESET"):
            for m in ("timely", "bytes_per_pkt", "originations"):
                v = [metric(rows[(alg, c, s)], m) for c in cs for s in seeds if (alg, c, s) in rows]
                if v:
                    absm["%s %s" % (alg, m)] = round(st.mean(v), 4)
        res["per_condition" if cond != "ALL" else "pooled"][cond] = {"paired": out, "absolute": absm}
    OUT.write_text(json.dumps(res, indent=1))
    p = res["pooled"]["ALL"]
    print("SCR-2 runs:", sum(1 for k in rows if k[0] == "SCR2"))
    print("absolute (pooled C1):")
    for k, v in p["absolute"].items():
        print("   %-32s %s" % (k, v))
    print("paired (pooled over conditions, n = seed x condition pairs):")
    for k, v in p["paired"].items():
        print("   %-44s n=%-3d %+.4f  CI %s  sign p=%s" % (k, v["n"], v["mean"], v["ci95"], v["sign_p"]))
    print("per condition, SCR2 - STOCK timely:")
    for c in conds:
        v = res["per_condition"][c]["paired"].get("SCR2 - AODV-STOCK: timely")
        w = res["per_condition"][c]["paired"].get("SCR2 - SCR: timely")
        if v:
            print("   %-22s vs stock %+.4f %s | vs SCR %+.4f %s" % (c, v["mean"], v["ci95"], w["mean"], w["ci95"]))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
