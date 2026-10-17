#!/usr/bin/env bash
# CI helper: select the newest installed Xcode (hosted macOS runners keep several
# side by side) and require Xcode 26 or later, which the iOS client needs.
set -euo pipefail
xcode=$(ls -d /Applications/Xcode_*.app 2>/dev/null | sed -n 's#^/Applications/Xcode_\([0-9][0-9.]*\)\.app$#\1 &#p' |
    sort -V | tail -n 1 | cut -d' ' -f2)
if [ -n "$xcode" ]; then sudo xcode-select -s "$xcode"; fi
xcodebuild -version
major=$(xcodebuild -version | sed -n 's/^Xcode \([0-9]*\).*/\1/p')
if [ "${major:-0}" -lt 26 ]; then
    echo "::error::The iOS client needs Xcode 26 or later."
    exit 1
fi
