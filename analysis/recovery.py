#!/usr/bin/env python3
"""
M19 / E10 — true vs source-observed recovery time, with censoring.

E10 separates two quantities that are easy to conflate:

  T_true(e)   the interval from the start of an outage episode until timely
              service is genuinely restored, measured from delivery evidence
  T_source(e) the interval until the SOURCE can know service is restored,
              i.e. until a valid E2 confirmation exists at the source

T_source >= T_true always, because confirmation requires K closed blocks whose
receipts must themselves traverse the network. Reporting only T_source would
flatter any feedback-free baseline; reporting only T_true would hide the cost of
SCR's evidence requirement. Both are reported.

CENSORING is mandatory. An episode still unresolved when generation stops has no
observed recovery time. It must be recorded as censored at the cap, NEVER
discarded and NEVER imputed as if it had recovered: dropping censored episodes
biases every mean downward, and the bias is largest exactly where a method
performs worst.

This module reads per-run packet event data. It does not invent episodes.
"""

import argparse
import csv
import json
import math
import pathlib
import statistics as st
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


class Episode:
    """One outage episode for a single flow."""

    __slots__ = ("flow", "start_ns", "true_ns", "source_ns",
                 "true_censored", "source_censored")

    def __init__(self, flow, start_ns):
        self.flow = flow
        self.start_ns = start_ns
        self.true_ns = None
        self.source_ns = None
        self.true_censored = False
        self.source_censored = False

    def as_dict(self, cap_ns):
        return {
            "flow": self.flow,
            "start_ns": self.start_ns,
            "true_recovery_ns": self.true_ns if self.true_ns is not None else cap_ns,
            "true_censored": self.true_censored,
            "source_recovery_ns": (self.source_ns if self.source_ns is not None
                                   else cap_ns),
            "source_censored": self.source_censored,
        }


def build_episodes(packet_rows, deadline_ns, gap_multiple, cap_ns, end_ns,
                   confirm_blocks, block_size):
    """
    Reconstruct outage episodes from per-packet delivery evidence.

    An episode OPENS when a flow goes `gap_multiple` inter-arrival intervals
    without an on-time delivery. It closes for T_true at the next on-time
    delivery, and for T_source once `confirm_blocks` consecutive blocks have
    each met the on-time threshold - the earliest instant a source could hold
    valid E2 evidence.
    """
    by_flow = {}
    for r in packet_rows:
        by_flow.setdefault(r["flow"], []).append(r)

    episodes = []
    for flow, rows in sorted(by_flow.items()):
        rows.sort(key=lambda r: r["generation_ns"])
        interval_ns = None
        if len(rows) > 1:
            diffs = [rows[i + 1]["generation_ns"] - rows[i]["generation_ns"]
                     for i in range(min(50, len(rows) - 1))]
            diffs = [d for d in diffs if d > 0]
            interval_ns = st.median(diffs) if diffs else None
        if not interval_ns:
            continue
        gap_ns = gap_multiple * interval_ns

        current = None
        last_ontime_ns = None
        # rolling record of per-block on-time counts, for the T_source test
        block_ontime = {}

        for r in rows:
            g = r["generation_ns"]
            ontime = r["ontime"]
            blk = r["sequence"] // block_size
            block_ontime.setdefault(blk, 0)
            if ontime:
                block_ontime[blk] += 1

            if ontime:
                if current is not None:
                    # T_true closes at the first on-time delivery.
                    if current.true_ns is None:
                        current.true_ns = r["first_rx_ns"] - current.start_ns
                    # T_source closes once K consecutive blocks are good.
                    good_run = 0
                    b = blk
                    while b >= 0 and block_ontime.get(b, 0) >= math.ceil(0.8 * block_size):
                        good_run += 1
                        if good_run >= confirm_blocks:
                            break
                        b -= 1
                    if good_run >= confirm_blocks and current.source_ns is None:
                        current.source_ns = r["first_rx_ns"] - current.start_ns
                        episodes.append(current)
                        current = None
                last_ontime_ns = r["first_rx_ns"] if r["first_rx_ns"] else g
            else:
                if current is None and last_ontime_ns is not None:
                    if g - last_ontime_ns > gap_ns:
                        current = Episode(flow, last_ontime_ns)

        # Any episode still open at the end of generation is CENSORED.
        if current is not None:
            if current.true_ns is None:
                current.true_censored = True
            if current.source_ns is None:
                current.source_censored = True
            episodes.append(current)

    # Apply the cap. A recovery longer than the cap is censored at the cap.
    for e in episodes:
        if e.true_ns is not None and e.true_ns > cap_ns:
            e.true_ns = None
            e.true_censored = True
        if e.source_ns is not None and e.source_ns > cap_ns:
            e.source_ns = None
            e.source_censored = True
    return episodes


