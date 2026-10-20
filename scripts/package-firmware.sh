#!/usr/bin/env bash
# Packages an `idf.py build` output into a flash kit for the web console:
#   synora-<version>-flashkit.zip = flasher_args.json + every image it lists
#   (bootloader, partition table, otadata, app, srmodels), paths unchanged.
# Upload the zip on the web console's Firmware tab: the app image becomes the
# OTA build and the rest enables "Flash via USB".
#
# Usage: scripts/package-firmware.sh [build-dir] [version] [out-dir]
#   build-dir  default: firmware-synora/build
#   version    default: the version embedded in the app image (PROJECT_VER)
#   out-dir    default: current directory
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$repo_root/firmware-synora/build}"
version="${2:-}"
out_dir="${3:-$PWD}"

if [ ! -f "$build_dir/flasher_args.json" ]; then
  echo "error: $build_dir/flasher_args.json not found -- run idf.py build first" >&2
  exit 1
fi

python3 - "$build_dir" "$version" "$out_dir" <<'PY'
import json, os, sys, zipfile

build_dir, version, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
with open(os.path.join(build_dir, "flasher_args.json")) as f:
    args = json.load(f)

app = args.get("app", {}).get("file")
if not app:
    sys.exit("error: flasher_args.json has no app entry")

if not version:
    # esp_app_desc_t follows the 24-byte image header + 8-byte first segment
    # header; its version[32] field sits 16 bytes in (after magic,
    # secure_version and reserved words).
    with open(os.path.join(build_dir, app), "rb") as f:
        head = f.read(32 + 48)
    if int.from_bytes(head[32:36], "little") != 0xABCD5432:
        sys.exit("error: could not read esp_app_desc from the app image -- pass a version")
    version = head[48:80].split(b"\0", 1)[0].decode()

files = sorted(set(args["flash_files"].values()))
missing = [p for p in files if not os.path.isfile(os.path.join(build_dir, p))]
if missing:
    sys.exit("error: missing build outputs: " + ", ".join(missing))

os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, f"synora-{version}-flashkit.zip")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(build_dir, "flasher_args.json"), "flasher_args.json")
    for p in files:
        z.write(os.path.join(build_dir, p), p)

print(f"version {version}")
for offset, p in sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
    print(f"  {offset:>10}  {p}  ({os.path.getsize(os.path.join(build_dir, p))} bytes)")
print(out)
PY
