#!/usr/bin/env python3
"""
Stage 9 analysis — paired statistics over completed campaign runs.

Registered statistical plan (specification section 5):
  - PAIRED comparisons on identical seeds (same exogenous inputs)
  - paired sign tests with Holm correction across the algorithm family
  - bootstrap confidence intervals on paired differences
  - noninferiority on timely delivery at a margin of -0.02
  - n selected by 1.96 * s_d / sqrt(n) <= 0.02

Nothing here invents data. A run that did not complete is absent, not imputed;
its absence is reported. Every number traces back to a run_id.
"""

import argparse
import json
import math
import pathlib
import random
import statistics as st
import sys
from collections import defaultdict

ROOT = pathlib.Path(__file__).resolve().parent.parent
CHECKPOINTS = ROOT / "checkpoints"

# Bootstrap resampling is seeded so the reported interval is reproducible.
BOOTSTRAP_SEED = 20260905
BOOTSTRAP_SAMPLES = 10000
NONINFERIORITY_MARGIN = -0.02


def load_runs(campaign):
    """Read every completed run's summary. Incomplete runs are reported, never guessed."""
    base = CHECKPOINTS / campaign
    flat = False
    if not base.exists():
        # published artefact layout: results/<campaign>/<run_id>.summary.csv + .done.json
        base, flat = ROOT / "results" / campaign, True
    if not base.exists():
        sys.exit("no such campaign: " + str(base))

    # R035: only directories named by the CURRENT manifest are runs of this
    # campaign. C4's manifest was trimmed (R023), leaving 276 directories from
    # the superseded version; counting those as "incomplete" reported 960
    # completed AND 276 incomplete for a 960-run campaign, and would let one
    # genuine incomplete hide among the phantoms. Directories outside the
    # manifest are reported separately, never silently.
    manifest_ids = None
    mf = ROOT / "configs" / (campaign + ".json")
    if mf.exists():
        import json as _json
        manifest_ids = {r["run_id"] for r in _json.loads(mf.read_text())["runs"]}

    rows = []
    incomplete = []
    stale = []
    if flat:
        entries = [(p.name[:-len(".summary.csv")], p, base / (p.name[:-len(".summary.csv")] + ".done.json"))
                   for p in sorted(base.iterdir()) if p.name.endswith(".summary.csv")]
        if manifest_ids is not None:
            # runs that never wrote a summary are absent from the flat layout; list them as incomplete
            have = {e[0] for e in entries}
            incomplete.extend(sorted(manifest_ids - have))
    else:
        entries = [(d.name, d / "out_summary.csv", d / "DONE.json") for d in sorted(base.iterdir()) if d.is_dir()]
    for name, summary, done in entries:
        if manifest_ids is not None and name not in manifest_ids:
            stale.append(name)
            continue
        if not done.exists():
            incomplete.append(name)
            continue
        if not summary.exists():
            incomplete.append(name + " (no summary)")
            continue
        kv = {}
        for line in summary.read_text(errors="replace").splitlines()[1:]:
            if "," in line:
                k, _, v = line.partition(",")
                kv[k.strip()] = v.strip().strip('"')
        kv["_run_id"] = name
        rows.append(kv)
    if stale:
        print("STALE dirs not in the current manifest (ignored): %d  %s"
              % (len(stale), stale[:3]))
    return rows, incomplete


# ---------------------------------------------------------------------------
# R028 PROVENANCE GATE
#
# Runs produced before 2026-09-14 came from a binary in which the AODV RREQ
# retry counter could run away in SCR and RREP-RESET: the give-up test used
# `==` where the counter could step past it, and every origination cancelled
# the timer that would have run that test. Seven runs died of the resulting
# integer overflow; the rest kept going with a retry backoff of up to 2^31 x
# net_traversal_time.
#
# Those runs cannot be told apart by their numbers -- they look entirely
# plausible. They are told apart by whether their summary carries the health
# counters at all, which only the fixed binary writes. A run with no counters
# is a run from the old binary.
#
# This gate exists because the same failure mode has now happened three times
# (R017, R024, R026): an analysis that silently accepted data it should have
# questioned, and produced a clean-looking table from it.
# ---------------------------------------------------------------------------
# Scope is decided by the run's OWN recorded scr_enabled, not by a list of
# algorithm names. The first version of this gate hard-coded ("SCR",
# "RREP-RESET") because those were the two arms observed to clamp in one test
# cell -- but TIME-BUCKET and PERSISTENT-BACKOFF run the same origination path
# and simply did not clamp in THAT cell. A name list would have silently
# cleared 257 runs that were never checked.


