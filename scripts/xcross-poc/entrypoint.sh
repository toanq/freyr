#!/usr/bin/env bash
set -euo pipefail

export PATH="/opt/flutter/bin:/root/.local/bin:/root/.swiftly/bin:${PATH}"

XIP=/work/Xcode.xip
APP_DIR=/work/app

echo "== xcross --version =="
xcross --version

echo "== xcross setup --yes =="
xcross setup --yes

echo "== xcross sdk install =="
xcross sdk install "$XIP"

cd "$APP_DIR"

echo "== flutter pub get =="
flutter pub get

echo "== xcross flutter doctor =="
xcross flutter doctor || true

echo "== xcross flutter build --release --ipa =="
xcross flutter build --release --ipa

echo "== done, looking for ipa =="
find "$APP_DIR/build" -iname "*.ipa" -o -iname "*.app" 2>/dev/null
