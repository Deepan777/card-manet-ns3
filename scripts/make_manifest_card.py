"""R053: manifests for CARD (cause-aware rationed discovery), fork_rev 6.

Two stages, written as separate campaigns so neither can mutate the other or
any published campaign:

  pilot  development seeds 101-105, never used by any evaluation campaign.
         Three cells (bottleneck repeated outage; mobility at 5 and 10 m/s),
         the four CARD variants plus the two references they are read against
         (RREP-RESET and AODV-STOCK at the same development seeds). Its only
         jobs are to show that CARD runs cleanly and to score the break
         classifier; its delivery numbers are not reported as results.

  main   the held-out seeds and the exact cells of the three published
         families: c1_outage (6 cells x 10 seeds), c4_scaling (12 x 10) and
         stage8_campaign (8 x 30). CARD arms only. Every reference arm already
         exists at rev 5 for every one of these (cell, seed) pairs and is
         paired against, which is valid only because scripts/
         card_identity_check.py showed that rev 6 reproduces rev 5 exactly for
         the pre-CARD modes.

Run identity is produced by each family's own make_run, imported from its
generator, so ids, arguments and the preregistered freshness rule are the same
functions that produced the published runs.
"""

import argparse
import importlib.util
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


G8 = load("make_manifest_campaign")
G1 = load("make_manifest_c1")
G4 = load("make_manifest_c4")

# R054 (rev 7). Priority order, used arm-major in the main campaign: CARD
# first, then the two cause-blind variants that test whether the cause matters
# (hint on every recovery; every break exempt), then the two single-component
# variants (cause-aware reach only; cause-aware exemption only).
CARD_ARMS = ["CARD", "CARD-NO-CAUSE", "CARD-EXEMPT-ALL", "CARD-NO-EXEMPT", "CARD-NO-REACH"]
PILOT2_ARMS = ["CARD", "CARD-NO-EXEMPT"]
PILOT_REFS = ["RREP-RESET", "AODV-STOCK"]
PILOT_SEEDS = [101, 102, 103, 104, 105]

C1_BIN = ("vendor/ns-3-dev-ns-3.46.1/build/scratch/scr-c1-outage/"
          "ns3.46.1-scr-c1-outage-default.exe")
MANET_BIN = ("vendor/ns-3-dev-ns-3.46.1/build/scratch/scr-manet/"
             "ns3.46.1-scr-manet-default.exe")


def tag(row, family, binary):
    # The runner takes one binary per manifest, so a mixed-family manifest
    # carries the binary on each run as its first argument.
    row = dict(row)
    row["meta"] = dict(row["meta"], family=family)
    row["binary"] = str(ROOT / binary)
    return row