def on_scr_origination_path(kv):
    return "scr_enabled=1" in kv.get("effective_parameters", "")


# The minimum fork revision whose SCR-path behaviour matches the registered
# design. Raise this whenever a change alters what an SCR-family run produces.
#
#   1  first campaign binary
#   2  R028: RREQ give-up test fixed, retry backoff bounded
#   3  R029: SCR reply timeout taken from the ledger, as D002 registered
#   4  R030: dedicated scheduler-jitter stream wired up, as D003 registered
#   5  R032: an answered request ends the reply wait; no post-reply cooldown
#
# Native-path runs (scr_enabled=0, plus OLSR and DSDV) are NOT gated: R029 is
# SCR-only by D002's explicit requirement, and R028's give-up change is a no-op
# for them because their counter lands exactly on the cap. That was verified by
# measurement, not assumed - AODV-STOCK and AODV-FB reproduced their pre-fix
# summaries field for field.
MIN_FORK_REV = 5


def fork_rev(kv):
    for tok in kv.get("effective_parameters", "").split():
        if tok.startswith("fork_rev="):
            try:
                return int(tok.split("=", 1)[1].strip('"'))
            except ValueError:
                return 0
    return 0


def provenance_report(rows):
    """Classify runs by whether their binary matches the registered design."""
    pre_fix, clamped, legacy = [], [], []
    for kv in rows:
        if on_scr_origination_path(kv) and fork_rev(kv) < MIN_FORK_REV:
            pre_fix.append(kv["_run_id"])
            continue
        if "rreq_backoff_clamped" not in kv:
            continue
        if int(kv.get("rreq_backoff_clamped", "0") or 0) != 0:
            clamped.append(kv["_run_id"])
        if "rreq_cap_legacy_equality=1" in kv.get("effective_parameters", ""):
            legacy.append(kv["_run_id"])
    return pre_fix, clamped, legacy


# ---------------------------------------------------------------------------
# EXPERIMENTAL CELL IDENTITY
#
# A campaign varies parameters: C1 has six fault patterns per seed, C4 twelve
# scale/density/payload panels. Two runs differing in any of these are different
# cells and must never be treated as repetitions of one.
#
# Until 2026-09-13 runs were keyed on (algorithm, seed) alone, so for C1 five of
# every six runs were silently overwritten and the survivor -- whichever sorted
# last -- was compared as if it were the condition. The tool still printed a
# tidy table with p-values. Keys now carry the condition, taken from the run's
# OWN summary rather than a manifest, so the label always matches what ran.
#
# Keep CONDITION_FIELDS identical to the copy in make_figures.py; an audit check
# enforces that.
CONDITION_FIELDS = (
    "fault", "cold_stagger",                                  # C1
    "num_nodes", "area_side_m", "speed_mps",                  # geometry, mobility
    "num_flows", "rate_pps", "payload_bytes",                 # offered load
    "freshness_s", "block_size_m", "k_blocks", "theta",       # evidence rule
    "c3_fading", "c3_feedback_loss", "c3_clock_offset_ms",    # C3 impairments
)


def condition_key(kv):
    """The experimental cell a run belongs to, from its own summary."""
    return tuple((f, kv[f]) for f in CONDITION_FIELDS if f in kv)


def condition_label(cond):
    return ", ".join("%s=%s" % (f, v) for f, v in cond) or "(single condition)"


