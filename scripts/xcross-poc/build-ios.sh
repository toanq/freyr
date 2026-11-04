#!/usr/bin/env bash
# Builds an unsigned iOS .ipa via xcross/Docker, with firebase_core and
# firebase_messaging compiled out. Needed because xcross's SwiftPM plugin
# vendoring drops firebase_core's pubspec.yaml, which its Package.swift reads
# two directories up -- crashes the manifest compiler (see
# scripts/xcross-poc/no-firebase.patch for the exact diff this applies).
#
# Real project files are never touched: this works on a throwaway rsync copy.
#
# Usage: scripts/xcross-poc/build-ios.sh [path-to-Xcode.xip]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
XIP="${1:-$ROOT/../Xcode_27.2.xip}"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

if [ ! -f "$XIP" ]; then
  echo "Xcode.xip not found at $XIP (pass its path as \$1)" >&2
  exit 1
fi

echo "== copying app/ to scratch build dir =="
rsync -a --exclude .dart_tool --exclude build "$ROOT/app/" "$SCRATCH/"

echo "== applying no-firebase.patch =="
git -C "$SCRATCH" apply --unsafe-paths -p1 "$ROOT/scripts/xcross-poc/no-firebase.patch"
rm -f "$SCRATCH/pubspec.lock"

echo "== building xcross-poc image =="
docker build -t xcross-poc "$ROOT/scripts/xcross-poc"

echo "== running build =="
docker run --rm --privileged \
  -v "$XIP:/work/Xcode.xip:ro" \
  -v "$SCRATCH:/work/app" \
  xcross-poc

echo "== ipa =="
find "$SCRATCH/build" -iname '*.ipa'
echo "(built from a scratch copy, not app/ -- copy it out before the scratch dir is removed)"
