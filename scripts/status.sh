#!/usr/bin/env bash
# One-glance status for every campaign.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
echo "SCR campaigns  --  $(date '+%Y-%m-%d %H:%M:%S')"
echo
for d in checkpoints/*/; do
  name="$(basename "$d")"
  [ -f "$d/PROGRESS.txt" ] || continue
  case "$name" in stage2*|stage7_pilot*|*SUPERSEDED*|*INVALID*|hb_test|breaker_test) continue;; esac
  echo "== $name"
  grep -E "progress |runs |mean run|ETA |in flight|heartbeat" "$d/PROGRESS.txt" | sed 's/^/   /'
  echo
done
running=$(ps -W 2>/dev/null | grep -cE "scr-manet|scr-c1-outage")
# `ps -W` reports only the executable path, so the runner's manifest argument is
# invisible to it. Ask Windows for the actual command lines instead.
runners=$(powershell -NoProfile -Command   "(Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { \$_.CommandLine -like '*campaign_runner*' } | Measure-Object).Count"   2>/dev/null | tr -d '

 ')
# PowerShell can emit stray whitespace/BOM; keep digits only so the numeric
# comparisons below cannot fail on an invisible character.
runners=$(printf '%s' "$runners" | tr -cd '0-9')
runners=${runners:-0}
running=$(printf '%s' "$running" | tr -cd '0-9')
running=${running:-0}
echo "simulations running : $running"
echo "runners alive       : $runners"
if [ "$running" -eq 0 ] && [ "$runners" -eq 0 ]; then
  echo
  echo "NOTHING IS RUNNING. Resume with:  bash scripts/resume_all.sh"
elif [ "$runners" -eq 0 ]; then
  echo
  echo "WARNING: simulations are running but no runner owns them (orphans)."
  echo "They will finish, but no further runs will start. Resume with:"
  echo "  bash scripts/resume_all.sh"
fi
