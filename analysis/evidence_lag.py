"""Measure the renewal window on real traffic: how long after service resumes
does the confirmation rule first become satisfiable?

For every outage episode reconstructed from the per-packet event logs (same
construction as recovery_table.py / recovery.py), T_true is the first on-time
delivery after the outage and T_source the earliest instant at which K
consecutive blocks are good, i.e. the earliest time a service confirmation
could exist if receipts returned instantly. The evidence lag is
T_source - T_true. Receipt return and freshness are not included, so the lag is
a lower bound on SCR's renewal delay.

Measured under every arm, and in particular under stock AODV, where no
rationing interferes with route dynamics: there it shows how available the
confirmation signal would be to a controller that waited for it.

Campaigns: c1_outage (bottleneck) and stage8_campaign (mobility); the scaling
campaign has no per-packet logs at the reported revision.

Run:  python analysis/evidence_lag.py   (writes analysis_out/evidence_lag.json)
"""

import json
import statistics as st
from collections import defaultdict

import recovery_table as rt

ARMS = ["AODV-STOCK", "RREP-RESET", "TIME-BUCKET", "SCR", "PERSISTENT-BACKOFF"]
CAMPAIGNS = {"C1": "c1_outage", "S8": "stage8_campaign"}
OUT = rt.ROOT / "analysis_out" / "evidence_lag.json"


def main():
    if not all((rt.ROOT / "checkpoints" / c).exists() for c in CAMPAIGNS.values()):
        print("no per-packet logs under %s; keeping %s" % (rt.ROOT / "checkpoints", OUT))
        return
    res = {}
    for tag, camp in CAMPAIGNS.items():
        acc = defaultdict(lambda: {"runs": 0, "resumed": 0, "never": 0, "lag_ms": []})
        for d in sorted((rt.ROOT / "checkpoints" / camp).iterdir()):
            if not (d / "DONE.json").exists() or not (d / "out_packet_events.csv").exists():
                continue
            kv = rt.summary_kv(d)
            alg = kv.get("algorithm")
            if alg not in ARMS:
                continue
            # same provenance rule as q1_common.load: fork runs must carry fork_rev=5
            ep = kv.get("effective_parameters", "")
            if "scr_enabled=1" in ep and "fork_rev=5" not in ep:
                continue
            eps = rt.episodes_for(d / "out_packet_events.csv")
            if not eps:
                continue
            a = acc[alg]
            a["runs"] += 1
            for e in eps:
                if e.true_censored or e.true_ns is None:
                    continue
                a["resumed"] += 1
                if e.source_censored or e.source_ns is None:
                    a["never"] += 1
                else:
                    a["lag_ms"].append((e.source_ns - e.true_ns) / 1e6)
        res[tag] = {}
        for alg in ARMS:
            a = acc[alg]
            lag = sorted(a["lag_ms"])
            res[tag][alg] = {
                "runs": a["runs"],
                "resumed_episodes": a["resumed"],
                "never_confirmable_fraction": a["never"] / a["resumed"] if a["resumed"] else None,
                "lag_median_ms": st.median(lag) if lag else None,
                "lag_p75_ms": lag[int(0.75 * len(lag))] if lag else None,
                "lag_gt_1s_fraction": sum(1 for x in lag if x > 1000) / len(lag) if lag else None,
                # empirical CDF over ALL resumed episodes (never-confirmable ones never reach it):
                # value at q is the lag (ms) by which a fraction q of recoveries became confirmable
                "ecdf_ms": ([lag[min(len(lag) - 1, int(q / 200 * a["resumed"]))]
                             if int(q / 200 * a["resumed"]) < len(lag) else None
                             for q in range(0, 201)] if lag else None),
            }
            r = res[tag][alg]
            print("%-3s %-19s runs=%3d resumed=%6d never=%5.1f%% median=%6.0f ms  >1s=%5.1f%%" % (
                tag, alg, r["runs"], r["resumed_episodes"], 100 * r["never_confirmable_fraction"],
                r["lag_median_ms"], 100 * r["lag_gt_1s_fraction"]))
    OUT.write_text(json.dumps({"parameters": {"K": rt.K, "M": rt.M, "deadline_ms": 250,
                                              "gap_multiple": rt.GAP, "cap_s": 30},
                               "lag": res}, indent=1))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
