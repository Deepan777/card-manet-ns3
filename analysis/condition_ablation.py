"""Section 6 on a COMPLETE Stage 8 condition, paired by seed with bootstrap CIs.

The campaign-wide report averages per-condition means and is therefore limited
by how many conditions have paired data. A single finished condition instead
gives full n per arm, which is what an effect size needs.
"""

import collections
import json
import os
import pathlib
import random
import statistics as st

# Derive the root from this file rather than hardcoding one machine's path, so
# the script runs from the published artefact as well as from the working tree.
# SCR_ROOT overrides. RUNS resolves to checkpoints/ in the working tree and to
# results/ in the artefact, where the per-run working files are not shipped.
ROOT = pathlib.Path(os.environ.get("SCR_ROOT", pathlib.Path(__file__).resolve().parent.parent))
RUNS = ROOT / "checkpoints"
if not RUNS.exists():
    RUNS = ROOT / "results"
ABL = ["NO-PROBE", "ONE-BLOCK", "NO-DEADLINE", "NO-NODE-CAP", "RESET-ESCALATION"]


def run_records(campaign):
    """Yield (run_id, summary_path, done) for a campaign, in either layout.

    The working tree stores checkpoints/<campaign>/<run_id>/out_summary.csv.
    The published artefact flattens that to <campaign>/<run_id>.summary.csv,
    because the extra level pushed extraction past Windows' MAX_PATH. Reading
    both means the same analysis runs unchanged from the repository and from
    the archive, which is what the artefact README promises a reader.
    """
    base = RUNS / campaign
    if not base.exists():
        return
    for e in base.iterdir():
        if e.is_dir():
            s = e / "out_summary.csv"
            if s.exists():
                yield e.name, s, (e / "DONE.json").exists()
        elif e.name.endswith(".summary.csv"):
            rid = e.name[:-len(".summary.csv")]
            yield rid, e, (base / (rid + ".done.json")).exists()


def complete_conditions():
    """Every Stage 8 condition whose runs are all DONE, in manifest order.

    This was a hardcoded ("5","40","8") until 2026-09-22. That froze Section 6
    to the first condition that finished: when a second one completed, the table
    the manuscript inputs would silently have kept reporting the first. The
    first complete condition is still the PRIMARY one -- Section 6.2 declares
    its intervals final and they must not move -- but the others are now
    reported alongside it, which is what Section 6.5 promises a reader.
    """
    man = json.loads((ROOT / "configs/stage8_campaign.json").read_text())["runs"]
    done = {rid for rid, _, ok in run_records("stage8_campaign") if ok}
    total, fin, order = collections.Counter(), collections.Counter(), []
    for r in man:
        m = r["meta"]
        # summary CSV writes these as strings; match that so keys compare equal
        c = (("%g" % m["speed"]), ("%g" % m["ratePps"]), ("%g" % m["numFlows"]))
        if c not in total:
            order.append(c)
        total[c] += 1
        if r["run_id"] in done:
            fin[c] += 1
    return [c for c in order if fin[c] == total[c]], total, fin


def load(target):
    man = {r["run_id"] for r in
           json.loads((ROOT / "configs/stage8_campaign.json").read_text())["runs"]}
    out = collections.defaultdict(dict)
    for rid, s, ok in run_records("stage8_campaign"):
        if rid not in man or not ok:
            continue
        kv = {}
        for line in s.read_text(errors="replace").splitlines():
            if "," in line:
                k, _, v = line.partition(",")
                kv[k.strip()] = v.strip().strip('"')
        key = (kv.get("speed_mps") or kv.get("speed"),
               kv.get("rate_pps") or kv.get("ratePps"),
               kv.get("num_flows") or kv.get("numFlows"))
        if key != target:
            continue
        g = float(kv.get("generated", 0) or 0)
        if not g:
            continue
        out[kv["algorithm"]][kv["rng_run"]] = (
            float(kv.get("ontime_true", 0) or 0) / g,
            float(kv.get("ctrl_bytes_sent", 0) or 0) / g,
            float(kv.get("service_confirmations", 0) or 0))
    return out


