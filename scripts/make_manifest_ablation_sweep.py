"""Stage 9: the five ablations in the six conditions Stage 8 did not cover.

Stage 8 scheduled the ablations only at the nominal condition and its load
variants -- `make_manifest_campaign.py` records this as "RQ7 ... PARTIAL (C2
half only)". That was a deliberate scoping decision, but Section 6.5 of the
manuscript then promised readings the design cannot supply: whether the node cap
matters at 16 flows, and whether the probe bucket weakens at 5 pkt/s. This
manifest closes that gap.

It does NOT regenerate anything: it imports make_run from the Stage 8 generator,
so run identity, the preregistered freshness rule (R022) and the held-out seed
range are the same function that produced the published runs, not a
reimplementation that could drift from it.

Kept as a separate campaign so the completed Stage 8 manifest -- which produced
results already written into the paper -- is not mutated.

  6 conditions x 5 ablations x 30 seeds = 900 runs.

The SCR arm each ablation is paired against already exists in Stage 8 for every
one of these conditions, so no reference runs are needed.
"""

import importlib.util
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location(
    "gen", ROOT / "scripts" / "make_manifest_campaign.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

# The six conditions that carry the eight principal arms but no ablation.
MISSING = [
    {"speed": 1.0,  "ratePps": 20.0, "numFlows": 8},
    {"speed": 10.0, "ratePps": 20.0, "numFlows": 8},
    {"speed": 15.0, "ratePps": 20.0, "numFlows": 8},
    {"speed": 5.0,  "ratePps": 5.0,  "numFlows": 8},
    {"speed": 5.0,  "ratePps": 20.0, "numFlows": 2},
    {"speed": 5.0,  "ratePps": 20.0, "numFlows": 16},
]


def main():
    stage8 = json.loads((ROOT / "configs" / "stage8_campaign.json").read_text())
    existing = {r["run_id"] for r in stage8["runs"]}

    runs, dupes = [], 0
    for cond in MISSING:                      # condition-major (R036)
        for algo in gen.ABLATIONS:
            for seed in gen.CAMPAIGN_SEEDS_ALL:
                rid, row = gen.make_run(algo, seed, cond, ["RQ7"])
                if rid in existing:      # must never re-run a published run
                    dupes += 1
                    continue
                runs.append(row)

    # Inherit Stage 8's provenance header verbatim -- same binary, same ns-3
    # commit, same geometry, same seed policy, same freshness rule -- and
    # override only what actually differs. The runner requires campaign_id and
    # binary; the rest is the provenance the analysis and the audit read.
    man = {k: v for k, v in stage8.items() if k != "runs"}
    man["campaign_id"] = "stage9_ablation_sweep"
    man["purpose"] = ("Ablation arms in the six Stage 8 conditions that carried only the "
                      "eight principal arms. Closes the RQ7 'PARTIAL (C2 half)' scope noted "
                      "in the Stage 8 generator and promised against in manuscript S6.5.")
    man["scope_limitation"] = ("Ablations only. The eight principal arms and their SCR "
                               "reference already exist in stage8_campaign for every one of "
                               "these conditions and are NOT re-run; pairing is against those.")
    man["rq_coverage"] = {"RQ7": "completes the C2 half left partial by stage8_campaign"}
    man["execution_order"] = ("condition-major (R036): finishing conditions one at a time "
                              "yields fully populated cells progressively, so Section 6 can "
                              "be sharpened before the campaign ends.")
    man["derived_from"] = "stage8_campaign; run identity via make_manifest_campaign.make_run"
    man["runs"] = runs

    out = ROOT / "configs" / "stage9_ablation_sweep.json"
    out.write_text(json.dumps(man, indent=1))

    print("conditions      : %d" % len(MISSING))
    print("ablation arms   : %d  %s" % (len(gen.ABLATIONS), gen.ABLATIONS))
    print("seeds per cell  : %d" % len(gen.CAMPAIGN_SEEDS_ALL))
    print("runs written    : %d" % len(runs))
    print("skipped as dupes: %d" % dupes)
    print("unique run_ids  : %d" % len({r["run_id"] for r in runs}))
    print("collides w/ st8 : %d" % len({r["run_id"] for r in runs} & existing))
    print("-> %s" % out)

    # Freshness must follow the preregistered rule, not the nominal, at 5 pkt/s.
    for r in runs:
        exp = gen.freshness_for(float(r["meta"]["ratePps"]))
        assert abs(r["meta"]["freshness_s"] - exp) < 1e-9, r["run_id"]
    print("freshness rule  : verified on all %d runs" % len(runs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