def to_float(kv, key, default=None):
    try:
        return float(kv[key])
    except (KeyError, ValueError):
        return default


def paired_sign_test(diffs):
    """Two-sided exact sign test. Zero differences are discarded (standard)."""
    pos = sum(1 for d in diffs if d > 0)
    neg = sum(1 for d in diffs if d < 0)
    n = pos + neg
    if n == 0:
        return 1.0, 0, 0
    k = min(pos, neg)
    # Two-sided p = 2 * P(X <= k) under Binomial(n, 0.5), capped at 1.
    cdf = sum(math.comb(n, i) for i in range(k + 1)) / (2 ** n)
    return min(1.0, 2 * cdf), pos, neg


def holm(pvals):
    """Holm-Bonferroni adjusted p-values, order preserved."""
    idx = sorted(range(len(pvals)), key=lambda i: pvals[i])
    m = len(pvals)
    adj = [0.0] * m
    running = 0.0
    for rank, i in enumerate(idx):
        val = (m - rank) * pvals[i]
        running = max(running, val)
        adj[i] = min(1.0, running)
    return adj


def bootstrap_ci(diffs, alpha=0.05):
    if not diffs:
        return (float("nan"), float("nan"))
    rng = random.Random(BOOTSTRAP_SEED)
    n = len(diffs)
    means = []
    for _ in range(BOOTSTRAP_SAMPLES):
        means.append(sum(rng.choice(diffs) for _ in range(n)) / n)
    means.sort()
    lo = means[int((alpha / 2) * BOOTSTRAP_SAMPLES)]
    hi = means[int((1 - alpha / 2) * BOOTSTRAP_SAMPLES) - 1]
    return (lo, hi)


