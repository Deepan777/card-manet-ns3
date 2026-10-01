#!/usr/bin/env python3
"""
Stage 8 campaign manifest — EXPLICIT and DEDUPLICATED.

The specification forbids a blind Cartesian product: every run must be an
enumerated, inspected identity. This generator therefore builds each research
question's rows separately and then deduplicates by run identity, so the shared
nominal condition (speed 5 m/s, 20 pkt/s, 8 flows) is executed ONCE and reused
by every question that needs it.

SCOPE LIMITATION - read before using the output.

Only scenario family C2 (random mobile MANET) is implemented. The campaign below
therefore covers, in whole or part:

    RQ3  mobility            speeds 1/5/10/15 m/s          FULL
    RQ4  contention/load     rates 5/20/40, flows 2/8/16   FULL
    RQ7  contribution        ablations at nominal + load   PARTIAL (C2 half only)
    RQ8  parameters          B, rho sweeps                 PARTIAL

It CANNOT cover:

    RQ1, RQ2, RQ5, RQ10   require C1 (controlled bridge outage and churn)
    RQ6                   requires C4 (scale and density families)
    RQ9                   requires C3 (fading, feedback loss, clock error)

C1, C3 and C4 scenarios are not implemented. Those questions remain unanswerable
and must not be reported as anything other than out of scope for this campaign.
"""

import argparse
import hashlib
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BINARY = (ROOT / "vendor/ns-3-dev-ns-3.46.1/build/scratch/scr-manet/"
          "ns3.46.1-scr-manet-default.exe")

CORE = ["SCR", "RREP-RESET", "TIME-BUCKET", "PERSISTENT-BACKOFF",
        "AODV-FB", "AODV-STOCK", "OLSR", "DSDV"]
ABLATIONS = ["RESET-ESCALATION", "NO-PROBE", "ONE-BLOCK", "NO-DEADLINE", "NO-NODE-CAP"]

SEED_BASE = 20260905
# Held-out campaign seeds, disjoint from development seeds 101-105.
CAMPAIGN_SEEDS_ALL = list(range(201, 231))

NOMINAL = {"speed": 5.0, "ratePps": 20.0, "numFlows": 8}

# Evidence block rule constants (E2), needed for the preregistered freshness rule.
K_BLOCKS = 2
BLOCK_M = 10
NOMINAL_FRESHNESS = 2.0


def freshness_for(rate_pps):
    """Preregistered rule: F = max(2 s, (K-1)*M*I + 1 s), applied to EVERY method.

    Specification, C4: "For the 5 packets/s case set F = max(2 s,(K-1)MI+1 s) = 3 s
    by a preregistered rule for every method using feedback. This prevents an
    impossible freshness condition and is not an algorithm-specific tuning
    concession."

    Why this is not optional. At 5 pkt/s the interval I is 0.2 s, so one block of
    M=10 packets spans 2.0 s. With F left at the nominal 2.0 s, the window cannot
    cover the K=2 consecutive good blocks that E2 requires, so service can NEVER
    be confirmed. Every feedback method -- SCR, RREP-RESET, TIME-BUCKET,
    PERSISTENT-BACKOFF, AODV-FB -- would then show catastrophic behaviour at low
    rate for a reason that is an artefact of the manifest, not a property of the
    mechanism.

    This rule was missing from this generator until 2026-09-10 (R022). It has
    always been present in the C4 generator.
    """
    interval = 1.0 / rate_pps
    return max(NOMINAL_FRESHNESS, (K_BLOCKS - 1) * BLOCK_M * interval + 1.0)


