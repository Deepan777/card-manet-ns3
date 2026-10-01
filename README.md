# CARD: cause-conditioned route repair in mobile ad hoc networks (ns-3)

This repository holds the simulation code, campaign definitions, per-run summary records and analysis scripts for
the manuscript

> P. Deepanramkumar and A. Helen Sharmila, *Cause-conditioned route repair in mobile ad hoc networks: conveying
> link-failure inference to the discovery decision*. Manuscript prepared for *Ad Hoc Networks* (Elsevier).

Authors: School of Computer Science and Engineering, Vellore Institute of Technology, Vellore, Tamil Nadu, India.
Corresponding author: A. Helen Sharmila (helensharmila.a@vit.ac.in).

## Contents

| Path | Content |
|---|---|
| `ns3/contrib/scr/` | ns-3 module `scr`: an AODV fork implementing stock AODV, the rationed-discovery controller (SCR), CARD and its ablations, and the reimplemented baselines CLAF-AODV and TAAODV. This is fork revision 10, the code that produced the published results. |
| `ns3/scenarios/` | Scenario programs (C1 outage, C4 scaling, the mobility campaign, the distance sweep, test fixtures). |
| `configs/` | Run manifests of the ten published campaigns: one entry per run, with arguments, seed and run identifier. |
| `scripts/` | Source sync, guarded build, campaign runner, manifest generators and verification tools (cross-revision identity check, trajectory equivalence, TAAODV checks). |
| `checkpoints/` | Per-run records of the published campaigns: `out_summary.csv`, `DONE.json` (arguments, exit code, SHA-256 of every output file), `stdout.txt`, `stderr.txt`. |
| `analysis/` | Analysis pipeline: loading, paired comparisons, bootstrap intervals, classifier scoring, tables and figures. |
| `analysis_out/` | Derived results that need the large event logs to recompute (classifier scores, evidence lag), plus `card_tables.json`. |
| `paper/scripts/` | Scripts that generate the manuscript's revised tables, number macros and Figs. 4-5; `paper/scripts/out/` keeps their intermediate results. |
| `raw_results/` | Distance sweep used to set the link-edge threshold. |
| `environment/` | Toolchain record (MSYS2 package lock, setup script, host and build details). |

## Requirements

- ns-3.46.1 (commit `51387bce`), from <https://gitlab.com/nsnam/ns-3-dev/-/archive/ns-3.46.1/ns-3-dev-ns-3.46.1.tar.bz2>.
- The published runs used native Windows 11 with MSYS2/MinGW64: g++ 16.1.0, CMake 4.4.2 and Ninja 1.13.2. The
  module is ordinary ns-3 C++ and should also build on Linux; it has not been tested there.
- Python 3.11 with `numpy` and `matplotlib`.

## Build

```bash
# 1. unpack ns-3.46.1 so that vendor/ns-3-dev-ns-3.46.1/ exists in this repository
mkdir -p vendor && tar -xjf ns-3-dev-ns-3.46.1.tar.bz2 -C vendor

# 2. copy the module into contrib/ and each scenario into scratch/<name>/
bash scripts/sync_sources.sh

# 3. configure and build (the options used for the published runs)
cd vendor/ns-3-dev-ns-3.46.1
python ./ns3 configure -G Ninja --build-profile=default --enable-examples --enable-tests \
    --disable-werror \
    --enable-modules="aodv;olsr;dsdv;wifi;applications;internet;mobility;propagation;flow-monitor;stats;point-to-point;csma;scr"
python ./ns3 build
cd ../..
```

`scripts/safe_build.sh` wraps step 3's build and refuses to rebuild while simulations are running. On Windows,
`scripts/run_campaign.sh` sets the DLL search path the simulator needs.

## Run the simulations

```bash
python scripts/campaign_runner.py --manifest configs/card_campaign.json --jobs 4
python scripts/campaign_runner.py --manifest configs/card_campaign.json --status
```

