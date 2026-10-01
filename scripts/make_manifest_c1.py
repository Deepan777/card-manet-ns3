#!/usr/bin/env python3
"""
C1 manifest — controlled bridge outage (RQ1, RQ2, RQ5, RQ10).

C1 is the only family in which route loss is IMPOSED rather than incidental, so
it is the only place the recovery questions can be answered. Its topology is two
clusters 400 m apart, connected solely by two bridge groups; measured
`direct_cross_links=0` against a 141 m radio range, so a bridge outage is a
genuine partition and not a detour.

CONDITIONS (the five registered fault patterns, plus the cold-start variant)

    healthy         no fault; the reference arm
    single          bridge A down [100, 120)
    repeated        A down [100,104.2) [108,112.2) [116,120)
                    B down [104,108.2) [112,116.2)
                    -> deliberate 200 ms overlaps at four switching boundaries
    long            all ten bridge nodes down [100, 160)
    cold            repeated bridge events, all eight flows starting at 100 s
    cold_staggered  as cold, but starts at 100 + 0.1*i s

`cold` tests concurrent discovery from a standing start, not recovery of running
service; the two are different questions and are kept as separate conditions.

OUTAGE MECHANISM -- read D006 before interpreting these runs

Unavailability is expressed by taking the bridge node's IPv4 interfaces down,
NOT by switching the Wi-Fi PHY off as originally specified. `WifiPhy::SetOffMode()`
corrupts MAC state in ns-3.46.1 when an outage boundary lands during a frame
exchange, and it aborted two of these five patterns outright. Patching vendored
ns-3 would destroy the provenance the study rests on. Node software state --
the SCR ledger, episode counters, escalation index -- survives the outage either
way, which is the property the fixture exists to test. What differs is that the
radio remains physically present, so neighbours see a silent-but-present node.
C1 results must be described as IP-layer node unavailability.

A CAUTION ON THE PRIMARY ENDPOINT HERE

Measured on this topology, timely PDR is ~0.08 against ~0.32 in C2: the
two-cluster geometry forces long paths against the same 250 ms deadline. That
floor is a property of the scenario, not of any algorithm, but it compresses the
range in which methods can differ on timeliness. **The recovery analysis
(analysis/recovery.py, M19) carries RQ1/RQ2/RQ5, and timely PDR is secondary
here.** Do not read a small timely-PDR difference in C1 as evidence of
equivalence.
"""

import argparse
import hashlib
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BINARY = (ROOT / "vendor/ns-3-dev-ns-3.46.1/build/scratch/scr-c1-outage/"
          "ns3.46.1-scr-c1-outage-default.exe")

CORE = ["SCR", "RREP-RESET", "TIME-BUCKET", "PERSISTENT-BACKOFF",
        "AODV-FB", "AODV-STOCK", "OLSR", "DSDV"]

SEED_BASE = 20260905
CAMPAIGN_SEEDS_ALL = list(range(201, 231))

# (condition label, --fault value, --coldStagger)
CONDITIONS = [
    ("healthy", "healthy", 0),
    ("single", "single", 0),
    ("repeated", "repeated", 0),
    ("long", "long", 0),
    ("cold", "cold", 0),
    ("cold_staggered", "cold", 1),
]


def make_run(algo, seed, label, fault, stagger):
    key = "c1|%s|seed=%d|%s" % (algo, seed, label)
    rid = "c1_%s_%s_s%d_%s" % (label, algo.replace("-", ""), seed,
                               hashlib.sha256(key.encode()).hexdigest()[:8])
    args = ["--algorithm=%s" % algo,
            "--fault=%s" % fault,
            "--coldStagger=%d" % stagger,
            "--rngRun=%d" % seed,
            "--rngSeed=%d" % SEED_BASE,
            "--outPrefix=out"]
    return rid, {
        "run_id": rid,
        "args": args,
        # Observed solo wall time on this scenario is 380-700 s. 3600 s is ~5x
        # the slowest observed, and generous rather than tuned; the R014 heavy
        # tail that made large timeouts necessary is gone.
        "timeout_s": 3600,
        "meta": {"algorithm": algo, "rng_run": seed, "condition": label,
                 "fault": fault, "cold_stagger": stagger},
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=10,
                    help="seeds per condition")
    ap.add_argument("--out", default=str(ROOT / "configs" / "c1_outage.json"))
    args = ap.parse_args()

    seeds = CAMPAIGN_SEEDS_ALL[:args.n]
    runs = []
    for algo in CORE:
        for label, fault, stagger in CONDITIONS:
            for s in seeds:
                _, rec = make_run(algo, s, label, fault, stagger)
                runs.append(rec)

    runs.sort(key=lambda r: r["run_id"])
    manifest = {
        "campaign_id": "c1_outage",
        "purpose": "C1 controlled bridge outage: the recovery questions.",
        "scope_limitation": (
            "C1 answers RQ1, RQ2, RQ5 and contributes the permanent-outage cell of "
            "RQ10. It does NOT cover RQ3/RQ4/RQ7 (C2), RQ6 (C4) or RQ9 (C3)."),
        "outage_mechanism": (
            "IPv4 interface down/up, NOT WifiPhy off (D006). ns-3.46.1 corrupts MAC "
            "state when SetOffMode lands during a frame exchange, which aborted the "
            "'repeated' and 'long' patterns. Describe results as IP-layer node "
            "unavailability, not radio blackout."),
        "endpoint_caution": (
            "timely PDR on this topology is ~0.08 (vs ~0.32 in C2) because the "
            "two-cluster geometry forces long paths against the same 250 ms deadline. "
            "Recovery analysis (M19) is primary here; a small timely-PDR difference is "
            "NOT evidence of equivalence."),
        "topology_validation": (
            "min_cluster_to_cluster_m=400, direct_cross_links=0, assumed_range_m=141; "
            "reproduce with --validateTopology=1"),
        "binary": str(BINARY),
        "scenario_source": "ns3/scenarios/scr-c1-outage.cc",
        "ns3_version": "3.46.1",
        "ns3_commit": "51387bce7e5f5c57aa080612a4ed690bf33d0c92",
        "seeds": seeds,
        "n_per_condition": args.n,
        "n_selection_note": (
            "FIRST PASS at n=10, declared in advance. The C2 n-selection (n=30) was "
            "derived from C2 variance and does not transfer: C1 imposes outages and "
            "its paired variance is unknown. This pass estimates s_d per condition; "
            "the registered rule 1.96*s_d/sqrt(n)<=0.02 then decides whether to "
            "extend. Seeds are the first 10 of the held-out block, so extending ADDS "
            "seeds and never rewrites these runs."),
        "runs": runs,
    }

    out = pathlib.Path(args.out)
    out.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    from collections import Counter
    per = Counter(r["meta"]["condition"] for r in runs)
    print("wrote %s" % out)
    print("  total runs        : %d" % len(runs))
    print("  seeds per condition: %d" % args.n)
    for c, _, _ in CONDITIONS:
        print("  %-16s %d runs" % (c, per[c]))


if __name__ == "__main__":
    main()