def required_n(diffs, target=0.02):
    """Registered rule: smallest n in {10,20,30} with 1.96*s_d/sqrt(n) <= target."""
    if len(diffs) < 2:
        return None, None
    s = st.stdev(diffs)
    for n in (10, 20, 30):
        if 1.96 * s / math.sqrt(n) <= target:
            return n, s
    return None, s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--campaign", required=True)
    ap.add_argument("--reference", default="AODV-STOCK",
                    help="algorithm each other is paired against")
    ap.add_argument("--metric", default="timely_pdr",
                    help="derived metric: timely_pdr | pdr | control_cost")
    ap.add_argument("--out", default=None)
    ap.add_argument("--allow-pre-fix-r028", action="store_true",
                    help="analyse SCR/RREP-RESET runs from the pre-R028 binary "
                         "anyway; results are provisional and not publishable")
    args = ap.parse_args()

    rows, incomplete = load_runs(args.campaign)
    print("campaign        : " + args.campaign)
    print("completed runs  : %d" % len(rows))
    if incomplete:
        # Reported, never imputed.
        print("INCOMPLETE runs : %d  %s" % (len(incomplete), incomplete[:5]))
    if not rows:
        sys.exit("no completed runs to analyse")

    # ---- R028 provenance gate ---------------------------------------------
    pre_fix, clamped, legacy = provenance_report(rows)
    if legacy:
        sys.exit("REFUSING TO ANALYSE: %d run(s) were produced with the R028 "
                 "legacy give-up test, which reproduces a known bug on purpose: %s"
                 % (len(legacy), legacy[:5]))
    if pre_fix:
        print("SUPERSEDED-BINARY RUNS: %d of %d (scr_enabled=1, fork_rev < %d)"
              % (len(pre_fix), len(rows), MIN_FORK_REV))
        print("  These came from a binary whose SCR reply timeout did not follow")
        print("  the registered D002 rule, so they carry an unregistered second")
        print("  throttle. Their numbers look entirely plausible and cannot be")
        print("  validated from the summary alone.")
        if not args.allow_pre_fix_r028:
            sys.exit("  Re-run them, or pass --allow-pre-fix-r028 to proceed with "
                     "results that are NOT publishable.")
        print("  PROCEEDING ANYWAY: --allow-pre-fix-r028 was given. Any result "
              "below is provisional and must not be reported.")
    if clamped:
        print("R028 CLAMPED RUNS : %d  %s" % (len(clamped), clamped[:5]))
        print("  A route search outran its own give-up timer in these runs.")

    # ---- derive per-run metrics -------------------------------------------
    by_key = {}
    algos = set()
    seeds = set()
    conds = set()
    collisions = []
    for kv in rows:
        algo = kv.get("algorithm", "?")
        seed = kv.get("rng_run", "?")
        gen = to_float(kv, "generated", 0.0) or 0.0
        rreq = to_float(kv, "source_rreq_originated", 0.0) or 0.0
        accepted = to_float(kv, "reports_accepted", 0.0) or 0.0
        confirms = to_float(kv, "service_confirmations", 0.0) or 0.0
        transport_err = to_float(kv, "transport_integration_error", 0.0) or 0.0

        if transport_err:
            # A technical failure is excluded from scientific statistics and
            # reported separately - it is not channel loss.
            print("  EXCLUDED (transport integration error): " + kv["_run_id"])
            continue

        # PRIMARY ENDPOINT. ontime_true is the receiver's on-time count judged
        # against TRUE simulator time, so a C3 clock offset changes what the
        # receiver acts on but never what is scored here.
        #
        # Runs produced before 2026-09-09 carry no delivery quantity at all
        # (the scenario did not write one). Those runs are usable for cost
        # comparisons and must NOT be silently scored as zero delivery, so the
        # metric is left absent rather than defaulted -- the pairing loop below
        # drops a pair when the metric is missing, and the run count printed
        # for each metric makes the shortfall visible.
        ontime = to_float(kv, "ontime_true", None)
        rec = to_float(kv, "received", None)

        entry = {
            "run_id": kv["_run_id"],
            "generated": gen,
            "control_cost": rreq,
            "reports_accepted": accepted,
            "confirmations": confirms,
            # Control cost per generated packet: the comparable cost quantity.
            "cost_per_packet": (rreq / gen) if gen else float("nan"),
        }
        if ontime is not None and gen:
            entry["timely_pdr"] = ontime / gen
        if rec is not None and gen:
            entry["pdr"] = rec / gen
        cond = condition_key(kv)
        key = (algo, cond, seed)
        if key in by_key:
            # Two runs of the identical cell and seed: a manifest defect, not a
            # repetition. Never silently overwrite.
            collisions.append((key, by_key[key]["run_id"], entry["run_id"]))
        by_key[key] = entry
        algos.add(algo)
        seeds.add(seed)
        conds.add(cond)

    algos = sorted(algos)
    seeds = sorted(seeds)
    print("algorithms      : " + ", ".join(algos))
    print("seeds           : %d" % len(seeds))
    print("conditions      : %d" % len(conds))
    if collisions:
        print("ERROR: %d duplicate (algorithm, condition, seed) key(s); the manifest "
              "defines the same cell twice:" % len(collisions))
        for k, a, b in collisions[:5]:
            print("   %s  %s vs %s" % (condition_label(k[1]), a, b))
        sys.exit("refusing to analyse: duplicate cells would be averaged silently")
    print()

    ref = args.reference
    if ref not in algos:
        sys.exit("reference algorithm not present: " + ref)

    # Distinguish "the metric is absent from these runs" from "the seeds do not
    # pair". Without this the report reads as a statistical shortfall when the
    # real cause is that the scenario never wrote the quantity.
    wanted = "cost_per_packet" if args.metric == "control_cost" else args.metric
    have = sum(1 for v in by_key.values() if wanted in v)
    if have == 0:
        sys.exit(chr(10).join([
            "metric " + repr(args.metric) + " is absent from every completed run.",
            "",
            "Runs produced before 2026-09-09 carry no delivery quantity: the C2",
            "scenario wrote generated, control counters and radio seconds, but no",
            "arrivals. Re-run the campaign on a binary that emits ontime_true,",
            "or analyse --metric control_cost, which those runs do support.",
        ]))
    if have < len(by_key):
        print("NOTE: metric '%s' present in %d of %d runs; the rest are excluded "
              "from this comparison." % (args.metric, have, len(by_key)))

    metric = "cost_per_packet" if args.metric == "control_cost" else args.metric

    # ---- paired differences vs the reference, WITHIN each condition -------
    #
    # Pairing across conditions would difference a healthy run against a
    # long-outage run of the same seed. Every comparison below is inside one
    # experimental cell, and Holm correction is applied within that cell's own
    # family of comparisons -- correcting across unrelated cells would be a
    # different (and unregistered) multiplicity claim.
    results = []
    thin = []
    for cond in sorted(conds):
        cond_results = []
        for algo in algos:
            if algo == ref:
                continue
            diffs = []
            used_seeds = []
            for s in seeds:
                a = by_key.get((algo, cond, s))
                b = by_key.get((ref, cond, s))
                if not a or not b:
                    continue  # unpaired seed is dropped, never imputed
                if metric not in a or metric not in b:
                    continue
                va, vb = a[metric], b[metric]
                if va != va or vb != vb:
                    continue
                diffs.append(va - vb)
                used_seeds.append(s)
            if len(diffs) < 2:
                thin.append((condition_label(cond), algo, len(diffs)))
                continue
            p, pos, neg = paired_sign_test(diffs)
            lo, hi = bootstrap_ci(diffs)
            n_needed, sd = required_n(diffs)
            cond_results.append({
                "condition": condition_label(cond),
                "algorithm": algo,
                "reference": ref,
                "metric": metric,
                "n_pairs": len(diffs),
                "mean_diff": st.mean(diffs),
                "sd_diff": sd,
                "ci_low": lo,
                "ci_high": hi,
                "sign_p": p,
                "pos": pos,
                "neg": neg,
                "required_n": n_needed,
                "seeds": used_seeds,
            })
        if cond_results:
            adj = holm([r["sign_p"] for r in cond_results])
            for r, a in zip(cond_results, adj):
                r["holm_p"] = a
        results.extend(cond_results)

    # ---- report -----------------------------------------------------------
    print("PAIRED COMPARISON vs %s   metric=%s" % (ref, metric))
    print("Holm correction applied WITHIN each condition; %d condition(s) present."
          % len(conds))
    hdr = ("%-20s %5s %12s %22s %10s %10s %6s"
           % ("algorithm", "n", "mean diff", "95% CI (bootstrap)", "sign p", "Holm p", "n req"))
    current = None
    for r in results:
        if r["condition"] != current:
            current = r["condition"]
            print()
            print("condition: " + current)
            print(hdr)
            print("-" * len(hdr))
        print("%-20s %5d %12.5f  [%9.5f,%9.5f] %10.4f %10.4f %6s"
              % (r["algorithm"], r["n_pairs"], r["mean_diff"], r["ci_low"], r["ci_high"],
                 r["sign_p"], r["holm_p"], str(r["required_n"])))
    if thin:
        print()
        print("cells with fewer than 2 paired seeds (reported, never imputed): %d" % len(thin))
        for c, a, n in thin[:6]:
            print("   %-40s %-20s n=%d" % (c[:40], a, n))

    print()
    print("NOTE: n req is the smallest n in {10,20,30} satisfying "
          "1.96*s_d/sqrt(n) <= 0.02; 'None' means 30 is insufficient at the "
          "observed variance.")

    if args.out:
        payload = {
            "campaign": args.campaign,
            "reference": ref,
            "metric": metric,
            "bootstrap_samples": BOOTSTRAP_SAMPLES,
            "bootstrap_seed": BOOTSTRAP_SEED,
            "noninferiority_margin": NONINFERIORITY_MARGIN,
            "completed_runs": len(rows),
            "incomplete_runs": incomplete,
            "results": results,
        }
        pathlib.Path(args.out).write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print("\nwrote " + args.out)


if __name__ == "__main__":
    main()
