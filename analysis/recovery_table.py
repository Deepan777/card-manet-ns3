"""Aggregate per-run recovery analysis into the manuscript's recovery table.

The recovery table in the draft had no generating script: its AODV-STOCK row
matched a ledger entry marked "nothing here may be reported as a result" (a
partial, pre-fork_rev-5 campaign) and its other rows had no recorded source.
This regenerates it from the per-packet event logs of the fork_rev-5 C1
campaign, using analysis/recovery.py's own episode construction unchanged.

Per arm and condition it reports mean episodes per run, the pooled fraction of
episodes censored at the 30 s cap, and the median observed true-recovery time.
Medians are over OBSERVED episodes only and must never be quoted without the
censoring fraction beside them (see recovery.py).

Run:  python analysis/recovery_table.py
"""

import csv
import importlib.util
import json
import os
import pathlib
import statistics as st
from collections import defaultdict

ROOT = pathlib.Path(os.environ.get("SCR_ROOT", pathlib.Path(__file__).resolve().parent.parent))
spec = importlib.util.spec_from_file_location("recovery", pathlib.Path(__file__).resolve().parent / "recovery.py")
rec = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rec)

ARMS = ["AODV-STOCK", "RREP-RESET", "TIME-BUCKET", "SCR", "PERSISTENT-BACKOFF"]
DEADLINE_NS, GAP, CAP_NS, K, M = int(250e6), 4.0, int(30e9), 2, 10
OUT = ROOT / "analysis_out" / "recovery_c1_rev5.json"


def summary_kv(d):
    kv = {}
    for line in (d / "out_summary.csv").read_text(errors="replace").splitlines()[1:]:
        k, _, v = line.partition(",")
        kv[k.strip()] = v.strip().strip('"')
    return kv


def episodes_for(path):
    rows = []
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                rows.append({"flow": int(r["flow"]), "sequence": int(r["sequence"]),
                             "generation_ns": int(r["generation_ns"]),
                             "first_rx_ns": int(r["first_rx_ns"]) if r["first_rx_ns"] else 0,
                             "ontime": r["ontime"] == "1"})
            except (KeyError, ValueError):
                continue
    if not rows:
        return None
    end_ns = max(r["generation_ns"] for r in rows)
    return rec.build_episodes(rows, DEADLINE_NS, GAP, CAP_NS, end_ns, K, M)


def main():
    acc = defaultdict(lambda: {"runs": 0, "episodes": 0, "censored": 0, "true_obs": []})
    base = ROOT / "checkpoints" / "c1_outage"
    if not base.exists():
        print("no per-packet logs under %s; keeping %s" % (base, OUT))
        return
    for d in sorted(base.iterdir()):
        if not (d / "DONE.json").exists() or not (d / "out_packet_events.csv").exists():
            continue
        kv = summary_kv(d)
        alg = kv.get("algorithm")
        if alg not in ARMS:
            continue
        cond = "%s/stagger=%s" % (kv.get("fault"), kv.get("cold_stagger"))
        eps = episodes_for(d / "out_packet_events.csv")
        if eps is None:
            continue
        for key in ((alg, cond), (alg, "ALL")):
            a = acc[key]
            a["runs"] += 1
            a["episodes"] += len(eps)
            a["censored"] += sum(1 for e in eps if e.true_censored)
            a["true_obs"] += [e.true_ns / 1e6 for e in eps
                              if not e.true_censored and e.true_ns is not None]
    table = {}
    for (alg, cond), a in sorted(acc.items()):
        table.setdefault(cond, {})[alg] = {
            "runs": a["runs"],
            "episodes_per_run": round(a["episodes"] / a["runs"], 1),
            "censored_fraction": round(a["censored"] / a["episodes"], 3) if a["episodes"] else None,
            "median_true_ms_observed": round(st.median(a["true_obs"])) if a["true_obs"] else None,
        }
    OUT.write_text(json.dumps({"parameters": {"deadline_ms": 250, "gap_multiple": GAP,
                                              "cap_s": 30, "K": K, "M": M},
                               "table": table}, indent=1))
    for cond in ("healthy/stagger=0", "ALL"):
        print("==", cond)
        for alg in ARMS:
            r = table.get(cond, {}).get(alg)
            if r:
                print("  %-19s runs=%-3d episodes/run=%6.1f  censored=%5.1f%%  median=%s ms"
                      % (alg, r["runs"], r["episodes_per_run"], 100 * r["censored_fraction"],
                         r["median_true_ms_observed"]))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