def boot(diffs, n=10000):
    """Percentile bootstrap over seed-level paired differences.

    Identical to analyze_campaign.bootstrap_ci (10,000 resamples, seed
    20260905, same percentile indices). Until 2026-09-23 this used 6,000
    resamples with seed 11 and a one-off upper index, so the two scripts could
    print slightly different intervals for the same comparison.
    """
    rng = random.Random(20260905)
    ms = sorted(sum(rng.choice(diffs) for _ in diffs) / len(diffs) for _ in range(n))
    return ms[int(0.025 * n)], ms[int(0.975 * n) - 1]


def sign_p(diffs):
    """Two-sided exact sign test."""
    pos = sum(1 for d in diffs if d > 0)
    neg = sum(1 for d in diffs if d < 0)
    n = pos + neg
    if n == 0:
        return 1.0
    from math import comb
    k = min(pos, neg)
    tail = sum(comb(n, i) for i in range(k + 1)) / (2 ** n)
    return min(1.0, 2 * tail)


def compare(data, arm, ref):
    a, b = data.get(arm, {}), data.get(ref, {})
    seeds = sorted(set(a) & set(b))
    if len(seeds) < 3:
        return None
    dt = [a[s][0] - b[s][0] for s in seeds]
    dc = [a[s][1] - b[s][1] for s in seeds]
    lo, hi = boot(dt)
    return len(seeds), st.mean(dt), lo, hi, sign_p(dt), st.mean(dc)


def latex(rows, n_seeds, target):
    """DISABLED 2026-09-23. analysis/make_q1_tables.py owns every manuscript table
    (manuscript/tables/). Two writers for one table is how R044 shipped a wrong
    table; this script is now a console report only."""
    return
    out = ROOT / "manuscript" / "tab_ablation.tex"
    out.parent.mkdir(exist_ok=True)
    B = chr(92)
    L = ["%% generated by analysis/condition_ablation.py",
         "%% Stage 8 condition speed=%s m/s, rate=%s pkt/s, flows=%s; n=%d paired seeds"
         % (target[0], target[1], target[2], n_seeds),
         B + "begin{tabular}{@{}lrlr@{}}", B + "toprule",
         "Ablation & $" + B + "Delta$ timely & 95" + B + "% CI & sign $p$ " + B + B,
         B + "midrule"]
    for arm, n, m, lo, hi, pv, dc in rows:
        ps = "$<0.0001$" if pv < 1e-4 else "$%.2f$" % pv
        L.append("%s & $%+.4f$ & $[%+.4f, %+.4f]$ & %s %s"
                 % (arm, m, lo, hi, ps, B + B))
    L += [B + "bottomrule", B + "end{tabular}"]
    out.write_text(chr(10).join(L) + chr(10), encoding="utf-8")
    print(chr(10) + "wrote manuscript/tab_ablation.tex")


def report(data, target, primary):
    """Console report for one condition; writes Section 6's table only for the
    primary (first-complete) condition, whose intervals the paper declares final."""
    print("=" * 74)
    print("Stage 8 condition speed=%s m/s, rate=%s pkt/s, flows=%s  (COMPLETE)%s"
          % (target[0], target[1], target[2], "   [PRIMARY]" if primary else ""))
    print("arms: %d, seeds per arm: %s" % (len(data), sorted({len(v) for v in data.values()})))
    print()

    vs_scr = []
    for ref in ("SCR", "AODV-STOCK"):
        arms = ABL if ref == "SCR" else ABL + ["SCR", "RREP-RESET", "TIME-BUCKET",
                                               "PERSISTENT-BACKOFF"]
        print("=== paired vs %s ===" % ref)
        print("%-20s %4s %10s %-22s %9s %11s"
              % ("arm", "n", "d timely", "95% CI", "sign p", "d ctrlB/pkt"))
        rows = []
        for arm in arms:
            r = compare(data, arm, ref)
            if r:
                rows.append((arm,) + r)
        rows.sort(key=lambda x: x[2])
        for arm, n, m, lo, hi, pv, dc in rows:
            print("%-20s %4d %+10.4f  [%+.4f, %+.4f] %9.4f %+11.2f"
                  % (arm, n, m, lo, hi, pv, dc))
        print()
        if ref == "SCR":
            vs_scr = rows
            if primary:
                latex(rows, max((r[1] for r in rows), default=0), target)

    print("absolute, mean over seeds:")
    print("%-20s %9s %11s %7s" % ("arm", "timely", "ctrlB/pkt", "confs"))
    for a in sorted(data, key=lambda a: -st.mean(v[0] for v in data[a].values())):
        v = list(data[a].values())
        print("%-20s %9.4f %11.2f %7.1f"
              % (a, st.mean(x[0] for x in v), st.mean(x[1] for x in v),
                 st.mean(x[2] for x in v)))
    print()
    return {arm: (m, lo, hi, pv) for arm, n, m, lo, hi, pv, dc in vs_scr}


