#!/usr/bin/env python3
"""
C4 manifest — traffic and scaling families (RQ6, and the rate/flow/payload
one-factor families of RQ4).

C4 is not a new scenario. The specification defines it as the C2 topology and
traffic family under different parameters, so it runs on the C2 binary with
different arguments. That is deliberate: a second scenario source would be a
second place for the topology to drift, and RQ6 compares scale against a fixed
reference that must be the *same* code.

FAMILIES (one factor at a time; the nominal cell is shared and executed once)

  scale families, from the specification:
    N in {30, 60, 90, 120}
      density panel : area fixed          -> nodes/m^2 rises with N
      scale panel   : side * sqrt(N/60)   -> nodes/m^2 held at the nominal
    flows:
      fixed panel        : 8 flows at every N
      proportional panel : max(2, round(N*8/60)), capped at floor(N/2)

  one-factor traffic families:
    payload {128, 512, 1024} bytes

  The rate {5,20,40} and flows {2,8,16} families were REMOVED on 2026-09-10:
  Stage 8 runs the identical simulations at n=30, so executing them here would
  reproduce a strict subset of another campaign at lower statistical power.
  See R023.

PREREGISTERED FRESHNESS RULE (specification, C4)

  At 5 pkt/s the fixed M/K block rule cannot close a block inside the nominal
  F=2 s, so evidence would be structurally impossible to qualify rather than
  merely slower. The rule, fixed in advance and applied to EVERY method that
  uses feedback:

      F = max(2 s, (K-1)*M*I + 1 s)

  With K=2, M=10, I=1/rate: at 5 pkt/s that is max(2, 10*0.2+1) = 3 s.
  This is not an algorithm-specific tuning concession -- it is applied
  identically to SCR, RREP-RESET, TIME-BUCKET, PERSISTENT-BACKOFF and AODV-FB,
  and it is recorded in each run's summary as freshness_s so the applied value
  can be audited rather than trusted.

GEOMETRY NOTE

  The specification's C2 text says 1000x1000 m. This study froze 700x700 m
  (R007, D005) on a measured connectivity criterion decided before any SCR code
  existed. C4 scales from the FROZEN side, not the specification's, so the
  density panel's reference cell is the same 60-node cell the rest of the study
  uses. Deviating here would make RQ6 incomparable with RQ3/RQ4.
"""

import argparse
import hashlib
import json
import math
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BINARY = (ROOT / "vendor/ns-3-dev-ns-3.46.1/build/scratch/scr-manet/"
          "ns3.46.1-scr-manet-default.exe")

# RQ6 compares scale and density; the full eight-algorithm set is not required
# by the specification for component studies, but the four that differ in
# discovery control plus the two native references are.
CORE = ["SCR", "RREP-RESET", "TIME-BUCKET", "PERSISTENT-BACKOFF",
        "AODV-FB", "AODV-STOCK", "OLSR", "DSDV"]

SEED_BASE = 20260905
CAMPAIGN_SEEDS_ALL = list(range(201, 231))

# Frozen C2 nominal cell.
NOMINAL_NODES = 60
NOMINAL_SIDE = 700.0        # FROZEN: R007, D005
NOMINAL_FLOWS = 8
NOMINAL_RATE = 20.0
NOMINAL_PAYLOAD = 512
NOMINAL_SPEED = 5.0

# Evidence block rule constants (E2), needed for the freshness rule.
K_BLOCKS = 2
BLOCK_M = 10
NOMINAL_FRESHNESS = 2.0

FEEDBACK_ALGORITHMS = {"SCR", "RREP-RESET", "TIME-BUCKET",
                       "PERSISTENT-BACKOFF", "AODV-FB"}


def freshness_for(rate_pps):
    """Preregistered rule F = max(2 s, (K-1)*M*I + 1 s). Applied to all methods."""
    interval = 1.0 / rate_pps
    return max(NOMINAL_FRESHNESS, (K_BLOCKS - 1) * BLOCK_M * interval + 1.0)