def summarise(episodes, cap_ns):
    n = len(episodes)
    if n == 0:
        return {"episodes": 0}

    true_obs = [e.true_ns for e in episodes if not e.true_censored and e.true_ns is not None]
    src_obs = [e.source_ns for e in episodes
               if not e.source_censored and e.source_ns is not None]
    true_cens = sum(1 for e in episodes if e.true_censored)
    src_cens = sum(1 for e in episodes if e.source_censored)

    def ms(v):
        return v / 1e6

    return {
        "episodes": n,
        "true_observed": len(true_obs),
        "true_censored": true_cens,
        "true_censoring_fraction": true_cens / n,
        "true_median_ms": ms(st.median(true_obs)) if true_obs else None,
        "true_mean_ms": ms(st.mean(true_obs)) if true_obs else None,
        "source_observed": len(src_obs),
        "source_censored": src_cens,
        "source_censoring_fraction": src_cens / n,
        "source_median_ms": ms(st.median(src_obs)) if src_obs else None,
        "source_mean_ms": ms(st.mean(src_obs)) if src_obs else None,
        "cap_ms": ms(cap_ns),
        # Reported explicitly so no reader mistakes a capped mean for a true mean.
        "note": ("Means and medians are over OBSERVED episodes only. The censoring "
                 "fraction must be reported alongside them; a low mean with high "
                 "censoring indicates failure, not speed."),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--packets", required=True,
                    help="packet_events CSV with flow,sequence,generation_ns,"
                         "first_rx_ns,delay_ns,hops,delivered,ontime")
    ap.add_argument("--deadline-ms", type=float, default=250.0)
    ap.add_argument("--gap-multiple", type=float, default=4.0,
                    help="intervals without on-time delivery that open an episode")
    ap.add_argument("--cap-s", type=float, default=30.0,
                    help="recovery cap; longer recoveries are censored at the cap")
    ap.add_argument("--confirm-blocks", type=int, default=2, help="K")
    ap.add_argument("--block-size", type=int, default=10, help="M")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    path = pathlib.Path(args.packets)
    if not path.exists():
        sys.exit("no such file: " + str(path))

    rows = []
    with open(path, newline="") as f:
        for rec in csv.DictReader(f):
            try:
                rows.append({
                    "flow": int(rec["flow"]),
                    "sequence": int(rec["sequence"]),
                    "generation_ns": int(rec["generation_ns"]),
                    "first_rx_ns": int(rec["first_rx_ns"]) if rec["first_rx_ns"] else 0,
                    "ontime": rec["ontime"] == "1",
                })
            except (KeyError, ValueError):
                continue

    if not rows:
        sys.exit("no usable packet rows")

    cap_ns = int(args.cap_s * 1e9)
    end_ns = max(r["generation_ns"] for r in rows)
    episodes = build_episodes(rows, int(args.deadline_ms * 1e6), args.gap_multiple,
                              cap_ns, end_ns, args.confirm_blocks, args.block_size)
    summary = summarise(episodes, cap_ns)

    print(json.dumps(summary, indent=2))
    if args.out:
        payload = {
            "source_file": str(path),
            "parameters": {
                "deadline_ms": args.deadline_ms,
                "gap_multiple": args.gap_multiple,
                "cap_s": args.cap_s,
                "confirm_blocks": args.confirm_blocks,
                "block_size": args.block_size,
            },
            "summary": summary,
            "episodes": [e.as_dict(cap_ns) for e in episodes],
        }
        pathlib.Path(args.out).write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print("wrote " + args.out)


if __name__ == "__main__":
    main()
