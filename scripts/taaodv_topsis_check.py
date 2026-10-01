"""R058: recompute TAAODV's forwarder choice independently from logged decision matrices.

A TAAODV smoke run with NS_LOG=ScrRoutingProtocol=level_debug prints, for every
restricted RREQ, the candidates' criteria (V, N, S), the weights, the TOPSIS
closeness and the chosen forwarders ("TAA-TOPSIS ..." lines). This script
recomputes, from the logged criteria alone and following Li et al. (IEEE T-ITS
2025) Eqs. 14-16 and 28-34 and Sec. IV-A:
  * coefficient-of-variation weights,
  * Frobenius-normalised weighted TOPSIS closeness (all criteria benefits),
  * the number of forwarders (3 for 4-6 candidates, floor(n/2) above 6),
  * the chosen set (closeness descending, ties in candidate order),
and reports any disagreement beyond print precision.

Usage: python scripts/taaodv_topsis_check.py <log file>
"""

import math
import re
import sys

REL = 2e-4  # the log prints 6 significant digits


def close(a, b):
    return abs(a - b) <= REL * max(1.0, abs(a), abs(b)) or abs(a - b) < 1e-6


def recompute(X):
    n = len(X)
    w = []
    for c in range(3):
        col = [x[c] for x in X]
        m = sum(col) / n
        sd = math.sqrt(sum((v - m) ** 2 for v in col) / n)
        w.append(sd / m if m > 0 else 0.0)
    s = sum(w)
    w = [v / s for v in w] if s > 0 else [1 / 3] * 3
    fro = math.sqrt(sum(v * v for x in X for v in x))
    if fro <= 0:
        return w, [0.5] * n
    Y = [[w[c] * x[c] / fro for c in range(3)] for x in X]
    best = [max(y[c] for y in Y) for c in range(3)]
    worst = [min(y[c] for y in Y) for c in range(3)]
    f = []
    for y in Y:
        sb = math.sqrt(sum((y[c] - best[c]) ** 2 for c in range(3)))
        sw = math.sqrt(sum((y[c] - worst[c]) ** 2 for c in range(3)))
        f.append(sw / (sb + sw) if sb + sw > 0 else 0.5)
    return w, f


def main(path):
    pat = re.compile(r"TAA-TOPSIS k=(\d+) w=([^ ]+) \|(.*)\| chosen(.*)$")
    n_lines = n_ok = 0
    bad = []
    for line in open(path, errors="replace"):
        m = pat.search(line.strip())
        if not m:
            continue
        n_lines += 1
        k = int(m.group(1))
        w_log = [float(v) for v in m.group(2).split(",")]
        cand, X, f_log = [], [], []
        for tok in m.group(3).split():
            ip, vals = tok.split(":")
            v = [float(x) for x in vals.split(",")]
            cand.append(ip)
            X.append(v[:3])
            f_log.append(v[3])
        chosen = m.group(4).split()
        n = len(cand)
        problems = []
        k_rule = 3 if n <= 6 else n // 2
        if n <= 3 or k != k_rule or len(chosen) != k:
            problems.append("k=%d for n=%d (rule %d), %d chosen" % (k, n, k_rule, len(chosen)))
        w, f = recompute(X)
        if not all(close(a, b) for a, b in zip(w, w_log)):
            problems.append("weights %s vs logged %s" % (w, w_log))
        if not all(close(a, b) for a, b in zip(f, f_log)):
            problems.append("closeness differs")
        # Chosen set: the k best by closeness. Near-ties (within print precision)
        # may legitimately order either way, so compare against the logged
        # closeness values, ranking ties in candidate order.
        order = sorted(range(n), key=lambda j: -f_log[j])
        want = {cand[j] for j in order[:k]}
        if set(chosen) != want:
            kth = sorted(f_log, reverse=True)[k - 1]
            ties = [j for j in range(n) if close(f_log[j], kth)]
            if len(ties) <= 1:
                problems.append("chosen %s, expected %s" % (chosen, sorted(want)))
        for j in range(n):
            if not (0.0 <= X[j][0] <= 1.0 and 0.0 <= X[j][1] <= 1.0 and 0.0 <= X[j][2] <= 2.0):
                problems.append("criterion out of range at %s: %s" % (cand[j], X[j]))
        if problems:
            bad.append((n_lines, problems))
        else:
            n_ok += 1
    print("%d TOPSIS decisions checked, %d agree" % (n_lines, n_ok))
    for i, p in bad[:10]:
        print("  decision %d: %s" % (i, "; ".join(p)))
    return 0 if n_lines and n_ok == n_lines else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
