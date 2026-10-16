#!/usr/bin/env bash
# Build the Zerus iOS app with development signing and install it on an attached
# iPhone. Runs on the Mac that the phone is connected to. It never touches desktop
# installations, LaunchAgents, native agents or other apps.
#
# Usage: mobile/ios/scripts/dev-install.sh [--device UDID|NAME] [--configuration Release|Debug]
#                                          [--no-launch] [--build-only]
# Signing team: ZERUS_IOS_TEAM_ID, or ZERUS_DEVELOPMENT_TEAM in mobile/ios/Config/Local.xcconfig.
# Optional: ZERUS_IOS_SOURCE_REVISION overrides the Git revision (remote builds),
#           ZERUS_IOS_DERIVED_DATA overrides the shared DerivedData folder.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
device=""
configuration=Release
launch=1
install=1
while [ $# -gt 0 ]; do
    case "$1" in
        --device) device=${2:?--device needs a UDID or name}; shift 2 ;;
        --configuration) configuration=${2:?}; shift 2 ;;
        --no-launch) launch=0; shift ;;
        --build-only) install=0; launch=0; shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
case "$configuration" in Release|Debug) ;; *) echo "Configuration must be Release or Debug." >&2; exit 2 ;; esac
[ "$(uname -s)" = Darwin ] || { echo "Run this on the Mac the iPhone is attached to (see remote-dev-install.sh)." >&2; exit 2; }
command -v xcodebuild >/dev/null || { echo "Xcode is not installed." >&2; exit 2; }

team=${ZERUS_IOS_TEAM_ID:-}
if [ -z "$team" ] && [ -f "$root/Config/Local.xcconfig" ]; then
    team=$(sed -n 's/^[[:space:]]*ZERUS_DEVELOPMENT_TEAM[[:space:]]*=[[:space:]]*\([A-Z0-9]*\).*/\1/p' "$root/Config/Local.xcconfig" | head -n 1)
fi
if [ "$install" = 1 ] && ! [[ "$team" =~ ^[A-Z0-9]{10}$ ]]; then
    echo "Set ZERUS_IOS_TEAM_ID or ZERUS_DEVELOPMENT_TEAM in Config/Local.xcconfig (see Local.xcconfig.example)." >&2
    exit 2
fi

version=$(tr -d '[:space:]' < "$root/DEV_VERSION")
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "DEV_VERSION must be X.Y.Z." >&2; exit 2; }
revision=${ZERUS_IOS_SOURCE_REVISION:-}
if [ -z "$revision" ]; then
    if revision=$(git -C "$root" rev-parse --short=12 HEAD 2>/dev/null); then
        [ -z "$(git -C "$root" status --porcelain -- . 2>/dev/null)" ] || revision="$revision+dirty"
    else
        revision=unknown
    fi
fi
[[ "$revision" =~ ^[0-9A-Za-z.+_-]{1,64}$ ]] || { echo "Invalid source revision." >&2; exit 2; }

cache="$HOME/Library/Caches/zerus-ios"
derived=${ZERUS_IOS_DERIVED_DATA:-$cache/DerivedData}
mkdir -p "$cache"
lock="$cache/build.lock"
for _ in $(seq 1 360); do
    if mkdir "$lock" 2>/dev/null; then
        echo $$ > "$lock/pid"
        break
    fi
    holder=$(cat "$lock/pid" 2>/dev/null || true)
    if [ -n "$holder" ] && ! kill -0 "$holder" 2>/dev/null; then
        rm -rf "$lock"
        continue
    fi
    [ "${announced:-}" ] || { echo "Waiting for another Zerus iOS build (pid ${holder:-unknown})…"; announced=1; }
    sleep 5
done
[ -d "$lock" ] && [ "$(cat "$lock/pid" 2>/dev/null)" = $$ ] || { echo "Timed out waiting for the build lock." >&2; exit 1; }
trap 'rm -rf "$lock"' EXIT

pick_device() {
    local json
    json=$(mktemp "${TMPDIR:-/tmp}/zerus-devices.XXXXXX")
    xcrun devicectl list devices --json-output "$json" >/dev/null
    python3 - "$json" "$device" <<'PY'
import json, sys
devices = json.load(open(sys.argv[1]))["result"]["devices"]
wanted = sys.argv[2]
def matches(d):
    hw, props = d.get("hardwareProperties", {}), d.get("deviceProperties", {})
    return wanted in (d.get("identifier"), hw.get("udid"), props.get("name"))
candidates = [d for d in devices if d.get("hardwareProperties", {}).get("platform") == "iOS"
              and d.get("hardwareProperties", {}).get("reality") == "physical"
              and d.get("connectionProperties", {}).get("tunnelState") == "connected"]
if wanted:
    candidates = [d for d in devices if matches(d)]
    if not candidates: sys.exit("No device matches --device.")
    if candidates[0].get("connectionProperties", {}).get("tunnelState") != "connected":
        sys.exit("The selected device is not connected. Unlock it and keep it attached.")
elif len(candidates) != 1:
    sys.exit("Connect exactly one unlocked iPhone, or choose one with --device." if not candidates
             else "Several iPhones are connected; choose one with --device.")
print(candidates[0]["hardwareProperties"]["udid"])
PY
    rm -f "$json"
}

udid=""
destination="generic/platform=iOS"
if [ "$install" = 1 ]; then
    udid=$(pick_device)
    destination="id=$udid"
fi

log="$cache/last-build.log"
echo "Building Zerus $version ($configuration, $revision)…"
build_settings=(MARKETING_VERSION="$version" CURRENT_PROJECT_VERSION=0 ZERUS_BUILD_CHANNEL=local ZERUS_SOURCE_REVISION="$revision")
signing=(-allowProvisioningUpdates -allowProvisioningDeviceRegistration ZERUS_DEVELOPMENT_TEAM="$team")
[ "$install" = 1 ] || signing=(CODE_SIGNING_ALLOWED=NO)
if ! xcodebuild -project "$root/Zerus.xcodeproj" -scheme Zerus -configuration "$configuration" \
        -destination "$destination" -derivedDataPath "$derived" "${signing[@]}" "${build_settings[@]}" build > "$log" 2>&1; then
    grep -E "error:|BUILD FAILED" "$log" | sort -u | head -n 40 >&2 || true
    echo "Build failed; full log: $log" >&2
    exit 1
fi
app="$derived/Build/Products/$configuration-iphoneos/Zerus.app"
[ -d "$app" ] || { echo "Built app not found at $app" >&2; exit 1; }
echo "Built $app"
[ "$install" = 1 ] || exit 0

echo "Installing on the attached iPhone…"
xcrun devicectl device install app --device "$udid" "$app" > "$cache/last-install.log" 2>&1 || {
    tail -n 20 "$cache/last-install.log" >&2
    exit 1
}
if [ "$launch" = 1 ]; then
    xcrun devicectl device process launch --device "$udid" --terminate-existing app.zerus.mobile > "$cache/last-launch.log" 2>&1 || {
        tail -n 20 "$cache/last-launch.log" >&2
        echo "Installed, but the launch failed. Unlock the phone and open Zerus." >&2
        exit 1
    }
fi
if [ "$launch" = 1 ]; then echo "Zerus $version-local ($revision) is installed and running."
else echo "Zerus $version-local ($revision) is installed."; fi
