#!/usr/bin/env bash
# Build, test and assemble the unsigned Apple Silicon Zerus.app against the pinned
# official Qt. Uses no secrets; the macos-signing job signs the checked result.
set -euo pipefail
cd "$(dirname "$0")/../.."
qt="${ZERUS_QT_PREFIX:?set ZERUS_QT_PREFIX to the prefix from install-qt-macos.py}"
output="${ZERUS_MACOS_OUTPUT:-$PWD/artifacts/macos-unsigned}"
version="$(cat VERSION)"
# A nightly passes its release version; stable bundles use the product version.
release="${ZERUS_RELEASE_VERSION:-$version}"
jobs="${ZERUS_BUILD_JOBS:-3}"
command -v tmux > /dev/null || { echo 'Install tmux before bundle self-tests.' >&2; exit 1; }
export MACOSX_DEPLOYMENT_TARGET=14.0
mkdir -p artifacts/test-results
cargo build --locked --release
cargo build --locked --release --manifest-path services/mobile/connector/Cargo.toml
rm -rf artifacts/macos-licenses
python3 scripts/collect-licenses.py artifacts/macos-licenses \
    --manifest-path Cargo.toml --manifest-path services/mobile/connector/Cargo.toml
cmake_args=(-G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$qt"
            -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_OSX_ARCHITECTURES=arm64)
if [ -n "${ZERUS_DEV_BUILD_NUMBER:-}" ]; then
    cmake_args+=("-DZERUS_DEV_BUILD_NUMBER=$ZERUS_DEV_BUILD_NUMBER")
fi
# Release bundles always configure from scratch: a cached build directory can name
# an SDK that an Xcode update has since removed.
rm -rf .ci-build/macos-tests
# The shipped GUI is tested against exactly the Qt it bundles.
cmake -S tray -B .ci-build/macos-tests "${cmake_args[@]}" -DBUILD_TESTING=ON
cmake --build .ci-build/macos-tests --parallel "$jobs"
QT_QPA_PLATFORM=offscreen \
    ctest --test-dir .ci-build/macos-tests --parallel "$jobs" --timeout 240 --output-on-failure \
    --output-junit "$PWD/artifacts/test-results/macos-bundle-qt.xml"
python3 scripts/package-macos.py --qt "$qt" --app .ci-build/macos-tests/hgs-tray.app \
    --cli target/release/hgs --connector services/mobile/connector/target/release/zerus-mobile-connector \
    --licenses artifacts/macos-licenses --version "$version" --release "$release" \
    --output "$output"
python3 scripts/check-macos-bundle.py "$output/Zerus.app" --version "$version" --release "$release" --selftest --source-commit "$(git rev-parse HEAD)"
# ditto keeps the framework symlinks and permissions that artifact uploads drop.
ditto -c -k --keepParent "$output/Zerus.app" "$output/Zerus-unsigned.zip"
rm -rf "$output/Zerus.app"