def flows_proportional(n):
    """max(2, round(N*8/60)) capped at floor(N/2): never reuse an endpoint."""
    return min(max(2, round(n * NOMINAL_FLOWS / NOMINAL_NODES)), n // 2)


def side_for(n, mode):
    """density: fixed area. scale: constant nominal density."""
    if mode == "density":
        return NOMINAL_SIDE
    return NOMINAL_SIDE * math.sqrt(n / NOMINAL_NODES)


def make_run(algo, seed, params, panel):
    key = "c4|%s|seed=%d|%s" % (
        algo, seed, "|".join("%s=%s" % (k, params[k]) for k in sorted(params)))
    rid = "c4_%s_s%d_%s" % (algo.replace("-", ""), seed,
                            hashlib.sha256(key.encode()).hexdigest()[:8])
    args = ["--algorithm=%s" % algo,
            "--rngRun=%d" % seed,
            "--rngSeed=%d" % SEED_BASE,
            "--numNodes=%d" % params["numNodes"],
            "--areaSide=%.6f" % params["areaSide"],
            "--speed=%s" % params["speed"],
            "--numFlows=%d" % params["numFlows"],
            "--ratePps=%s" % params["ratePps"],
            "--payloadBytes=%d" % params["payloadBytes"],
            "--freshnessS=%.6f" % params["freshnessS"],
            "--outPrefix=out"]
    return rid, {
        "run_id": rid,
        "args": args,
        # Larger N costs superlinearly more; 90 and 120 nodes at 40 pkt/s are
        # the expensive corner. 7200 s is generous rather than tuned.
        "timeout_s": 7200,
        "meta": {"algorithm": algo, "rng_run": seed, "panel": [panel], **params},
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=20,
                    help="seeds per condition; set from the pilot n-selection rule")
    ap.add_argument("--out", default=str(ROOT / "configs" / "c4_scaling.json"))
    args = ap.parse_args()

    seeds = CAMPAIGN_SEEDS_ALL[:args.n]
    runs, panels = {}, {}

    def add(algo, seed, overrides, panel):
        params = {"numNodes": NOMINAL_NODES, "areaSide": NOMINAL_SIDE,
                  "speed": NOMINAL_SPEED, "numFlows": NOMINAL_FLOWS,
                  "ratePps": NOMINAL_RATE, "payloadBytes": NOMINAL_PAYLOAD,
                  "freshnessS": NOMINAL_FRESHNESS}
        params.update(overrides)
        # The freshness rule is derived, never passed in: it must follow from
        # the rate for every run, so no cell can quietly get a different F.
        params["freshnessS"] = freshness_for(params["ratePps"])
        rid, rec = make_run(algo, seed, params, panel)
        if rid in runs:
            panels[rid].add(panel)
        else:
            runs[rid] = rec
            panels[rid] = {panel}

    for algo in CORE:
        for s in seeds:
            # --- RQ6 scale/density, 8 flows held fixed ---
            for n in (30, 60, 90, 120):
                add(algo, s, {"numNodes": n, "areaSide": side_for(n, "density"),
                              "numFlows": min(NOMINAL_FLOWS, n // 2)}, "density_fixedflows")
                add(algo, s, {"numNodes": n, "areaSide": side_for(n, "scale"),
                              "numFlows": min(NOMINAL_FLOWS, n // 2)}, "scale_fixedflows")

            # --- RQ6 offered-load scaling, flows proportional to N ---
            for n in (30, 60, 90, 120):
                add(algo, s, {"numNodes": n, "areaSide": side_for(n, "scale"),
                              "numFlows": flows_proportional(n)}, "scale_propflows")

            # --- one-factor traffic families at the nominal cell ---
            #
            # TRIMMED 2026-09-10 (R023). The rate and flow families were removed
            # because Stage 8 executes the IDENTICAL simulations: same 60-node
            # 700 m geometry, same nominal speed, same rate and flow values, and
            # -- since R022 gave Stage 8 the preregistered freshness rule -- the
            # same derived F. Verified by normalising both manifests to their
            # effective simulation parameters (defaults filled in): 240 of 240
            # rate_family runs and 240 of 240 flow_family runs were already
            # present in Stage 8, which covers them at n=30 rather than n=10.
            #
            # Running them here would have spent roughly 40 hours of wall clock
            # reproducing a strict subset of another campaign at lower power.
            # RQ4 is answered by Stage 8; C4's own contribution is scale,
            # density and payload.
            for pb in (128, 512, 1024):
                add(algo, s, {"payloadBytes": pb}, "payload_family")

    for rid, ps in panels.items():
        runs[rid]["meta"]["panel"] = sorted(ps)

    ordered = [runs[k] for k in sorted(runs)]
    manifest = {
        "campaign_id": "c4_scaling",
        "purpose": "C4 traffic and scaling families over the frozen C2 scenario.",
        "scope_limitation": (
            "C4 answers RQ6 and the rate/flow/payload one-factor families. It does "
            "NOT cover RQ1, RQ2, RQ5, RQ10 (C1) or RQ9 (C3)."),
        "binary": str(BINARY),
        "scenario_source": "ns3/scenarios/scr-manet.cc",
        "geometry": ("scaled from the FROZEN 60-node 700x700 m cell (R007, D005); "
                     "density panel holds area, scale panel holds nodes/m^2"),
        "freshness_rule": "F = max(2 s, (K-1)*M*I + 1 s), K=2, M=10; applied to every method",
        "ns3_version": "3.46.1",
        "ns3_commit": "51387bce7e5f5c57aa080612a4ed690bf33d0c92",
        "seeds": seeds,
        "n_per_condition": args.n,
        "runs": ordered,
    }

    out = pathlib.Path(args.out)
    out.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    from collections import Counter
    per_panel = Counter()
    for r in ordered:
        for p in r["meta"]["panel"]:
            per_panel[p] += 1
    print("wrote %s" % out)
    print("  unique runs after deduplication : %d" % len(ordered))
    print("  seeds per condition             : %d" % args.n)
    for p in sorted(per_panel):
        print("  %-22s serves %d runs" % (p, per_panel[p]))
    print()
    print("  freshness rule applied:")
    for r in (5.0, 20.0, 40.0):
        print("    %-5s pkt/s -> F = %.1f s" % (r, freshness_for(r)))
    print("  proportional flows:")
    for n in (30, 60, 90, 120):
        print("    N=%-4d -> %d flows (cap floor(N/2)=%d)"
              % (n, flows_proportional(n), n // 2))
    print("  scale-panel side:")
    for n in (30, 60, 90, 120):
        print("    N=%-4d -> %.1f m (density %.4f nodes/1000m^2)"
              % (n, side_for(n, "scale"),
                 1000.0 * n / (side_for(n, "scale") ** 2)))


if __name__ == "__main__":
    main()
