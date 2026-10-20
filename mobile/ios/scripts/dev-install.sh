#!/usr/bin/env bash
# Build the Zerus iOS app and install it on the attached iPhone (development
# signing) or on an iOS simulator. Runs on the Mac the phone is connected to. It
# never touches desktop installations, LaunchAgents, native agents or other apps.
#
# Usage: mobile/ios/scripts/dev-install.sh [--device UDID|NAME | --simulator [UDID]]
#            [--configuration Release|Debug] [--no-launch] [--build-only] [-- APP_ARGUMENTS...]
#
# Signing team (device builds): ZERUS_IOS_TEAM_ID, or ZERUS_DEVELOPMENT_TEAM in
#   mobile/ios/Config/Local.xcconfig or ~/.config/zerus-ios/Local.xcconfig.
# Provisioning uses the Apple ID signed in to Xcode, or an App Store Connect API key:
#   ZERUS_IOS_ASC_KEY_PATH, ZERUS_IOS_ASC_KEY_ID and ZERUS_IOS_ASC_ISSUER_ID.
# ZERUS_IOS_SOURCE_REVISION overrides the Git revision (remote builds);
# ZERUS_IOS_DERIVED_DATA overrides the shared DerivedData folder.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
device=""
simulator=0
simulator_id=""
configuration=""
launch=1
install=1
app_arguments=()
while [ $# -gt 0 ]; do
    case "$1" in
        --device) device=${2:?--device needs a UDID or name}; shift 2 ;;
        --simulator)
            simulator=1; shift
            if [ $# -gt 0 ] && [[ "$1" =~ ^[0-9A-Fa-f-]{36}$ ]]; then simulator_id=$1; shift; fi ;;
        --configuration) configuration=${2:?}; shift 2 ;;
        --no-launch) launch=0; shift ;;
        --build-only) install=0; launch=0; shift ;;
        --) shift; app_arguments=("$@"); break ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
[ "$simulator" = 0 ] || [ -z "$device" ] || { echo "Choose either --device or --simulator." >&2; exit 2; }
configuration=${configuration:-$([ "$simulator" = 1 ] && echo Debug || echo Release)}
case "$configuration" in Release|Debug) ;; *) echo "Configuration must be Release or Debug." >&2; exit 2 ;; esac
[ "$(uname -s)" = Darwin ] || { echo "Run this on the Mac the iPhone is attached to (see remote-dev-install.sh)." >&2; exit 2; }
command -v xcodebuild >/dev/null || { echo "Xcode is not installed." >&2; exit 2; }

device_build=0
[ "$simulator" = 1 ] || [ "$install" = 0 ] || device_build=1
team=${ZERUS_IOS_TEAM_ID:-}
for config in "$root/Config/Local.xcconfig" "$HOME/.config/zerus-ios/Local.xcconfig"; do
    if [ -z "$team" ] && [ -f "$config" ]; then
        team=$(sed -n 's/^[[:space:]]*ZERUS_DEVELOPMENT_TEAM[[:space:]]*=[[:space:]]*\([A-Z0-9]*\).*/\1/p' "$config" | head -n 1)
    fi
done
if [ "$device_build" = 1 ] && ! [[ "$team" =~ ^[A-Z0-9]{10}$ ]]; then
    echo "Set ZERUS_IOS_TEAM_ID, or ZERUS_DEVELOPMENT_TEAM in Config/Local.xcconfig (see Local.xcconfig.example)." >&2
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
    return wanted in (d.get("identifier"), d.get("hardwareProperties", {}).get("udid"), d.get("deviceProperties", {}).get("name"))
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

pick_simulator() {
    if [ -n "$simulator_id" ]; then echo "$simulator_id"; return; fi
    xcrun simctl list devices booted -j | python3 -c '
import json, sys
booted = [d["udid"] for runtime, items in json.load(sys.stdin)["devices"].items() if "iOS" in runtime for d in items]
if len(booted) != 1:
    sys.exit("Boot exactly one iOS simulator, or pass its UDID after --simulator.")
print(booted[0])'
}

target=""
if [ "$simulator" = 1 ]; then
    target=$(pick_simulator)
    destination="id=$target"
    platform=iphonesimulator
elif [ "$install" = 1 ]; then
    target=$(pick_device)
    destination="id=$target"
    platform=iphoneos
else
    destination="generic/platform=iOS"
    platform=iphoneos
fi

build_settings=(MARKETING_VERSION="$version" CURRENT_PROJECT_VERSION=0 ZERUS_BUILD_CHANNEL=local ZERUS_SOURCE_REVISION="$revision")
signing=()
if [ "$device_build" = 1 ]; then
    signing=(-allowProvisioningUpdates -allowProvisioningDeviceRegistration ZERUS_DEVELOPMENT_TEAM="$team")
    if [ -n "${ZERUS_IOS_ASC_KEY_PATH:-}" ]; then
        signing+=(-authenticationKeyPath "$ZERUS_IOS_ASC_KEY_PATH" -authenticationKeyID "${ZERUS_IOS_ASC_KEY_ID:?}"
                  -authenticationKeyIssuerID "${ZERUS_IOS_ASC_ISSUER_ID:?}")
    fi
elif [ "$simulator" = 0 ]; then
    signing=(CODE_SIGNING_ALLOWED=NO)
fi

# ZerusKit links the project task replica, which Xcode needs before it resolves the package.
replica_log="$cache/last-replica-build.log"
if ! "$root/scripts/build-replica.sh" > "$replica_log" 2>&1; then
    tail -n 20 "$replica_log" >&2
    echo "The task replica did not build; full log: $replica_log" >&2
    exit 1
fi

log="$cache/last-build.log"
echo "Building Zerus $version ($configuration, $platform, $revision)…"
if ! xcodebuild -project "$root/Zerus.xcodeproj" -scheme Zerus -configuration "$configuration" \
        -destination "$destination" -derivedDataPath "$derived" ${signing[@]+"${signing[@]}"} "${build_settings[@]}" build > "$log" 2>&1; then
    grep -E "error:|BUILD FAILED" "$log" | sort -u | head -n 40 >&2 || true
    echo "Build failed; full log: $log" >&2
    exit 1
fi
app="$derived/Build/Products/$configuration-$platform/Zerus.app"
[ -d "$app" ] || { echo "Built app not found at $app" >&2; exit 1; }
echo "Built $app"
[ "$install" = 1 ] || exit 0

if [ "$simulator" = 1 ]; then
    xcrun simctl install "$target" "$app"
    if [ "$launch" = 1 ]; then
        xcrun simctl launch --terminate-running-process "$target" app.zerus.mobile ${app_arguments[@]+"${app_arguments[@]}"} >/dev/null
        echo "Zerus is running in simulator $target."
    else
        echo "Zerus is installed in simulator $target."
    fi
    exit 0
fi

echo "Installing on the attached iPhone…"
xcrun devicectl device install app --device "$target" "$app" > "$cache/last-install.log" 2>&1 || {
    tail -n 20 "$cache/last-install.log" >&2
    exit 1
}
if [ "$launch" = 1 ]; then
    xcrun devicectl device process launch --device "$target" --terminate-existing app.zerus.mobile \
        ${app_arguments[@]+"${app_arguments[@]}"} > "$cache/last-launch.log" 2>&1 || {
        tail -n 20 "$cache/last-launch.log" >&2
        echo "Installed, but the launch failed. Unlock the phone and open Zerus." >&2
        exit 1
    }
    echo "Zerus $version-local ($revision) is installed and running."
else
    echo "Zerus $version-local ($revision) is installed."
fi
