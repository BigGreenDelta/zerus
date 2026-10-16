#!/usr/bin/env bash
# Build and install this worktree's iOS app on the Mac that has the test iPhone
# attached, from another computer (for example an agent on Linux). Uncommitted
# changes are included; the build reports the local revision with "+dirty".
#
# Usage: mobile/ios/scripts/remote-dev-install.sh SSH_HOST [dev-install.sh options...]
#
# The Mac needs Xcode and a signing team in ~/.config/zerus-ios/Local.xcconfig
# (or ZERUS_IOS_TEAM_ID in its login environment). SSH runs with BatchMode, so
# keys must already be authorized; nothing is prompted.
set -euo pipefail

host=${1:?Usage: remote-dev-install.sh SSH_HOST [dev-install.sh options...]}
shift
ios=$(cd "$(dirname "$0")/.." && pwd)
repo=$(git -C "$ios" rev-parse --show-toplevel)
revision=$(git -C "$repo" rev-parse --short=12 HEAD)
[ -z "$(git -C "$repo" status --porcelain -- mobile/ios)" ] || revision="$revision+dirty"
# One scratch copy per worktree on the Mac, named after the worktree folder and path.
slug=$(basename "$repo" | tr -c 'A-Za-z0-9._-' '-')-$(printf '%s' "$repo" | shasum | cut -c1-8)
remote_dir="Library/Caches/zerus-ios/src/$slug"

ssh -o BatchMode=yes "$host" "mkdir -p '$remote_dir'"
rsync -a --delete -e "ssh -o BatchMode=yes" \
    --exclude .build/ --exclude .swiftpm/ --exclude build/ --exclude DerivedData/ --exclude xcuserdata/ \
    --exclude Config/Local.xcconfig \
    "$ios/" "$host:$remote_dir/"

quoted=""
for argument in "$@"; do quoted+=" $(printf '%q' "$argument")"; done
ssh -o BatchMode=yes "$host" \
    "zsh -lc 'cd \"\$HOME/$remote_dir\" && ZERUS_IOS_SOURCE_REVISION=$revision ./scripts/dev-install.sh$quoted'"
