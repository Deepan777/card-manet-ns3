"""Revision: macros for the revised manuscript that the original pipeline does not produce.

1. Family-level 95% intervals (POST HOC, added in the revision) for the main paired comparisons.
   Estimand: the unweighted mean over the family's scenarios of the per-scenario mean paired difference
   in timely PDR (the same estimand as the paper's family "Delta"). Interval: stratified percentile
   bootstrap -- within each scenario, seeds (paired runs) are resampled with replacement; scenarios are
   fixed and equally weighted; 10,000 resamples, seed 20260929.
2. Evidence-timescale quantities of Section 3 (Proposition 2) from the controller parameters.
3. Run counts, composition direction counts, relative PDR changes and the classifier's run-clustered
   intervals (from paper/scripts/out/numerical_recompute.json).

Writes paper/tables/rev_numbers.tex and paper/scripts/out/rev_numbers.json.
Usage (from the project root): python paper/scripts/make_rev_numbers.py
"""

import json
import math
import pathlib
import sys

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "analysis"))
from q1_common import conditions, load, load_card, paired  # noqa: E402

REV = ROOT / "paper"
FAM = [("C1", "c1_outage", "Cone"), ("C4", "c4_scaling", "Cfour"), ("S8", "stage8_campaign", "Seight")]
TAG = {"AODV-STOCK": "Stock", "RREP-RESET": "RR", "CARD": "Card", "CLAF-AODV": "Claf", "TAAODV": "Taa",
       "CARD-NO-EXEMPT": "Noex", "CARD-NO-REACH": "Noreach", "CARD-NO-CAUSE": "Blind",
       "CARD-EXEMPT-ALL": "Exall", "CARD-CLAF": "Cc"}
PAIRS = [("CARD", "AODV-STOCK"), ("CARD", "RREP-RESET"), ("CARD", "CLAF-AODV"), ("CARD", "TAAODV"),
         ("CARD", "CARD-NO-EXEMPT"), ("CARD", "CARD-NO-REACH"), ("CARD", "CARD-NO-CAUSE"),
         ("CARD", "CARD-EXEMPT-ALL"), ("CARD-CLAF", "CLAF-AODV"), ("CARD-CLAF", "CARD"),
         ("CARD-CLAF", "AODV-STOCK"), ("CLAF-AODV", "AODV-STOCK"), ("TAAODV", "AODV-STOCK"),
         ("RREP-RESET", "AODV-STOCK")]
NBOOT, SEED = 10000, 20260929
M = {}


def put(k, v):
    M[k] = v


def s3(x):
    return ("$%+.3f$" % x)


def family_ci(rows, conds, a, b, rng):
    per = [np.array(paired(rows, a, b, c)) for c in conds]
    per = [d for d in per if len(d) >= 3]
    est = float(np.mean([d.mean() for d in per]))
    boots = np.zeros(NBOOT)
    for d in per:
        idx = rng.integers(0, len(d), size=(NBOOT, len(d)))
        boots += d[idx].mean(axis=1)
    boots /= len(per)
    lo, hi = np.percentile(boots, [2.5, 97.5])
    return est, float(lo), float(hi), len(per)


def main():
    rng = np.random.default_rng(SEED)
    J = {"family_ci": {}}
    for tag, camp, T in FAM:
        rows = load(camp)
        rows.update(load_card(camp))
        conds = [c for c in conditions(rows) if any(x == "CARD" and cc == c for (x, cc, _) in rows)]
        J["family_ci"][tag] = {}
        for a, b in PAIRS:
            est, lo, hi, n = family_ci(rows, conds, a, b, rng)
            J["family_ci"][tag]["%s - %s" % (a, b)] = {"est": est, "lo": lo, "hi": hi, "scenarios": n}
            k = "Rev%s%s%s" % (T, TAG[a], TAG[b])
            put(k + "D", s3(est))
            put(k + "Dabs", "%.3f" % abs(est))
            put(k + "Lo", s3(lo))
            put(k + "Hi", s3(hi))
            put(k + "CI", "[%s, %s]" % (s3(lo), s3(hi)))

    # Proposition 2: m = M - ceil(theta M) misses tolerated per block.
    K, Mb, theta, D = 2, 10, 0.8, 0.25
    m = Mb - math.ceil(theta * Mb - 1e-9)
    put("RevMiss", str(m))
    for rate, name in ((5, "Five"), (20, "Twenty"), (40, "Forty")):
        I = 1.0 / rate
        put("RevWmin%s" % name, "%.2f" % ((K * Mb - m - 1) * I))
        put("RevSmin%s" % name, "%.2f" % ((K * Mb - 2 * m - 1) * I - D))
        put("RevKMI%s" % name, "%.2f" % (K * Mb * I))
    J["timescale"] = {k: v for k, v in M.items() if k.startswith(("RevWmin", "RevSmin", "RevKMI", "RevMiss"))}

    # From the numerical recomputation (verify_numbers.py).
    N = json.loads((REV / "scripts" / "out" / "numerical_recompute.json").read_text())
    F = N["families"]
    for tag, _, T in FAM:
        for arm, A in F[tag]["arms"].items():
            t = TAG.get(arm)
            if not t:
                continue
            put("Rev%s%sBpool" % (T, t), "%.0f" % abs(A["bytes_rel_ratio_of_pooled_pct"]))
            put("Rev%s%sBpoolSign" % (T, t), "fewer" if A["bytes_rel_ratio_of_pooled_pct"] < 0 else "more")
            put("Rev%s%sTrelPct" % (T, t), "%.0f" % abs(A["timely_rel_ratio_of_pooled_pct"]))
            put("Rev%s%sTpp" % (T, t), "%.1f" % abs(A["timely_pooled_diff_pp"]))
    for tag, _, T in FAM:
        c = N["classifier"].get(tag)
        if not c:
            continue
        for key, nm in (("recall_departure", "Rec"), ("precision_topology", "PrecTopo"),
                        ("precision_transient", "PrecTrans")):
            v = c.get(key)
            if v:
                put("Rev%s%s" % (T, nm), "%.2f" % v[0])
                put("Rev%s%sCI" % (T, nm), "[%.2f, %.2f]" % (v[1], v[2]))
        put("Rev%sClassRuns" % T, str(c["runs"]))

    # Composition: direction of per-scenario mean differences (card_tables.json, unchanged analysis).
    C = json.loads((ROOT / "analysis_out" / "card_tables.json").read_text())["compose"]
    def npos(tag, ref, only_large=False):
        pc = C[tag][ref]["per_condition"]
        items = [(k, v) for k, v in pc.items() if not only_large or int(k.split("/")[0]) >= 90]
        return sum(v["mean"] > 0 for _, v in items), len(items)
    p, n = npos("C1", "CLAF-AODV")
    put("RevHonePos", str(p)); put("RevHoneN", str(n))
    p, n = npos("C4", "CARD", True)
    put("RevHtwoPos", str(p)); put("RevHtwoN", str(n))

    # Run counts.
    put("RevRunsNine", "3{,}780")
    put("RevRunsCompose", "420")
    put("RevRunsFading", "240")
    lines = ["%% generated by paper/scripts/make_rev_numbers.py -- do not edit"]
    for k in sorted(M):
        lines.append("\\newcommand{\\%s}{%s}" % (k, M[k]))
    (REV / "tables" / "rev_numbers.tex").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (REV / "scripts" / "out" / "rev_numbers.json").write_text(json.dumps(J, indent=1))
    print("rev_numbers.tex: %d macros" % len(M))


if __name__ == "__main__":
    main()
