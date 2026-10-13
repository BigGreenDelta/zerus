#!/usr/bin/env bash
# Type-check hgs for x86_64-pc-windows-msvc from Linux or macOS, without the MSVC
# toolchain. Build scripts that compile C (bundled SQLite) get a stand-in compiler
# that writes empty objects: `cargo check` never links, so only the Rust code is
# checked. The Windows CI job still builds and tests the real binary.
#
# Usage: scripts/ci/check-windows-target.sh [check|clippy] [extra cargo args...]
# Needs: rustup target add x86_64-pc-windows-msvc (and for clippy, the same target
# for the 1.85.0 toolchain).
set -euo pipefail
cd "$(dirname "$0")/../.."
mode="${1:-check}"
[ "$#" -gt 0 ] && shift
target=x86_64-pc-windows-msvc
tools="$(mktemp -d "${TMPDIR:-/tmp}/hgs-windows-check.XXXXXX")"
trap 'rm -rf "$tools"' EXIT
cat >"$tools/cc" <<'EOF'
#!/bin/sh
# Create the object file cl.exe would have written (-Fo<path> or -o <path>).
out=
previous=
for argument in "$@"; do
    case "$argument" in -Fo*|/Fo*) out="${argument#???}" ;; esac
    [ "$previous" = "-o" ] && out="$argument"
    previous="$argument"
done
[ -n "$out" ] && : >"$out"
exit 0
EOF
cat >"$tools/ar" <<'EOF'
#!/bin/sh
# Create the library lib.exe would have written (-out:<path>).
for argument in "$@"; do
    case "$argument" in -out:*|/OUT:*|/out:*) : >"${argument#*:}" ;; esac
done
exit 0
EOF
chmod +x "$tools/cc" "$tools/ar"
export CC_x86_64_pc_windows_msvc="$tools/cc" AR_x86_64_pc_windows_msvc="$tools/ar"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$PWD/target/windows-check}"
case "$mode" in
    check) cargo check --locked --target "$target" --all-targets "$@" ;;
    clippy) cargo +1.85.0 clippy --locked --target "$target" --all-targets "$@" -- -D warnings ;;
    *) echo "usage: $0 [check|clippy] [cargo args...]" >&2; exit 2 ;;
esac
