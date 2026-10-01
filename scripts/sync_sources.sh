#!/usr/bin/env bash
# Sync project-owned ns-3 sources into the private vendor tree before building.
#
# Canonical sources live in ASTRA_PROJECT_ROOT/ns3/.
# The ns-3 build system requires them inside the ns-3 tree, so they are COPIED in.
# Rationale for copying rather than symlinking: Windows symlink creation needs elevation,
# and links could redirect writes outside the project.
#
# The vendor tree is treated as build output. Never edit sources there directly; edits
# there are overwritten by this script.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NS3="$ROOT/vendor/ns-3-dev-ns-3.46.1"

if [ ! -d "$NS3" ]; then
  echo "ERROR: ns-3 tree not found at $NS3" >&2
  exit 1
fi

echo "ASTRA_PROJECT_ROOT = $ROOT"
echo "ns-3 tree          = $NS3"

# --- scenarios -> ns-3 scratch/ ---
# ns-3 treats each scratch SUBDIRECTORY as a single program and links every .cc in it
# together. Two files each defining main() in one subdirectory is a configure error.
# Each scenario therefore gets its own subdirectory named after the source file.
if [ -d "$ROOT/ns3/scenarios" ]; then
  rm -rf "$NS3/scratch/scr" 2>/dev/null || true   # remove the old flat-collision layout
  count=0
  for f in "$ROOT"/ns3/scenarios/*.cc; do
    [ -e "$f" ] || continue
    base="$(basename "$f" .cc)"
    mkdir -p "$NS3/scratch/$base"
    cp -f "$f" "$NS3/scratch/$base/"
    echo "  scenario: $base"
    count=$((count + 1))
  done
  echo "synced $count scenario source(s) -> scratch/<name>/"
fi

# --- contrib module -> ns-3 contrib/ ---
if [ -d "$ROOT/ns3/contrib/scr" ] && [ -n "$(ls -A "$ROOT/ns3/contrib/scr" 2>/dev/null)" ]; then
  mkdir -p "$NS3/contrib"
  rm -rf "$NS3/contrib/scr"
  cp -r "$ROOT/ns3/contrib/scr" "$NS3/contrib/scr"
  echo "synced contrib module -> contrib/scr/"
else
  echo "contrib/scr: empty or absent (expected until Stage 3 fork is created)"
fi

echo "sync complete."
