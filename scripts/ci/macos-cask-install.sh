#!/usr/bin/env bash
# Install the signed candidate through a local tap exactly as users install the
# channel's cask (stable, or nightly with its release tag), then check Gatekeeper,
# setup, the bundled CLI and uninstallation.
# Runs only on ephemeral hosted runners; it installs into /Applications.
set -euo pipefail
cd "$(dirname "$0")/../.."
signed="$(cd "${1:?signed artifact directory}" && pwd)"
channel="${2:-stable}"
tag="${3:-}"
version="$(cat VERSION)"
read -r cask release team < <(python3 - "$signed/macos-info.json" "$channel" <<'PY'
import json, re, sys
sys.path.insert(0, "scripts")
from macos_release import cask_for
info = json.load(open(sys.argv[1]))
release = re.fullmatch(r"zerus-(.+)-macos-arm64\.zip", info["asset"]).group(1)
print(cask_for(sys.argv[2]), release, info["team_id"])
PY
)
asset="$signed/zerus-$release-macos-arm64.zip"
digest="$(shasum -a 256 "$asset" | cut -d ' ' -f 1)"
mkdir -p artifacts/homebrew
python3 - "$release" "$digest" "$channel" "$tag" "artifacts/homebrew/$cask.rb" <<'PY'
import sys
sys.path.insert(0, "scripts")
from macos_release import render_cask
release, digest, channel, tag, target = sys.argv[1:]
open(target, "w").write(render_cask(release, digest, channel, tag or None))
PY
brew tap-new --no-git zerus-ci/candidate > /dev/null
tap="$(brew --repository zerus-ci/candidate)"
mkdir -p "$tap/Casks"
# Lint the exact published cask from a tap, where Homebrew applies its cask rules.
cp "artifacts/homebrew/$cask.rb" "$tap/Casks/$cask.rb"
brew style --cask "zerus-ci/candidate/$cask"
# Then install the same cask with the not yet published URL pointing at the candidate bytes.
sed -e "s|^  url \".*\"$|  url \"file://$asset\"|" "artifacts/homebrew/$cask.rb" > "$tap/Casks/$cask.rb"
grep -q "^  url \"file://$asset\"$" "$tap/Casks/$cask.rb"
brew install --cask "zerus-ci/candidate/$cask"
app=/Applications/Zerus.app
python3 scripts/check-macos-bundle.py "$app" --version "$version" --release "$release" --team-id "$team" --selftest
home="${RUNNER_TEMP:?}/zerus-home"
mkdir -p "$home"
HOME="$home" "$(brew --prefix)/bin/zerus-setup"
test "$(readlink "$home/.local/bin/hgs")" = "$app/Contents/MacOS/hgs"
HOME="$home" "$home/.local/bin/hgs" --version
HOME="$home" "$home/.local/bin/hgs" ls --json --local > /dev/null
HOME="$home" "$(brew --prefix)/bin/zerus-setup" --remove
test ! -e "$home/.local/bin/hgs"
brew uninstall --cask "$cask"
test ! -e "$app"
echo "$cask $release: cask installation, Gatekeeper, setup and removal checks passed."
