#!/usr/bin/env bash
#
# Build ns-3 ONLY when no simulation is running.
#
# Rebuilding mid-campaign replaces the shared libraries underneath processes
# that have already mapped them. It has damaged this study twice:
#
#   R008 note  geometry confirmation runs 2-9 were corrupted this way and had
#              to be discarded and repeated.
#   2026-09-09 a rebuild was started 17 minutes into the clean Stage 7 pilot.
#              The link failed because Windows held the DLL open, which is the
#              only reason the pilot survived. On a filesystem with permissive
#              replace semantics the campaign would have been silently ruined,
#              and the results would have looked perfectly plausible.
#
# The second case is the dangerous one: the protection was accidental. This
# script makes it deliberate.
#
# Usage:
#   bash scripts/safe_build.sh [extra ns3 build args]
#   FORCE_BUILD=1 bash scripts/safe_build.sh      # override, with a warning

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NS3_DIR="$ROOT/vendor/ns-3-dev-ns-3.46.1"

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH:$NS3_DIR/build/lib"

# Any running scenario binary counts, not just scr-manet: a fixture, a sweep or
# a C1 outage run maps the same libraries.
running="$(ps -W 2>/dev/null | grep -E "ns3\.46\.1-scr-|ns3\.46\.1-test-runner" | grep -v grep || true)"

if [ -n "$running" ]; then
  echo "REFUSING TO BUILD: simulation processes are running." >&2
  echo >&2
  echo "$running" | sed 's/^/  /' >&2
  echo >&2
  echo "Rebuilding now would replace shared libraries under these processes and" >&2
  echo "silently corrupt their results. Wait for them to finish, or stop them." >&2
  echo >&2
  echo "Check progress:  tail -f $ROOT/logs/*.log" >&2
  if [ "${FORCE_BUILD:-0}" != "1" ]; then
    exit 1
  fi
  echo "FORCE_BUILD=1 set; proceeding anyway. Any results produced by the" >&2
  echo "processes above must be discarded." >&2
fi

# R025: a live runner or supervisor is as dangerous as a live simulation. The
# check above only sees simulations already running; a runner or supervisor
# will START new ones during the build, which then load half-rebuilt libraries.
# On 2026-09-11 the at-logon watchdog restarted both campaigns at 10:01, while
# a rebuild was pending, and nothing noticed.
# Match by process NAME as well as command line, and exclude the checker's own
# PID. The first version matched on command line alone, and the PowerShell
# process running the check contains "*campaign_runner*" in ITS OWN command
# line, so it always counted itself: a guard that fires on nothing, which only
# teaches the operator to reach for FORCE_BUILD.
live_runners=$(powershell -NoProfile -Command '@(Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -ne $PID -and (($_.Name -eq "python.exe" -and $_.CommandLine -like "*campaign_runner*") -or ($_.Name -eq "bash.exe" -and $_.CommandLine -like "*supervise.sh*") -or ($_.Name -eq "python.exe" -and $_.CommandLine -like "*supervise.py*")) }).Count' 2>/dev/null | tr -cd '0-9')
live_runners=${live_runners:-0}
if [ "$live_runners" -gt 0 ] && [ "${FORCE_BUILD:-0}" != "1" ]; then
  echo "REFUSING TO BUILD: $live_runners campaign runner/supervisor process(es) alive." >&2
  echo "They would start simulations against a half-rebuilt tree. Stop them first." >&2
  exit 1
fi

# The watchdog must not fire mid-build either: it would start the supervisor,
# which would start simulations against libraries being relinked. Disable it for
# the duration and restore it on ANY exit, including a failed build.
TASK="SCR_ResumeCampaigns_Watchdog"
task_state=$(powershell -NoProfile -Command '(Get-ScheduledTask -TaskName "SCR_ResumeCampaigns_Watchdog" -ErrorAction SilentlyContinue).State' 2>/dev/null | tr -cd 'A-Za-z')
if [ "$task_state" = "Ready" ]; then
  powershell -NoProfile -Command 'Disable-ScheduledTask -TaskName "SCR_ResumeCampaigns_Watchdog" | Out-Null' >/dev/null 2>&1
  echo "watchdog task disabled for the build; it will be re-enabled on exit"
  trap 'powershell -NoProfile -Command "Enable-ScheduledTask -TaskName SCR_ResumeCampaigns_Watchdog | Out-Null" >/dev/null 2>&1; echo "watchdog task re-enabled"' EXIT
fi

bash "$ROOT/scripts/sync_sources.sh"

cd "$NS3_DIR"
python ./ns3 build "$@"
