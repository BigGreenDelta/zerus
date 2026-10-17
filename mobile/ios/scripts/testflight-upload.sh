#!/usr/bin/env bash
# Archive the iOS app and upload it to App Store Connect. Used by the trusted main
# workflow ios-testflight.yml; it can also run on a maintainer's Mac.
#
# Environment:
#   CHANNEL        dev (TestFlight internal testing only) or stable (App Store candidate)
#   BUILD_NUMBER   unique increasing build number (the CI sequence for dev)
#   SOURCE_COMMIT  the exact source revision being archived
#   TEAM_ID        Apple Developer team ID
#   ASC_KEY        App Store Connect API private key (.p8 contents)
#   ASC_KEY_ID     its key ID
#   ASC_ISSUER_ID  its issuer ID
# Signing is automatic and cloud-managed through the API key; no certificate or
# profile is stored in the repository or the runner keychain.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=${RUNNER_TEMP:-$(mktemp -d)}/app-store-connect
case "${CHANNEL:-}" in
    dev) version=$(tr -d '[:space:]' < "$root/DEV_VERSION"); internal=true ;;
    stable)
        [ -f "$root/VERSION" ] || { echo "Add mobile/ios/VERSION with the stable release version first." >&2; exit 2; }
        version=$(tr -d '[:space:]' < "$root/VERSION"); internal=false ;;
    *) echo "CHANNEL must be dev or stable." >&2; exit 2 ;;
esac
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Invalid marketing version." >&2; exit 2; }
[[ "${BUILD_NUMBER:-}" =~ ^[1-9][0-9]{0,8}$ ]] || { echo "BUILD_NUMBER must be a positive integer." >&2; exit 2; }
[[ "${SOURCE_COMMIT:-}" =~ ^[0-9a-f]{40}$ ]] || { echo "SOURCE_COMMIT must be a full commit hash." >&2; exit 2; }
[[ "${TEAM_ID:-}" =~ ^[A-Z0-9]{10}$ ]] || { echo "TEAM_ID is missing or invalid." >&2; exit 2; }
[[ "${ASC_KEY_ID:-}" =~ ^[A-Z0-9]{8,16}$ ]] || { echo "ASC_KEY_ID is missing or invalid." >&2; exit 2; }
[[ "${ASC_ISSUER_ID:-}" =~ ^[0-9a-f-]{36}$ ]] || { echo "ASC_ISSUER_ID is missing or invalid." >&2; exit 2; }
[ -n "${ASC_KEY:-}" ] || { echo "ASC_KEY is missing." >&2; exit 2; }

rm -rf "$work"
mkdir -p "$work"
chmod 700 "$work"
key="$work/AuthKey_$ASC_KEY_ID.p8"
(umask 077; printf '%s\n' "$ASC_KEY" > "$key")
trap 'rm -f "$key"' EXIT
auth=(-allowProvisioningUpdates -authenticationKeyPath "$key" -authenticationKeyID "$ASC_KEY_ID"
      -authenticationKeyIssuerID "$ASC_ISSUER_ID")

cat > "$work/ExportOptions.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>method</key><string>app-store-connect</string>
	<key>destination</key><string>upload</string>
	<key>teamID</key><string>$TEAM_ID</string>
	<key>signingStyle</key><string>automatic</string>
	<key>uploadSymbols</key><true/>
	<key>manageAppVersionAndBuildNumber</key><false/>
	<key>testFlightInternalTestingOnly</key><$internal/>
</dict>
</plist>
PLIST

echo "Archiving Zerus $version ($CHANNEL, build $BUILD_NUMBER, ${SOURCE_COMMIT:0:12})…"
xcodebuild -project "$root/Zerus.xcodeproj" -scheme Zerus -configuration Release -destination generic/platform=iOS \
    -archivePath "$work/Zerus.xcarchive" -derivedDataPath "$work/DerivedData" "${auth[@]}" \
    ZERUS_DEVELOPMENT_TEAM="$TEAM_ID" MARKETING_VERSION="$version" CURRENT_PROJECT_VERSION="$BUILD_NUMBER" \
    ZERUS_BUILD_CHANNEL="$CHANNEL" ZERUS_SOURCE_REVISION="${SOURCE_COMMIT:0:12}" archive

plist="$work/Zerus.xcarchive/Products/Applications/Zerus.app/Info.plist"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$plist")" = app.zerus.mobile
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist")" = "$version"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$plist")" = "$BUILD_NUMBER"
test "$(/usr/libexec/PlistBuddy -c 'Print :ZerusBuildChannel' "$plist")" = "$CHANNEL"

echo "Uploading to App Store Connect…"
xcodebuild -exportArchive -archivePath "$work/Zerus.xcarchive" -exportOptionsPlist "$work/ExportOptions.plist" \
    -exportPath "$work/export" "${auth[@]}"
echo "Uploaded Zerus $version ($BUILD_NUMBER). TestFlight processes it before testers receive it."