Each run writes `checkpoints/<campaign>/<run_id>/`. A run is complete only when `DONE.json` exists, and the runner
resumes from the first incomplete run. Seeds come from the manifests: evaluation seeds start at 201, and the
development seeds (101-105) were never used in an evaluation campaign.

| Manifest | Content |
|---|---|
| `c1_outage` | C1: relay outage (cold, staggered, repeated, long, single, healthy) |
| `c4_scaling` | C4: density and load scaling |
| `stage8_campaign` | Mobility campaign: reference protocols and controller variants |
| `stage9_ablation_sweep` | Controller component ablations |
| `c1_scr2_supplement` | SCR-2 supplement on C1 |
| `card_campaign` | CARD and its ablations (NO-CAUSE, NO-REACH, NO-EXEMPT, EXEMPT-ALL) |
| `card_baselines` | CLAF-AODV |
| `card_taaodv` | TAAODV |
| `card_cardclaf` | CARD-CLAF composition test |
| `card_fading` | Nakagami/Rayleigh fading supplement |

`configs/*.json` can be regenerated with `scripts/make_manifest_*.py`.

## Reproduce the tables and figures

From the repository root, after the records are in `checkpoints/` (the shipped summaries are enough):

```bash
python analysis/make_q1_tables.py
python analysis/make_card_tables.py
python paper/scripts/verify_numbers.py
python paper/scripts/sci_numbers.py
python paper/scripts/make_rev_numbers.py
python paper/scripts/make_rev_tables.py
python paper/scripts/make_rev_figures.py
python paper/scripts/interval_robustness.py
python paper/scripts/regime_table.py
python paper/scripts/relabel_tables.py
python paper/scripts/make_mechanism_figure.py
```

- Tables and number macros are written to `paper/tables/`, figures to `paper/figures/`.
- `relabel_tables.py` gives the renewal-study tables written by `analysis/make_q1_tables.py` the manuscript's policy
  names (acquisition, confirmation and clock renewal; fixed narrow start). It changes labels only.
  `make_mechanism_figure.py` draws Fig. 4 with the same names.
- With the shipped summaries, every table used in the manuscript regenerates identically except the break-classifier
  results. Values agree to the printed precision; the last binary digit of some intermediate floats can differ.
- The classifier results need the per-break logs: `tab_card_classifier_rev.tex` and 16 macros in `card_numbers.tex`
  (`\CardConeAcc`, `\CardCfourBreaks`, `\CardSeightPrecTopo` and the like). Without the logs:
  - `make_card_tables.py` leaves those macros out and rewrites `analysis_out/card_tables.json` without them;
  - `verify_numbers.py` stops at the classifier step.
- The values computed from the full logs are in `analysis_out/card_tables.json`,
  `analysis_out/card_classifier_*.json` and `paper/scripts/out/numerical_recompute.json`. Restore them with
  `git checkout -- analysis_out paper/scripts/out` after a run without the logs.

## Data not included here

Each run also wrote:

- a per-packet event log (`out_packet_events.csv`);
- for CARD runs, a per-break log (`out_card_breaks.csv`).

Compressed, these come to about 2.4 GB, more than a code repository should hold. They are available from the
corresponding author on reasonable request. The SHA-256 of every such file is recorded in the run's `DONE.json`, so a
copy can be checked against these records.

## Code revisions

- Every record states the fork revision that produced it (`fork_rev` in `out_summary.csv`). Campaigns were run as the
  module grew, from revision 5 to revision 10.
- Each revision added new modes without changing existing ones. A revision-10 build replayed 22 stored runs from earlier
  revisions and reproduced each one exactly: same packet-event SHA-256, same summary values. The reports are
  `checkpoints/identity_check_rev10_main/REPORT.json` and `checkpoints/identity_check_rev10_cardclaf/REPORT.json`;
  the tool is `scripts/card_identity_check.py`.
- Source comments cite entries of the project's internal research log (`R0xx`, `D0xx`) and sections of the study's
  design specification. Neither document is part of this repository.

## Licence

The `scr` module is derived from the ns-3 AODV module and is distributed under the GNU General Public License,
version 2 (see `LICENSE`), as ns-3 is.
