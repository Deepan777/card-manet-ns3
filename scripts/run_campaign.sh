#!/usr/bin/env bash
#
# Launch a campaign with the environment the simulator actually needs.
#
# Why this exists. The runners were once started with:
#
#     export PATH="...:$NS3/build/lib" && nohup python runner.py A & 
#     nohup python runner.py B &
#
# In bash, `A && B &` backgrounds the WHOLE compound in a subshell, so the
# export never reached the parent shell. The first campaign got the DLL search
# path and the other two did not: 1,756 runs "failed" in 0.05 s each with
# 0xC0000135 STATUS_DLL_NOT_FOUND, burning through two entire manifests in
# seconds. Nothing was wrong with the binaries -- the same arguments ran fine by
# hand. An environment fault that looks exactly like a mass simulation failure
# is worth making impossible rather than remembering.
#
# Usage:  bash scripts/run_campaign.sh <manifest.json> <jobs> [logfile]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NS3_DIR="$ROOT/vendor/ns-3-dev-ns-3.46.1"

MANIFEST="${1:?usage: run_campaign.sh <manifest.json> <jobs> [logfile]}"
JOBS="${2:?usage: run_campaign.sh <manifest.json> <jobs> [logfile]}"
LOG="${3:-$ROOT/logs/$(basename "${MANIFEST%.json}").log}"

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH:$NS3_DIR/build/lib"

# Fail loudly HERE rather than as 1,276 identical run failures later.
BIN="$(python - "$MANIFEST" <<'PY'
import json, sys
print(json.load(open(sys.argv[1]))["binary"])
PY
)"
if [ ! -x "$BIN" ] && [ ! -f "$BIN" ]; then
  echo "campaign binary missing: $BIN" >&2
  exit 1
fi
if ! "$BIN" --PrintHelp >/dev/null 2>&1; then
  echo "PRE-FLIGHT FAILED: the campaign binary cannot start in this environment." >&2
  echo "  binary : $BIN" >&2
  echo "  This is usually a missing DLL search path (build/lib not on PATH)." >&2
  echo "  Refusing to launch; every run would fail instantly." >&2
  exit 1
fi

mkdir -p "$(dirname "$LOG")"
echo "launching $(basename "$MANIFEST") jobs=$JOBS -> $LOG"
nohup python "$ROOT/scripts/campaign_runner.py" --manifest "$MANIFEST" --jobs "$JOBS" >> "$LOG" 2>&1 &
RUNNER_PID=$!
# Record the PID so resume_all.sh can ask the OS whether this runner is alive,
# rather than parsing process command lines. Text matching for this was the
# cause of a silent no-op auto-resume: see the note in resume_all.sh.
CAMPAIGN_NAME="$(basename "${MANIFEST%.json}")"
mkdir -p "$ROOT/checkpoints/$CAMPAIGN_NAME"
echo "$RUNNER_PID" > "$ROOT/checkpoints/$CAMPAIGN_NAME/runner.pid"
echo "  pid=$RUNNER_PID"
