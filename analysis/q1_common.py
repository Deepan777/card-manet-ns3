"""Shared loader and statistics for the manuscript's tables and figures.

One place defines how a run is read, how conditions are keyed, how pairs are
formed and how intervals are computed, so every table and figure in the paper
uses the same rules. Runs are read from each campaign's checkpoint directory;
any fork-controlled run below fork_rev 5 is refused.
"""

import math
import os
import pathlib
import random
import statistics as st
from collections import defaultdict

ROOT = pathlib.Path(os.environ.get("SCR_ROOT", pathlib.Path(__file__).resolve().parent.parent))
RUNS = ROOT / "checkpoints"
if not RUNS.exists():
    RUNS = ROOT / "results"

POLICIES = ["RREP-RESET", "TIME-BUCKET", "SCR", "PERSISTENT-BACKOFF"]
REF = "AODV-STOCK"
ABLATIONS = ["NO-PROBE", "ONE-BLOCK", "NO-DEADLINE", "NO-NODE-CAP", "RESET-ESCALATION"]
BOOT_N, BOOT_SEED = 10000, 20260905

# Scenario fields that define a condition, per campaign.
COND_FIELDS = {
    "c1_outage": ("fault", "cold_stagger"),
    "c1_scr2_supplement": ("fault", "cold_stagger"),
    "c4_scaling": ("num_nodes", "area_side_m", "num_flows", "payload_bytes"),
    "stage8_campaign": ("speed_mps", "rate_pps", "num_flows"),
    "stage9_ablation_sweep": ("speed_mps", "rate_pps", "num_flows"),
}


def _records(camp):
    base = RUNS / camp
    if not base.exists():
        return
    for e in base.iterdir():
        if e.is_dir():
            s, done = e / "out_summary.csv", (e / "DONE.json").exists()
        elif e.name.endswith(".summary.csv"):
            s, done = e, (base / (e.name[:-len(".summary.csv")] + ".done.json")).exists()
        else:
            continue
        if s.exists() and done:
            yield s


def load(camp):
    """{(algorithm, condition_tuple, seed): kv} for completed, provenance-valid runs."""
    out = {}
    for s in _records(camp):
        kv = {}
        for line in s.read_text(errors="replace").splitlines()[1:]:
            k, _, v = line.partition(",")
            kv[k.strip()] = v.strip().strip('"')
        ep = kv.get("effective_parameters", "")
        if "scr_enabled=1" in ep and "fork_rev=5" not in ep:
            continue
        cond = tuple(kv.get(f) for f in COND_FIELDS[camp])
        out[(kv["algorithm"], cond, kv["rng_run"])] = kv
    return out


CARD_ARMS = ["CARD", "CARD-NO-CAUSE", "CARD-EXEMPT-ALL", "CARD-NO-EXEMPT", "CARD-NO-REACH"]
# R053/R054: the CARD campaign spans all three families in one directory. Run
# ids carry the family's generator prefix, which is how a CARD run is matched
# to the published campaign whose conditions and seeds it shares.
_CARD_PREFIX = {"c1_outage": "c1_", "c4_scaling": "c4_", "stage8_campaign": "c_"}


def load_card(camp_like):
    """CARD runs belonging to one published family, keyed like load(camp_like).

    Every CARD run must come from fork_rev 7. Pairing them with the rev-5
    references is valid because rev 7 reproduces rev 5 exactly for every
    pre-CARD mode (scripts/card_identity_check.py, RESEARCH_LEDGER R053-R055)
    and node trajectories agree across algorithms to within 4e-8 m (R055).
    """
    out = {}
    prefix = _CARD_PREFIX[camp_like]
    # R057: the published baselines (card_baselines, fork_rev 8) share the
    # CARD campaign's cells, seeds and run-id prefixes.
    entries = []
    # R058: TAAODV (card_taaodv, fork_rev 9) likewise.
    for sub, rev in (("card_campaign", "fork_rev=7"), ("card_baselines", "fork_rev=8"),
                     ("card_taaodv", "fork_rev=9"), ("card_cardclaf", "fork_rev=10")):
        base = RUNS / sub
        if base.exists():
            entries += [(e, rev) for e in base.iterdir()]
    for e, need_rev in entries:
        if not (e.is_dir() and e.name.startswith(prefix)):
            continue
        if e.name.startswith("c1_") and prefix == "c_":
            continue
        s = e / "out_summary.csv"
        if not (s.exists() and (e / "DONE.json").exists()):
            continue
        kv = {}
        for line in s.read_text(errors="replace").splitlines()[1:]:
            k, _, v = line.partition(",")
            kv[k.strip()] = v.strip().strip('"')
        if need_rev not in kv.get("effective_parameters", ""):
            continue
        cond = tuple(kv.get(f) for f in COND_FIELDS[camp_like])
        out[(kv["algorithm"], cond, kv["rng_run"])] = kv
    return out


def val(kv, m):
    g = float(kv["generated"])
    if m == "timely":
        return float(kv["ontime_true"]) / g
    if m == "bytes_per_pkt":
        return float(kv["ctrl_bytes_sent"]) / g
    return float(kv[m])


def conditions(rows):
    return sorted({c for (_, c, _) in rows}, key=lambda c: tuple(_num(x) for x in c))


def _num(x):
    try:
        return (0, float(x))
    except (TypeError, ValueError):
        return (1, str(x))


def paired(rows, a, b, cond, m="timely", rows_b=None):
    rb = rows_b if rows_b is not None else rows
    seeds = sorted({s for (alg, c, s) in rows if alg == a and c == cond})
    return [val(rows[(a, cond, s)], m) - val(rb[(b, cond, s)], m)
            for s in seeds if (b, cond, s) in rb]


def mean_of(rows, alg, cond, m="timely"):
    v = [val(kv, m) for (a, c, s), kv in rows.items() if a == alg and c == cond]
    return st.mean(v) if v else None


def boot(d):
    rng = random.Random(BOOT_SEED)
    ms = sorted(sum(rng.choice(d) for _ in d) / len(d) for _ in range(BOOT_N))
    return ms[int(0.025 * BOOT_N)], ms[int(0.975 * BOOT_N) - 1]


def sign_p(d):
    pos, neg = sum(x > 0 for x in d), sum(x < 0 for x in d)
    n = pos + neg
    if n == 0:
        return 1.0
    k = min(pos, neg)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n)


def fmt_p(p):
    return "$<$0.001" if p < 0.001 else "%.3f" % p
