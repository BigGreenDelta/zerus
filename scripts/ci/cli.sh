#!/usr/bin/env bash
# Run credential-free CLI checks. Every terminal fixture owns a private socket.
set -euo pipefail
cd "$(dirname "$0")/../.."
phase=${1:-all}
case "$phase" in all|debug|remaining) ;; *) echo "Unknown CLI phase" >&2; exit 2 ;; esac
mkdir -p artifacts/test-results
# Tests launched from an agent must not inherit its account/run selection.
while IFS= read -r variable; do
    case "$variable" in HGS_*) unset "$variable" ;; esac
done < <(compgen -e)
unset HGS_CLAUDE_TEST_BIN HGS_NATIVE_RECOVERY HGS_TEST_LEGACY_STATE HGS_TEST_LEGACY_BIN
python3 - <<'PY'
import re, subprocess
version = subprocess.check_output(['tmux', '-V'], text=True).strip()
match = re.match(r'tmux (\d+)\.(\d+)', version)
if not match or tuple(map(int, match.groups())) < (3, 7):
    raise SystemExit('Full CLI integration checks require tmux 3.7+ for verified input-mode inspection: ' + version)
print('Terminal fixture toolchain: ' + version)
PY
if [ "$phase" != remaining ]; then
cargo test --locked 2>&1 | tee artifacts/test-results/rust.log
cargo build --locked
# The native stand-in agent of tests/test_fake_agent.py, tests/test_backend_contract.py
# and later terminal suites.
cargo build --locked --example fake_agent
fi
if [ "$phase" != debug ]; then
export HGS_TEST_BIN="$PWD/target/debug/hgs"
bash tests/test_hgs.sh 2>&1 | tee artifacts/test-results/cli-smoke.log
python3 scripts/ci/python-tests.py 2>&1 | tee artifacts/test-results/python.log
cargo build --release --locked
printf 'CLI checks and release build passed.\n'
fi
