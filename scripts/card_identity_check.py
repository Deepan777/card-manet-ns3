"""R053: prove that fork_rev 6 leaves every pre-CARD mode unchanged.

CARD added code to paths that every mode executes: a PHY monitor callback,
a RERR flag bit, a second OnRouteInstalled overload, the admission switch.
Each is written to be inert outside the CARD modes, but "written to be inert"
is a claim, and the published results were produced by the rev-5 binary. This
script replays stored rev-5 runs with the rev-6 binary and compares:

  * out_packet_events.csv  -- SHA-256 must equal the hash recorded in DONE.json;
  * out_summary.csv        -- every key must be identical, except
                              effective_parameters, which must differ ONLY in
                              fork_rev=5 or 6 -> the current revision.

Any difference is a failure, and the rev-5 results could then not be pooled
with rev-6 CARD runs. Usage: python scripts/card_identity_check.py [--jobs N]
"""

import argparse
import concurrent.futures as cf
import csv
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
NS3 = ROOT / "vendor" / "ns-3-dev-ns-3.46.1"
OUT = ROOT / "checkpoints" / "card_identity_check"

# One run per family and per mode kind, chosen to exercise the paths CARD
# touched: the repeated C1 outage cycles interfaces down and up (the trace
# connect/disconnect path); mobility produces many RERRs; the native reference
# never enters the SCR admission path at all.
RUNS = [
    "c1_outage/c1_repeated_RREPRESET_s201_12541af1",
    "c1_outage/c1_repeated_SCR_s201_f6f4e2fa",
    "c1_outage/c1_repeated_AODVSTOCK_s201_9bd7c25b",
    "c4_scaling/c4_SCR_s201_45588025",
    "stage8_campaign/c_RREPRESET_s201_5c5568c4",
    "stage8_campaign/c_AODVSTOCK_s201_463a093b",
    "stage8_campaign/c_TIMEBUCKET_s201_2f68186f",
    # R054 (rev 7): the three CARD variants whose semantics did not change
    # must reproduce their rev-6 pilot runs exactly -- the transient-only
    # bottleneck path and both mobility paths (exempt + hint, hint only,
    # exempt only).
    "card_pilot/c1_repeated_CARDNOREACH_s101_9e523d05",
    "card_pilot/c_CARDEXEMPTALL_s101_2791bcd9",
    "card_pilot/c_CARDNOCAUSE_s101_33d43198",
    "card_pilot/c_CARDNOREACH_s101_01ddf17e",
    # R057 (rev 8): CARD and CARD-NO-EXEMPT as produced at rev 7 -- the
    # bottleneck (all-transient) path and a mobility cell.
    "card_campaign/c1_repeated_CARD_s201_0a210535",
    "card_campaign/c_CARD_s201_aa90392c",
    "card_pilot2/c_CARDNOEXEMPT_s101_b41cc56c",
    # R058 (rev 9): CLAF-AODV as produced at rev 8 -- two bottleneck cells from
    # card_baselines and the mobility smoke run (repackaged in claf_smoke_ref).
    "card_baselines/c1_repeated_CLAFAODV_s201_13ae1c05",
    "card_baselines/c1_cold_staggered_CLAFAODV_s201_227a29ad",
    "claf_smoke_ref/mob_CLAFAODV_s101",
    # R060 (rev 10): TAAODV as produced at rev 9 -- the bottleneck and 10 m/s.
    "card_taaodv/c1_repeated_TAAODV_s201_8d00dc3e",
    "card_taaodv/c_TAAODV_s201_b1262900",
    # Release (R061): the main tree rebuilt at rev 10 must also reproduce the
    # rev-10 composition runs and a rev-7 single-component variant.
    "card_cardclaf/c1_repeated_CARDCLAF_s201_721dce8c",
    "card_cardclaf/c_CARDCLAF_s201_350d78f5",
    "card_campaign/c_CARDNOREACH_s201_d6205f84",
    # R062 (rev 11): the scaling family (smallest cell) for every policy whose
    # published scaling runs come from a different tree, and the fading
    # supplement, whose channel draws per-frame fading on the classifier path.
    "card_campaign/c4_CARD_s201_562832fa",
    "card_baselines/c4_CLAFAODV_s201_10de0db7",
    "card_taaodv/c4_TAAODV_s201_44aa468d",
    "c4_scaling/c4_AODVSTOCK_s201_e5b2a901",
    "card_fading/f_CARD_s201_3905e110",
    "card_fading/f_AODVSTOCK_s201_051e739d",
]
CURRENT_REV = "fork_rev=7"
TREE_DEFAULT = "ns-3-dev-ns-3.46.1"
TREE = TREE_DEFAULT


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
    tree = ROOT / "vendor" / TREE
    env["PATH"] = os.pathsep.join([r"C:\msys64\mingw64\bin", str(tree / "build" / "lib"),
                                   env.get("PATH", "")])
    argv = list(done["argv"])
    # Stored runs name the tree that produced them (main or a development
    # tree); the replay always uses the tree under test.
    argv[0] = re.sub(r"ns-3-dev-[^/\\]+", TREE, argv[0])
    r = subprocess.run(argv, cwd=str(work), capture_output=True, text=True, env=env,
                       timeout=3600)
    (work / "stdout.txt").write_text(r.stdout)
    (work / "stderr.txt").write_text(r.stderr)
    problems = []
    if r.returncode != 0:
        return rel, ["returncode %d" % r.returncode]
    # Every per-event output the stored run recorded: packet events and, for
    # CARD runs, the per-break classifier log.
    for name in ("out_packet_events.csv", "out_card_breaks.csv"):
        h_old = done["outputs"].get(name)
        if h_old is None:
            continue
        if not (work / name).exists() or sha(work / name) != h_old:
            problems.append("%s differs" % name)
    old = summary(src / "out_summary.csv")
    new = summary(work / "out_summary.csv")
    added = []
    for k in sorted(set(old) | set(new)):
        if k == "effective_parameters":
            o, n = old.get(k, ""), new.get(k, "")
            if "fork_rev=" in o:
                ok = (o.replace("fork_rev=5", CURRENT_REV)
                       .replace("fork_rev=6", CURRENT_REV)
                       .replace("fork_rev=7", CURRENT_REV)
                       .replace("fork_rev=8", CURRENT_REV)
                       .replace("fork_rev=9", CURRENT_REV)
                       .replace("fork_rev=10", CURRENT_REV)) == n
            else:
                # Native references recorded before R028/R029 carry no revision
                # fields; the new record may only APPEND to the old one.
                ok = n.startswith(o + " ")
            if not ok:
                problems.append("effective_parameters: %r -> %r" % (o, n))
        elif k not in old:
            # A counter that did not exist when the stored run was produced
            # (e.g. the R032 denial breakdown on an older native reference).
            # Not a behaviour difference; listed, not failed.
            added.append(k)
        elif old.get(k) != new.get(k):
            problems.append("%s: %r -> %r" % (k, old.get(k), new.get(k)))
    if added and not problems:
        print("    %s: fields added since the stored run: %s" % (rel, ", ".join(added)))
    return rel, problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=len(RUNS))
    ap.add_argument("--rev", default="7", help="fork revision the binary under test reports")
    ap.add_argument("--tree", default=TREE_DEFAULT, help="vendor tree holding the binary")
    ap.add_argument("--out", default=None, help="checkpoint subdirectory for the replays")
    a = ap.parse_args()
    global CURRENT_REV, TREE, OUT
    CURRENT_REV = "fork_rev=" + a.rev
    TREE = a.tree
    if a.out:
        OUT = ROOT / "checkpoints" / a.out
    OUT.mkdir(parents=True, exist_ok=True)
    fails = 0
    report = {}
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for rel, problems in ex.map(replay, RUNS):
            report[rel] = problems or "IDENTICAL"
            print("%-60s %s" % (rel, "IDENTICAL" if not problems else "DIFFERS"), flush=True)
            for p in problems:
                print("    " + p)
            fails += bool(problems)
    (OUT / "REPORT.json").write_text(json.dumps(report, indent=1))
    print("%d of %d runs identical" % (len(RUNS) - fails, len(RUNS)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
