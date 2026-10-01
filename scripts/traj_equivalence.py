"""R055: are node trajectories really identical across algorithms for a seed?

The scenario hashes node positions every 10 s, quantised to 1 mm, and the
paper's pairing assumes every algorithm sees the same movement for a given
seed. In the published campaigns 313 of 540 (cell, seed) groups carry more
than one hash. Two explanations predict different things:

  benign   ns-3 advances a node's position incrementally each time anything
           reads it (the channel reads it for every transmission), so
           algorithms that transmit differently accumulate different
           floating-point rounding. Differences should be sub-micrometre, and
           a hash flips only when a coordinate lies near a millimetre boundary.
  real     some algorithm perturbs the mobility random stream. Differences
           would be of the order of metres and grow with time.

This script replays groups whose stored hashes differ, with --trajDump=1,
and reports the largest position difference between any two members of each
group. Each replay must also reproduce its stored run exactly (packet events
and every summary field), which shows that the dump itself changes nothing.

Usage: python scripts/traj_equivalence.py [--jobs N]
Writes checkpoints/traj_equivalence/REPORT.json.
"""

import argparse
import concurrent.futures as cf
import csv
import hashlib
import itertools
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
NS3 = ROOT / "vendor" / "ns-3-dev-ns-3.46.1"
OUT = ROOT / "checkpoints" / "traj_equivalence"

GROUPS = {
    # stage8, 15 m/s, seed 202: five different hashes across these five arms.
    "mobility_v15_s202": ["stage8_campaign/c_AODVSTOCK_s202_b69d3c99",
                          "stage8_campaign/c_RREPRESET_s202_c5d44a75",
                          "stage8_campaign/c_SCR_s202_eabee83f",
                          "stage8_campaign/c_PERSISTENTBACKOFF_s202_bc0ea628",
                          "stage8_campaign/c_DSDV_s202_e3d5f7fe"],
    # stage8 nominal, seed 202: NO-PROBE and DSDV differ from the rest.
    "mobility_v5_s202": ["stage8_campaign/c_AODVSTOCK_s202_83bacd85",
                         "stage8_campaign/c_NOPROBE_s202_993fb76d",
                         "stage8_campaign/c_DSDV_s202_6263b8c1"],
    # scaling, 90 nodes, seed 201: four hashes across eight arms.
    "scaling_n90_s201": ["c4_scaling/c4_AODVSTOCK_s201_d17a9c0c",
                         "c4_scaling/c4_RREPRESET_s201_334a047f",
                         "c4_scaling/c4_SCR_s201_4ef36ad5",
                         "c4_scaling/c4_OLSR_s201_dd186325"],
    # the pilot pair that tripped the CARD launch gate.
    "pilot_v5_s105": ["card_pilot2/c_CARD_s105_0",   # resolved below
                      "card_pilot2/c_CARDNOEXEMPT_s105_0",
                      "card_pilot/c_RREPRESET_s105_0",
                      "card_pilot/c_AODVSTOCK_s105_0"],
}


def resolve(rel):
    """Pilot entries are written with a placeholder hash; pick the 5 m/s run."""
    camp, name = rel.split("/")
    if not name.endswith("_0"):
        return rel
    prefix = name[:-1]
    for d in sorted((ROOT / "checkpoints" / camp).glob(prefix + "*")):
        if "--speed=5.0" in (d / "DONE.json").read_text():
            return "%s/%s" % (camp, d.name)
    raise SystemExit("cannot resolve " + rel)


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def summary(p):
    with open(p, newline="") as f:
        return {r[0]: r[1] for r in csv.reader(f) if len(r) >= 2}


def replay(rel):
    src = ROOT / "checkpoints" / rel
    done = json.loads((src / "DONE.json").read_text())
    work = OUT / src.name
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([r"C:\msys64\mingw64\bin", str(NS3 / "build" / "lib"),
                                   env.get("PATH", "")])
    argv = done["argv"] + ["--trajDump=1"]
    r = subprocess.run(argv, cwd=str(work), capture_output=True, text=True, env=env, timeout=7200)
    problems = []
    if r.returncode != 0:
        return rel, ["returncode %d" % r.returncode], None
    for name in ("out_packet_events.csv", "out_card_breaks.csv"):
        h = done["outputs"].get(name)
        if h is not None and sha(work / name) != h:
            problems.append("%s differs from the stored run" % name)
    old, new = summary(src / "out_summary.csv"), summary(work / "out_summary.csv")
    for k in old:
        if k == "effective_parameters":
            continue
        if old[k] != new.get(k):
            problems.append("%s: %r -> %r" % (k, old[k], new.get(k)))
    pos = {}
    with open(work / "out_traj.csv", newline="") as f:
        for row in csv.DictReader(f):
            pos[(float(row["t_s"]), int(row["node"]))] = (float(row["x"]), float(row["y"]))
    return rel, problems, pos


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=10)
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    groups = {g: [resolve(r) for r in rs] for g, rs in GROUPS.items()}
    allruns = [r for rs in groups.values() for r in rs]
    results = {}
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for rel, problems, pos in ex.map(replay, allruns):
            results[rel] = (problems, pos)
            print("%-55s %s" % (rel, "reproduces stored run" if not problems else problems), flush=True)
    report = {}
    worst_overall = 0.0
    for g, rs in groups.items():
        worst = 0.0
        pairs = {}
        for x, y in itertools.combinations(rs, 2):
            px, py = results[x][1], results[y][1]
            if px is None or py is None:
                continue
            keys = set(px) & set(py)
            d = max(max(abs(px[k][0] - py[k][0]), abs(px[k][1] - py[k][1])) for k in keys)
            pairs["%s vs %s" % (x.split("/")[1], y.split("/")[1])] = {"samples": len(keys),
                                                                    "max_abs_m": d}
            worst = max(worst, d)
        report[g] = {"max_abs_position_difference_m": worst, "pairs": pairs}
        worst_overall = max(worst_overall, worst)
        print("%-20s largest position difference between algorithms: %.3e m" % (g, worst))
    report["_replay_problems"] = {r: p for r, (p, _) in results.items() if p}
    report["_worst_overall_m"] = worst_overall
    (OUT / "REPORT.json").write_text(json.dumps(report, indent=1))
    print("worst overall: %.3e m" % worst_overall)
    return 0


if __name__ == "__main__":
    sys.exit(main())