def cross_condition(results):
    """Section 6.5's promise, made checkable: the same ablation across every
    complete condition. Where a later condition contradicts the primary, the
    contradiction is the finding and must be reported as one, not smoothed."""
    conds = list(results)
    print("=" * 74)
    print("ABLATION vs SCR ACROSS %d COMPLETE CONDITIONS" % len(conds))
    print("(delta timely PDR; CI excluding zero is marked *)")
    print()
    hdr = "%-20s" % "ablation"
    for c in conds:
        hdr += "  %-22s" % ("v=%s r=%s f=%s" % c)
    print(hdr)
    print("-" * len(hdr))
    for arm in ABL:
        line = "%-20s" % arm
        for c in conds:
            r = results[c].get(arm)
            if not r:
                line += "  %-22s" % "--"
            else:
                m, lo, hi, pv = r
                star = "*" if (lo > 0 or hi < 0) else " "
                line += "  %+.4f [%+.3f,%+.3f]%s" % (m, lo, hi, star)
        print(line)
    print()

    return  # manuscript table now generated by make_q1_tables.py (see latex())
    B = chr(92)
    out = ROOT / "manuscript" / "tab_ablation_conditions.tex"
    hdr = " & ".join("$v{=}%s$, $r{=}%s$, $f{=}%s$" % c for c in conds)
    L = ["%% generated by analysis/condition_ablation.py -- do not hand-edit",
         "%% Section 6.5: the same ablations across every COMPLETE Stage 8 condition.",
         "%% A starred cell has a 95 percent CI excluding zero.",
         B + "begin{tabular}{@{}l" + "r" * len(conds) + "@{}}",
         B + "toprule",
         "Ablation & " + hdr + " " + B + B,
         B + "midrule"]
    for arm in ABL:
        cells = []
        for c in conds:
            r = results[c].get(arm)
            if not r:
                cells.append("--")
            else:
                star = "^{*}" if (r[1] > 0 or r[2] < 0) else ""
                cells.append("$%+.4f%s$" % (r[0], star))
        L.append("%s & %s %s" % (arm, " & ".join(cells), B + B))
    L += [B + "bottomrule", B + "end{tabular}"]
    out.write_text(chr(10).join(L) + chr(10), encoding="utf-8")
    print("wrote manuscript/tab_ablation_conditions.tex")


def main():
    complete, total, fin = complete_conditions()
    if not complete:
        print("No Stage 8 condition is complete yet. Section 6 needs full cells,")
        print("not a balanced sample of thin ones. Nothing written.")
        for c in sorted(total, key=lambda c: -fin[c] / total[c]):
            print("  v=%s r=%s f=%s  %d/%d" % (c + (fin[c], total[c])))
        return

    print("complete conditions: %d of %d" % (len(complete), len(total)))
    print()
    results = {}
    for i, c in enumerate(complete):
        data = load(c)
        if not data:
            print("condition %s is complete but produced no usable summaries" % (c,))
            continue
        results[c] = report(data, c, primary=(i == 0))

    if len(results) > 1:
        cross_condition(results)
    else:
        print("Only one complete condition, so there is nothing to cross-check yet.")
        print("Section 6.5 stays as written until a second condition finishes.")


if __name__ == "__main__":
    main()
