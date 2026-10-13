#!/usr/bin/env bash
# Local equivalent of the separate Arch desktop and Arch package CI jobs.
set -euo pipefail
cd "$(dirname "$0")/../.."
git rev-parse --verify HEAD > /dev/null
bash scripts/ci/gui.sh
bash scripts/build-arch-package.sh
