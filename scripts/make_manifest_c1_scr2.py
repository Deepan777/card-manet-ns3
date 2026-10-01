"""C1 supplement: SCR-2 (no per-pair allowance) at fork_rev 5.

Why this exists. The manuscript's mechanism argument includes the explanation
"SCR fails because its limiter is too strict". The only direct test of that is
an arm with the allowance removed entirely: SCR-2. The draft quoted SCR-2 from
an exploratory run at fork_rev 4 (n=6 of 10, C1 healthy only), a build later
found to carry the post-reply cooldown defect (R032) - so the quoted value
(0.128) measured the defect, not the mechanism. This campaign re-measures SCR-2
on the corrected build across all six C1 conditions at the campaign's n=10.

Run identity, seeds and fault conditions come from importing make_run from the
C1 generator, so every SCR-2 run pairs with the published C1 SCR and
AODV-STOCK runs of the same seed and condition. Kept as its own campaign so the
completed C1 manifest is not mutated.

SCR-2 (ALGO_SCR2, repair-ledger.cc): no allowance and no cooldown; a fresh
confirmation acts as a brief hold-off and resets the search-reach index. It is
a declared supplementary arm, reported as such, not one of the four renewal
policies.

  6 conditions x 10 seeds = 60 runs.
"""

import importlib.util
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("c1gen", ROOT / "scripts" / "make_manifest_c1.py")
c1gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c1gen)

ALGO = "SCR2"
N = 10


def main():
    c1 = json.loads((ROOT / "configs" / "c1_outage.json").read_text())
    existing = {r["run_id"] for r in c1["runs"]}
    seeds = c1gen.CAMPAIGN_SEEDS_ALL[:N]
    assert seeds == c1["seeds"], "C1 seed set changed; pairing would break"

    runs = []
    for label, fault, stagger in c1gen.CONDITIONS:
        for s in seeds:
            _, rec = c1gen.make_run(ALGO, s, label, fault, stagger)
            assert rec["run_id"] not in existing
            runs.append(rec)

    man = {k: v for k, v in c1.items() if k != "runs"}
    man["campaign_id"] = "c1_scr2_supplement"
    man["purpose"] = ("Declared supplementary arm: SCR-2 (allowance removed) on C1 at "
                      "fork_rev 5, to test whether SCR's failure is explained by limiter "
                      "strictness. Replaces an exploratory fork_rev-4 measurement (R031/R032).")
    man["scope_limitation"] = ("One arm only. Pairs with c1_outage SCR and AODV-STOCK runs "
                               "of the same seed and condition; those are not re-run.")
    man["derived_from"] = "c1_outage; run identity via make_manifest_c1.make_run"
    man["runs"] = runs
    out = ROOT / "configs" / "c1_scr2_supplement.json"
    out.write_text(json.dumps(man, indent=1))
    print("runs %d, unique %d, collisions with c1_outage 0, seeds %s -> %s"
          % (len(runs), len({r["run_id"] for r in runs}), seeds, out.name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
