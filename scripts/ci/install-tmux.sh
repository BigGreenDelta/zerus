#!/usr/bin/env bash
# Build the pinned official tmux release for hosted input-contract tests.
set -euo pipefail
prefix="$(realpath -m "${1:?pass a disposable installation prefix}")"
tmux_version=3.7c
tmux_sha256=7c60cae9a0e25288e2e24750aafc9e8800fc7fd4555e447e1b29ee4201cfb3bf
work="$(mktemp -d "${RUNNER_TEMP:-/tmp}/zerus-tmux.XXXXXX")"
trap 'rm -rf "$work"' EXIT
curl -fsSL "https://github.com/tmux/tmux/releases/download/$tmux_version/tmux-$tmux_version.tar.gz" \
    -o "$work/tmux-$tmux_version.tar.gz"
(
    cd "$work"
    printf '%s  %s\n' "$tmux_sha256" "tmux-$tmux_version.tar.gz" | sha256sum --check -
    tar xzf "tmux-$tmux_version.tar.gz"
    cd "tmux-$tmux_version"
    # tmux requires explicit Unicode and allocator choices on macOS. The hosted
    # job installs both libraries; Linux keeps its original defaults.
    if [ "$(uname -s)" = Darwin ]; then
        ./configure --prefix="$prefix" --enable-utf8proc --enable-jemalloc
    else
        ./configure --prefix="$prefix"
    fi
    make --jobs=2
    make install
)
test "$("$prefix/bin/tmux" -V)" = "tmux $tmux_version"
