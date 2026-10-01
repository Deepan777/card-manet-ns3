"""R058: does TAAODV ever name a bridge relay in the bottleneck topology?

Reads TAA-TOPSIS debug lines from a scr-c1-outage TAAODV run and reports, for
the ten bridge relays (node ids 50-59, addresses 10.3.0.51-10.3.0.60), how
often each appeared as a candidate forwarder and how often it was named, and
the same for cluster nodes, together with the mean criteria (V, N, S) and
closeness of each group. Usage: python scripts/taaodv_bridge_check.py <log>
"""

import collections
import re
import statistics as st
import sys

PREFIX = "10.3.0."  # scr-c1-outage assigns 10.3.0.0/16; node i is PREFIX + (i + 1)
BRIDGE = {PREFIX + str(i + 1) for i in range(50, 60)}


def main(path):
    pat = re.compile(r"\[node (\d+)\].*TAA-TOPSIS k=(\d+) w=([^ ]+) \|(.*)\| chosen(.*)$")
    cand = collections.Counter()
    named = collections.Counter()
    crit = collections.defaultdict(list)
    w_all = []
    decisions = 0
    for line in open(path, errors="replace"):
        m = pat.search(line)
        if not m:
            continue
        decisions += 1
        w_all.append([float(x) for x in m.group(3).split(",")])
        chosen = set(m.group(5).split())
        for tok in m.group(4).split():
            ip, vals = tok.split(":")
            v = [float(x) for x in vals.split(",")]
            grp = "bridge" if ip in BRIDGE else "cluster"
            cand[ip] += 1
            named[ip] += ip in chosen
            crit[grp].append(v)
    print("%d decisions; mean weights V, N, S = %s" % (
        decisions, ", ".join("%.3f" % st.mean(w[i] for w in w_all) for i in range(3))))
    for grp in ("cluster", "bridge"):
        ips = [ip for ip in cand if (ip in BRIDGE) == (grp == "bridge")]
        c = sum(cand[ip] for ip in ips)
        n = sum(named[ip] for ip in ips)
        rows = crit[grp]
        if rows:
            means = [st.mean(r[i] for r in rows) for i in range(4)]
            print("%-7s candidate %6d times, named %6d (%.1f%%); mean V %.3f N %.3f S %.3f f %.3f"
                  % (grp, c, n, 100.0 * n / c if c else 0, *means))
    for ip in sorted(BRIDGE, key=lambda a: int(a.split(".")[-1])):
        print("  %s candidate %5d named %5d" % (ip, cand[ip], named[ip]))


if __name__ == "__main__":
    main(sys.argv[1])