def published_cells():
    """(family, cell-key, callable(algo, seed) -> row, seeds) for every published cell."""
    cells = []
    m1 = json.loads((ROOT / "configs" / "c1_outage.json").read_text())
    seen = set()
    for r in m1["runs"]:
        k = (r["meta"]["condition"], r["meta"]["fault"], r["meta"]["cold_stagger"])
        if k not in seen:
            seen.add(k)
            cells.append(("bottleneck", k,
                          lambda a, s, k=k: G1.make_run(a, s, k[0], k[1], k[2])[1],
                          m1["seeds"]))
    m4 = json.loads((ROOT / "configs" / "c4_scaling.json").read_text())
    seen = {}
    for r in m4["runs"]:
        p = {k: r["meta"][k] for k in ("numNodes", "areaSide", "speed", "numFlows",
                                        "ratePps", "payloadBytes", "freshnessS")}
        k = tuple(sorted(p.items()))
        if k not in seen:
            seen[k] = (p, r["meta"]["panel"][0])
    for k, (p, panel) in seen.items():
        cells.append(("scaling", k, lambda a, s, p=p, panel=panel: G4.make_run(a, s, p, panel)[1],
                      m4["seeds"]))
    m8 = json.loads((ROOT / "configs" / "stage8_campaign.json").read_text())
    seen = set()
    for r in m8["runs"]:
        k = (r["meta"]["speed"], r["meta"]["ratePps"], r["meta"]["numFlows"])
        if k not in seen:
            seen.add(k)
            cells.append(("mobility", k,
                          lambda a, s, k=k: G8.make_run(
                              a, s, {"speed": k[0], "ratePps": k[1], "numFlows": k[2]},
                              ["CARD"])[1],
                          m8["seeds"]))
    return cells


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", choices=["pilot", "pilot2", "main", "fading", "baselines",
                                        "taaodv", "cardclaf"],
                    required=True)
    ap.add_argument("--arms", default=",".join(CARD_ARMS))
    a = ap.parse_args()
    arms = a.arms.split(",")
    binaries = {"bottleneck": C1_BIN, "scaling": MANET_BIN, "mobility": MANET_BIN}
    if a.stage == "baselines":
        # R057: published baselines run from the rev-8 development tree, whose
        # binary was identity-checked against rev 5, 6 and 7 runs.
        binaries = {k: v.replace("ns-3-dev-ns-3.46.1", "ns-3-dev-baselines")
                    for k, v in binaries.items()}
        if a.arms == ",".join(CARD_ARMS):
            arms = ["CLAF-AODV"]
    if a.stage == "taaodv":
        # R058: TAAODV runs from its own rev-9 development tree, identity-checked
        # against rev 8 (CLAF-AODV), 7, 6 and 5 runs; the CLAF-AODV campaign keeps
        # its rev-8 tree untouched while it runs.
        binaries = {k: v.replace("ns-3-dev-ns-3.46.1", "ns-3-dev-taaodv")
                    for k, v in binaries.items()}
        if a.arms == ",".join(CARD_ARMS):
            arms = ["TAAODV"]
    if a.stage == "cardclaf":
        # R060: preregistered composition test, rev-10 development tree.
        binaries = {k: v.replace("ns-3-dev-ns-3.46.1", "ns-3-dev-cardclaf")
                    for k, v in binaries.items()}
        if a.arms == ",".join(CARD_ARMS):
            arms = ["CARD-CLAF"]

    runs = []
    cells = published_cells()
    if a.stage == "fading":
        # R056: robustness of the break classifier under a noisy channel. The main
        # families have no fading, so received power is a deterministic function of
        # distance and the signal trend is unusually clean. Here Nakagami m=1
        # (Rayleigh, the severe case) is chained after log-distance on its own pinned
        # stream. No published run has fading, so the references run here too.
        # Thresholds are NOT retuned. Run ids get an "f_" prefix so they can never
        # collide with, or be analysed as, an unfaded run.
        import hashlib
        arms = ["AODV-STOCK", "RREP-RESET", "CARD", "CARD-NO-CAUSE"]
        for cond in ({"speed": 5.0, "ratePps": 20.0, "numFlows": 8},
                     {"speed": 10.0, "ratePps": 20.0, "numFlows": 8}):
            for arm in arms:
                for s in G8.CAMPAIGN_SEEDS_ALL:
                    _, row = G8.make_run(arm, s, cond, ["R056"])
                    key = "fading|%s|seed=%d|%s" % (arm, s, sorted(cond.items()))
                    row["run_id"] = "f_%s_s%d_%s" % (arm.replace("-", ""), s,
                                                    hashlib.sha256(key.encode()).hexdigest()[:8])
                    row["args"] = row["args"] + ["--fading=1"]
                    row["meta"]["fading"] = 1
                    runs.append(tag(row, "mobility_fading", MANET_BIN))
    elif a.stage == "pilot2":
        # R054: only the two variants whose behaviour changed at rev 7; the
        # other variants and the references are identity-checked against the
        # rev-6 pilot instead of being re-run.
        arms = PILOT2_ARMS if a.arms == ",".join(CARD_ARMS) else arms
        keep = {("bottleneck", ("repeated", "repeated", 0)),
                ("mobility", (5.0, 20.0, 8)), ("mobility", (10.0, 20.0, 8))}
        for fam, key, mk, _ in cells:
            if (fam, key) in keep:
                for arm in arms:
                    for s in PILOT_SEEDS:
                        runs.append(tag(mk(arm, s), fam, binaries[fam]))
    elif a.stage == "pilot":
        keep = {("bottleneck", ("repeated", "repeated", 0)),
                ("mobility", (5.0, 20.0, 8)), ("mobility", (10.0, 20.0, 8))}
        for fam, key, mk, _ in cells:
            if (fam, key) not in keep:
                continue
            for arm in arms + PILOT_REFS:
                for s in PILOT_SEEDS:
                    runs.append(tag(mk(arm, s), fam, binaries[fam]))
    elif a.stage in ("baselines", "taaodv", "cardclaf"):
        for arm in arms:
            for fam, key, mk, seeds in cells:
                for s in seeds:
                    runs.append(tag(mk(arm, s), fam, binaries[fam]))
    else:
        # Arm-major: the full CARD arm completes in every cell before any
        # component ablation starts, so the principal comparison is available
        # first and the ablations only ever add to it.
        for arm in arms:
            for fam, key, mk, seeds in cells:
                for s in seeds:
                    runs.append(tag(mk(arm, s), fam, binaries[fam]))

    ids = [r["run_id"] for r in runs]
    assert len(ids) == len(set(ids)), "run_id collision"
    # Never re-run or overwrite a published run.
    published = set()
    for m in ("c1_outage", "c4_scaling", "stage8_campaign"):
        published |= {r["run_id"] for r in
                      json.loads((ROOT / "configs" / (m + ".json")).read_text())["runs"]}
    if a.stage in ("main", "baselines", "taaodv", "cardclaf"):
        assert not (set(ids) & published), "collides with a published run"

    cid = {"pilot": "card_pilot", "pilot2": "card_pilot2", "main": "card_campaign",
           "fading": "card_fading", "baselines": "card_baselines",
           "taaodv": "card_taaodv", "cardclaf": "card_cardclaf"}[a.stage]
    man = {
        "campaign_id": cid,
        "purpose": ("CARD pilot: runs cleanly? classifier accuracy? Development seeds only; "
                    "not reported as results." if a.stage == "pilot" else
                    "CARD arms in every published cell at the published held-out seeds."),
        "fork_rev": {"baselines": 8, "taaodv": 9, "cardclaf": 10}.get(a.stage, 7),
        "binary": str(ROOT / MANET_BIN),
        "binary_per_run": True,
        "ns3_version": "3.46.1",
        "ns3_commit": "51387bce7e5f5c57aa080612a4ed690bf33d0c92",
        "arms": arms + (PILOT_REFS if a.stage == "pilot" else []),
        "design_revision": ("R054: reach hint applied only after a topology change (CARD); "
                            "CARD-NO-EXEMPT added. Pilot 1 (rev 6) showed the cause-blind hint "
                            "costs 0.16 timely PDR in the bottleneck."),
        "classifier_defaults": {"card_edge_dbm": -79.0, "card_trend_db": 0.1,
                                "derivation": ("range edge -82 dBm (link cliff at 141-142 m, "
                                               "stage2 distance sweep) plus 3 dB; any fall "
                                               "above 0.1 dB over >= 1 s. Fixed before the "
                                               "pilot.")},
        "pairing": ("main stage pairs by (cell, seed) against the rev-5 reference runs; valid "
                    "only given the rev-6 identity check" if a.stage == "main" else
                    "references run at the same development seeds inside the pilot"),
        "runs": runs,
    }
    out = ROOT / "configs" / (cid + ".json")
    out.write_text(json.dumps(man, indent=1))
    from collections import Counter
    print("wrote %s: %d runs" % (out, len(runs)))
    print(" ", dict(Counter((r["meta"]["family"], r["meta"]["algorithm"]) for r in runs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