def make_run(algo, seed, overrides, rq_tags):
    params = dict(NOMINAL)
    params.update(overrides)
    key = "campaign|%s|seed=%d|%s" % (
        algo, seed, "|".join("%s=%s" % (k, params[k]) for k in sorted(params)))
    rid = "c_%s_s%d_%s" % (algo.replace("-", ""), seed,
                           hashlib.sha256(key.encode()).hexdigest()[:8])
    # F is DERIVED from the rate here, never passed in per-cell, so no condition
    # can quietly receive a different freshness window. Each run reports the
    # applied freshness_s in its summary for audit.
    fresh = freshness_for(float(params["ratePps"]))
    args = ["--algorithm=%s" % algo,
            "--rngRun=%d" % seed,
            "--rngSeed=%d" % SEED_BASE,
            "--speed=%s" % params["speed"],
            "--ratePps=%s" % params["ratePps"],
            "--numFlows=%d" % int(params["numFlows"]),
            "--freshnessS=%.6f" % fresh,
            # R024: per-packet event log. RQ3's supporting metrics (p50/p95
            # delay, missed packets) and RQ4's flow completion cannot be
            # computed from the aggregate summary.
            "--packetEvents=1",
            "--outPrefix=out"]
    return rid, {
        "run_id": rid,
        "args": args,
        "timeout_s": 1800,   # fixed-build max is 444 s; 4x headroom
        "meta": {"algorithm": algo, "rng_run": seed, "rq": sorted(rq_tags),
                 "freshness_s": fresh, **params},
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=20,
                    help="seeds per condition; set from the pilot n-selection rule")
    ap.add_argument("--out", default=str(ROOT / "configs" / "stage8_campaign.json"))
    args = ap.parse_args()

    seeds = CAMPAIGN_SEEDS_ALL[:args.n]
    runs = {}
    tags = {}

    def add(algo, seed, overrides, rq):
        rid, rec = make_run(algo, seed, overrides, {rq})
        if rid in runs:
            tags[rid].add(rq)           # shared condition: reuse, do not duplicate
        else:
            runs[rid] = rec
            tags[rid] = {rq}

    # RQ3 mobility: speeds 1/5/10/15
    for algo in CORE:
        for sp in (1.0, 5.0, 10.0, 15.0):
            for s in seeds:
                add(algo, s, {"speed": sp}, "RQ3")

    # RQ4a contention: rates 5/20/40 at nominal flows
    for algo in CORE:
        for r in (5.0, 20.0, 40.0):
            for s in seeds:
                add(algo, s, {"ratePps": r}, "RQ4")

    # RQ4b load: flows 2/8/16 at nominal rate
    for algo in CORE:
        for f in (2, 8, 16):
            for s in seeds:
                add(algo, s, {"numFlows": f}, "RQ4")

    # RQ7 contribution isolation: ablations at nominal and at high load
    for algo in ABLATIONS + ["SCR"]:
        for ov in ({}, {"ratePps": 40.0}):
            for s in seeds:
                add(algo, s, ov, "RQ7")

    for rid, rq in tags.items():
        runs[rid]["meta"]["rq"] = sorted(rq)

    ordered = [runs[k] for k in sorted(runs)]
    manifest = {
        "campaign_id": "stage8_campaign",
        "purpose": "Held-out campaign over scenario family C2 only.",
        "scope_limitation": (
            "C1, C3 and C4 scenarios are NOT implemented. RQ1, RQ2, RQ5, RQ6, RQ9 "
            "and RQ10 are therefore out of scope for this campaign and must not be "
            "reported as answered."),
        "binary": str(BINARY),
        "scenario_source": "ns3/scenarios/scr-manet.cc",
        "geometry": "60 nodes, 700x700 m (frozen: R007, D005)",
        "ns3_version": "3.46.1",
        "ns3_commit": "51387bce7e5f5c57aa080612a4ed690bf33d0c92",
        "seeds": seeds,
        "seed_policy": "Held-out seeds 201+. Development seeds 101-105 are never reused.",
        "n_per_condition": args.n,
        "rq_coverage": {"RQ3": "full", "RQ4": "full", "RQ7": "partial (C2 half)",
                        "RQ8": "not included; parameter sweep is a separate manifest",
                        "RQ1/RQ2/RQ5/RQ6/RQ9/RQ10": "OUT OF SCOPE - scenario not implemented"},
        "n_selection": {
            "rule": "smallest n in {10,20,30} with 1.96*s_d/sqrt(n) <= 0.02 on timely_pdr",
            "source": "stage7_pilot_v2, 40 runs, development seeds 101-105",
            "per_comparison_exact_n": {"TIME-BUCKET": 4, "SCR": 8, "OLSR": 9, "DSDV": 10,
                                       "PERSISTENT-BACKOFF": 13, "RREP-RESET": 17,
                                       "AODV-FB": 32},
            "selected": 30,
            "known_shortfall": ("AODV-FB vs AODV-STOCK needs n=32 (s_d=0.05756); at n=30 "
                                "the half-width is 0.0206 against the 0.02 target, a 3% "
                                "overshoot. n=30 is the preregistered grid maximum and was "
                                "kept rather than enlarged."),
        },
        "freshness_rule": ("F = max(2 s, (K-1)*M*I + 1 s), K=2, M=10, derived from ratePps "
                           "and applied to every method (R022)."),
        "packet_events": "enabled on every run (R024)",
        "runs": ordered,
    }

    out = pathlib.Path(args.out)
    out.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    from collections import Counter
    per_rq = Counter()
    for r in ordered:
        for q in r["meta"]["rq"]:
            per_rq[q] += 1
    print("wrote %s" % out)
    print("  unique runs after deduplication : %d" % len(ordered))
    print("  seeds per condition             : %d" % args.n)
    for q in sorted(per_rq):
        print("  %-5s serves %d runs" % (q, per_rq[q]))


if __name__ == "__main__":
    main()
