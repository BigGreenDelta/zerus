#!/usr/bin/env bash
# Build the project task replica (crates/replica-ffi) for ZerusKit and the iOS
# app: an XCFramework of its static library for the iPhone, the simulator and
# macOS (where `swift test` runs), and its UniFFI Swift bindings. Everything
# lands in mobile/ios/ZerusKit/Replica/, which git ignores; a build whose
# sources and toolchain did not change is skipped.
#
# Usage: mobile/ios/scripts/build-replica.sh [--bindings-only]
#   --bindings-only  only the Swift bindings and the C header, from a build for
#                    this host (it also runs on Linux, to check the Swift code)
#
# Environment: RUSTUP_TOOLCHAIN (default 1.99.1, as CI) and
#   ZERUS_REPLICA_TARGET_DIR (cargo's target directory, default Replica/cargo).
# The full build needs Xcode and these Rust targets:
#   rustup target add aarch64-apple-ios aarch64-apple-ios-sim x86_64-apple-ios \
#     aarch64-apple-darwin x86_64-apple-darwin
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
crate=$repo/crates/replica-ffi
out=$repo/mobile/ios/ZerusKit/Replica
export RUSTUP_TOOLCHAIN=${RUSTUP_TOOLCHAIN:-1.99.1}
target_dir=${ZERUS_REPLICA_TARGET_DIR:-$out/cargo}
targets=(aarch64-apple-ios aarch64-apple-ios-sim x86_64-apple-ios aarch64-apple-darwin x86_64-apple-darwin)
module=ZerusReplicaFFI
library=libzerus_replica_ffi.a

mode=full
case "${1:-}" in
  "") ;;
  --bindings-only) mode=bindings ;;
  *) echo "usage: $0 [--bindings-only]" >&2; exit 2 ;;
esac

hash() { if command -v sha256sum >/dev/null; then sha256sum; else shasum -a 256; fi; }

# The inputs that decide the outputs: the toolchain, the lock, the manifests and
# every source file of both crates, by path and content.
stamp=$(
  cd "$repo"
  {
    echo "$mode"
    cat "$here/build-replica.sh"
    rustc --version
    cat crates/replica-ffi/Cargo.lock crates/replica-ffi/Cargo.toml crates/replica-ffi/uniffi.toml crates/replica/Cargo.toml
    find crates/replica/src crates/replica-ffi/src -type f | LC_ALL=C sort | while read -r file; do
      echo "$file"
      cat "$file"
    done
  } | hash | cut -d' ' -f1
)
stamp_file=$out/stamp-$mode
if [ -f "$stamp_file" ] && [ "$(cat "$stamp_file")" = "$stamp" ] && [ -f "$out/Swift/ZerusReplica.swift" ] &&
   { [ "$mode" = bindings ] || [ -d "$out/$module.xcframework" ]; }; then
  echo "The task replica is up to date in $out."
  exit 0
fi

mkdir -p "$out"
cargo() { command cargo "$@" --locked --manifest-path "$crate/Cargo.toml" --target-dir "$target_dir"; }

# The bindings come from a debug build for this host (the release profile strips
# the UniFFI metadata), by the crate's own generator, as on Android.
cargo build --lib --features bindgen --bin uniffi-bindgen
host=$target_dir/debug/libzerus_replica_ffi.so
[ -f "$host" ] || host=$target_dir/debug/libzerus_replica_ffi.dylib
generated=$(mktemp -d)
trap 'rm -rf "$generated"' EXIT
# From the crate's folder, so its uniffi.toml names the module ZerusReplica.
(cd "$crate" && "$target_dir/debug/uniffi-bindgen" generate --library "$host" --language swift --no-format --out-dir "$generated")
rm -rf "$out/Swift" "$out/Headers"
mkdir -p "$out/Swift" "$out/Headers/$module"
mv "$generated/ZerusReplica.swift" "$out/Swift/"
mv "$generated/$module.h" "$out/Headers/$module/"
mv "$generated/$module.modulemap" "$out/Headers/$module/module.modulemap"

if [ "$mode" = full ]; then
  installed=$(rustup target list --installed)
  for target in "${targets[@]}"; do
    grep -qx "$target" <<<"$installed" || {
      echo "Rust target $target is missing: rustup target add --toolchain $RUSTUP_TOOLCHAIN ${targets[*]}" >&2
      exit 1
    }
  done
  # The objects carry the app's and the package's minimum versions.
  export IPHONEOS_DEPLOYMENT_TARGET=18.0 MACOSX_DEPLOYMENT_TARGET=15.0
  for target in "${targets[@]}"; do
    cargo rustc --release --lib --target "$target" --crate-type staticlib
  done
  slices=$out/slices
  rm -rf "$slices" "$out/$module.xcframework"
  mkdir -p "$slices/ios" "$slices/ios-simulator" "$slices/macos"
  cp "$target_dir/aarch64-apple-ios/release/$library" "$slices/ios/"
  lipo -create "$target_dir/aarch64-apple-ios-sim/release/$library" "$target_dir/x86_64-apple-ios/release/$library" \
    -output "$slices/ios-simulator/$library"
  lipo -create "$target_dir/aarch64-apple-darwin/release/$library" "$target_dir/x86_64-apple-darwin/release/$library" \
    -output "$slices/macos/$library"
  xcodebuild -create-xcframework \
    -library "$slices/ios/$library" -headers "$out/Headers" \
    -library "$slices/ios-simulator/$library" -headers "$out/Headers" \
    -library "$slices/macos/$library" -headers "$out/Headers" \
    -output "$out/$module.xcframework"
  rm -rf "$slices"
fi

echo "$stamp" >"$stamp_file"
echo "Built the task replica ($mode) in $out."
