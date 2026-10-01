#!/usr/bin/env bash
#
# Reproduce this study's build environment from a clean MSYS2 installation.
#
# This replaces the specification's `setup_wsl.sh`. WSL2 is not installable in
# the target environment without administrator elevation and a reboot; MSYS2 /
# MinGW64 is the officially supported ns-3 path on Windows. See
# docs/DESIGN_DEVIATIONS.md D001.
#
# Run from an MSYS2 MinGW64 shell:
#     bash environment/setup_msys2.sh
#
# It is deliberately NOT idempotent-by-force: it refuses to upgrade packages
# that are already installed at a different version, because MSYS2 is a rolling
# release and a silent upgrade would change the toolchain out from under a
# frozen campaign. Version drift is reported, never applied.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK="$ROOT/environment/pacman_lock.txt"

NS3_VERSION="3.46.1"
NS3_COMMIT="51387bce7e5f5c57aa080612a4ed690bf33d0c92"
NS3_ARCHIVE_SHA256="52A3D4B533779781234B758D6C608D0C6689E4C56266F6A7F3FB02AF887B03CF"
NS3_URL="https://gitlab.com/nsnam/ns-3-dev/-/archive/ns-3.46.1/ns-3-dev-ns-3.46.1.tar.bz2"

echo "SCR environment setup"
echo "  project root : $ROOT"
echo "  ns-3         : $NS3_VERSION ($NS3_COMMIT)"
echo

# ---------------------------------------------------------------------------
# 1. Required MSYS2 packages
# ---------------------------------------------------------------------------
PACKAGES=(
  mingw-w64-x86_64-gcc
  mingw-w64-x86_64-cmake
  mingw-w64-x86_64-ninja
  mingw-w64-x86_64-gsl
  mingw-w64-x86_64-sqlite3
)

echo "== checking packages =="
missing=()
for p in "${PACKAGES[@]}"; do
  if pacman -Q "$p" >/dev/null 2>&1; then
    printf "  present  %s\n" "$(pacman -Q "$p")"
  else
    printf "  MISSING  %s\n" "$p"
    missing+=("$p")
  fi
done

if [ ${#missing[@]} -gt 0 ]; then
  echo
  echo "Install the missing packages, then re-run this script:"
  echo "    pacman -S --needed ${missing[*]}"
  exit 1
fi

# ---------------------------------------------------------------------------
# 2. Compare against the recorded lock
# ---------------------------------------------------------------------------
if [ -f "$LOCK" ]; then
  echo
  echo "== comparing against $LOCK =="
  drift=0
  while read -r name version; do
    [ -z "$name" ] && continue
    actual="$(pacman -Q "$name" 2>/dev/null | awk '{print $2}')"
    if [ "$actual" != "$version" ]; then
      printf "  DRIFT   %-34s locked=%-14s installed=%s\n" "$name" "$version" "$actual"
      drift=1
    fi
  done < "$LOCK"
  if [ "$drift" -eq 1 ]; then
    echo
    echo "WARNING: the toolchain differs from the version that produced the"
    echo "recorded results. MSYS2 is a rolling release, so this is expected over"
    echo "time. Results generated now are NOT bit-comparable with the frozen"
    echo "campaign; record the drift in DESIGN_DEVIATIONS.md before proceeding."
  else
    echo "  all packages match the lock"
  fi
fi

# ---------------------------------------------------------------------------
# 3. ns-3 source
# ---------------------------------------------------------------------------
echo
echo "== ns-3 source =="
NS3_DIR="$ROOT/vendor/ns-3-dev-ns-$NS3_VERSION"
ARCHIVE="$ROOT/vendor/downloads/ns-3-dev-ns-$NS3_VERSION.tar.bz2"

if [ -d "$NS3_DIR" ]; then
  echo "  already extracted at vendor/ns-3-dev-ns-$NS3_VERSION"
else
  mkdir -p "$ROOT/vendor/downloads"
  if [ ! -f "$ARCHIVE" ]; then
    echo "  downloading $NS3_URL"
    curl -L -o "$ARCHIVE" "$NS3_URL"
  fi
  echo "  verifying checksum"
  actual="$(sha256sum "$ARCHIVE" | awk '{print toupper($1)}')"
  if [ "$actual" != "$NS3_ARCHIVE_SHA256" ]; then
    echo "  CHECKSUM MISMATCH"
    echo "    expected $NS3_ARCHIVE_SHA256"
    echo "    actual   $actual"
    echo "  Refusing to extract. Do not proceed with an unverified archive."
    exit 1
  fi
  echo "  checksum OK; extracting"
  ( cd "$ROOT/vendor" && tar -xjf "downloads/ns-3-dev-ns-$NS3_VERSION.tar.bz2" )
fi

# ---------------------------------------------------------------------------
# 4. Sync project sources and configure
# ---------------------------------------------------------------------------
echo
echo "== syncing project sources =="
bash "$ROOT/scripts/sync_sources.sh"

echo
echo "== configuring ns-3 =="
cd "$NS3_DIR"
python ./ns3 configure \
  --enable-modules=aodv,olsr,dsdv,wifi,applications,internet,mobility,propagation,flow-monitor,stats,point-to-point,csma,scr \
  --enable-tests --enable-examples

echo
echo "== building (use -j2 to match the registered build; raise if you have RAM) =="
python ./ns3 build -j2

echo
echo "Setup complete."
echo "Verify with:"
echo "  export PATH=\"/c/msys64/mingw64/bin:/c/msys64/usr/bin:\$PATH:$NS3_DIR/build/lib\""
echo "  $NS3_DIR/build/utils/ns3.46.1-test-runner-default.exe --suite=scr-ledger"
echo "  $NS3_DIR/build/utils/ns3.46.1-test-runner-default.exe --suite=scr-service-headers"
echo "  python $ROOT/scripts/audit_antifake.py"
