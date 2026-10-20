#!/usr/bin/env bash
# Archive the iOS app and upload it to App Store Connect. Used by the trusted main
# workflow ios-testflight.yml; it can also run on a maintainer's Mac.
#
# Environment:
#   CHANNEL        dev (TestFlight; internal testers, public beta once promoted) or stable (App Store candidate)
#   BUILD_NUMBER   unique increasing build number (the CI sequence for dev)
#   SOURCE_COMMIT  the exact source revision being archived
#   TEAM_ID        Apple Developer team ID
#   ASC_KEY        App Store Connect API private key (.p8 contents)
#   ASC_KEY_ID     its key ID
#   ASC_ISSUER_ID  its issuer ID
#   IOS_DEVELOPMENT_P12           CI only: base64 PKCS#12 of the dedicated Apple Development identity
#   IOS_DEVELOPMENT_P12_PASSWORD  CI only: its password
# The archive is signed automatically with an Apple Development identity and the
# upload re-signs it with a cloud-managed distribution certificate through the API
# key. A maintainer's Mac uses its own development identity. A CI runner imports
# the dedicated identity into a temporary keychain: without one, automatic signing
# would create a new development certificate on every ephemeral runner until the
# team reaches Apple's certificate limit.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=${RUNNER_TEMP:-$(mktemp -d)}/app-store-connect
case "${CHANNEL:-}" in
    dev) version=$(tr -d '[:space:]' < "$root/DEV_VERSION") ;;
    stable)
        [ -f "$root/VERSION" ] || { echo "Add mobile/ios/VERSION with the stable release version first." >&2; exit 2; }
        version=$(tr -d '[:space:]' < "$root/VERSION") ;;
    *) echo "CHANNEL must be dev or stable." >&2; exit 2 ;;
esac
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Invalid marketing version." >&2; exit 2; }
[[ "${BUILD_NUMBER:-}" =~ ^[1-9][0-9]{0,8}$ ]] || { echo "BUILD_NUMBER must be a positive integer." >&2; exit 2; }
[[ "${SOURCE_COMMIT:-}" =~ ^[0-9a-f]{40}$ ]] || { echo "SOURCE_COMMIT must be a full commit hash." >&2; exit 2; }
[[ "${TEAM_ID:-}" =~ ^[A-Z0-9]{10}$ ]] || { echo "TEAM_ID is missing or invalid." >&2; exit 2; }
[[ "${ASC_KEY_ID:-}" =~ ^[A-Z0-9]{8,16}$ ]] || { echo "ASC_KEY_ID is missing or invalid." >&2; exit 2; }
[[ "${ASC_ISSUER_ID:-}" =~ ^[0-9a-f-]{36}$ ]] || { echo "ASC_ISSUER_ID is missing or invalid." >&2; exit 2; }
[ -n "${ASC_KEY:-}" ] || { echo "ASC_KEY is missing." >&2; exit 2; }
ci=false
if [ "${GITHUB_ACTIONS:-}" = true ]; then
    ci=true
    if [ -z "${IOS_DEVELOPMENT_P12:-}" ] || [ -z "${IOS_DEVELOPMENT_P12_PASSWORD:-}" ]; then
        echo "IOS_DEVELOPMENT_P12 and IOS_DEVELOPMENT_P12_PASSWORD are missing; see docs/ios-distribution.md." >&2
        exit 2
    fi
elif [ -n "${IOS_DEVELOPMENT_P12:-}" ]; then
    # Importing changes the user's keychain search list, which a shared Mac must keep.
    echo "IOS_DEVELOPMENT_P12 is imported only on a CI runner; a Mac signs with its own identity." >&2
    exit 2
fi

rm -rf "$work"
mkdir -p "$work"
chmod 700 "$work"
key="$work/AuthKey_$ASC_KEY_ID.p8"
p12="$work/development.p12"
keychain="$work/signing.keychain-db"
keychain_created=false
search_list_changed=false
search_list=()

cleanup() {
    local status=$? failed=false
    if [ "$search_list_changed" = true ]; then
        security list-keychains -d user -s ${search_list[@]+"${search_list[@]}"} || failed=true
    fi
    if [ "$keychain_created" = true ]; then
        security delete-keychain "$keychain" || failed=true
    fi
    rm -f "$key" "$p12" || failed=true
    if [ "$failed" = true ]; then
        echo "Could not remove the temporary signing keychain or restore the keychain search list." >&2
        [ "$status" -ne 0 ] || status=1
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

(umask 077; printf '%s\n' "$ASC_KEY" > "$key")
auth=(-allowProvisioningUpdates -authenticationKeyPath "$key" -authenticationKeyID "$ASC_KEY_ID"
      -authenticationKeyIssuerID "$ASC_ISSUER_ID")

sha1() {
    openssl x509 -noout -fingerprint -sha1 "$@" | sed 's/.*=//' | tr -d ':' | tr '[:lower:]' '[:upper:]'
}

identity=
if [ "$ci" = true ]; then
    # Read the search list first: creating a keychain adds it to that list.
    listing=$(security list-keychains -d user)
    while IFS= read -r line; do
        line=$(printf '%s' "$line" | sed -e 's/^ *"//' -e 's/"$//')
        [ -z "$line" ] || search_list+=("$line")
    done <<< "$listing"
    password=$(openssl rand -hex 24)
    search_list_changed=true
    keychain_created=true
    security create-keychain -p "$password" "$keychain"
    security set-keychain-settings -lut 3600 "$keychain"
    security unlock-keychain -p "$password" "$keychain"
    (umask 077; printf '%s' "$IOS_DEVELOPMENT_P12" | base64 --decode > "$p12")
    security import "$p12" -k "$keychain" -f pkcs12 -P "$IOS_DEVELOPMENT_P12_PASSWORD" -T /usr/bin/codesign > /dev/null
    rm -f "$p12"
    security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$password" "$keychain" > /dev/null
    unset password
    security list-keychains -d user -s "$keychain" ${search_list[@]+"${search_list[@]}"}

    # Exactly one valid Apple Development identity of this team, still in date.
    valid=$(security find-identity -v -p codesigning "$keychain")
    identities=$(printf '%s\n' "$valid" | sed -n -E 's/^ *[0-9]+\) ([0-9A-F]{40}) "(.*)"$/\1 \2/p')
    if [ "$(printf '%s' "$identities" | grep -c .)" != 1 ] || [[ "${identities#* }" != "Apple Development: "* ]]; then
        echo "The keychain must hold exactly one valid Apple Development identity." >&2
        exit 1
    fi
    identity=${identities%% *}
    security find-certificate -a -c "Apple Development:" -p "$keychain" > "$work/development.pem"
    if [ "$(grep -c 'BEGIN CERTIFICATE' "$work/development.pem")" != 1 ] || [ "$(sha1 -in "$work/development.pem")" != "$identity" ]; then
        echo "The development certificate does not match its identity." >&2
        exit 1
    fi
    subject=$(openssl x509 -noout -subject -nameopt RFC2253 -in "$work/development.pem")
    if ! [[ "${subject#subject=}" =~ (^|,)\ *OU=$TEAM_ID(,|$) ]] ||
        ! openssl x509 -noout -checkend 0 -in "$work/development.pem" > /dev/null; then
        echo "The development certificate has expired or belongs to another team." >&2
        exit 1
    fi
fi

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
</dict>
</plist>
PLIST

# ZerusKit links the project task replica (mobile/ios/scripts/build-replica.sh).
# The workflow builds it in a step of its own, before any signing secret is in
# the environment, so no dependency's build script runs beside them.

echo "Archiving Zerus $version ($CHANNEL, build $BUILD_NUMBER, ${SOURCE_COMMIT:0:12})…"
xcodebuild -project "$root/Zerus.xcodeproj" -scheme Zerus -configuration Release -destination generic/platform=iOS \
    -archivePath "$work/Zerus.xcarchive" -derivedDataPath "$work/DerivedData" "${auth[@]}" \
    ZERUS_DEVELOPMENT_TEAM="$TEAM_ID" MARKETING_VERSION="$version" CURRENT_PROJECT_VERSION="$BUILD_NUMBER" \
    ZERUS_BUILD_CHANNEL="$CHANNEL" ZERUS_SOURCE_REVISION="${SOURCE_COMMIT:0:12}" archive

app="$work/Zerus.xcarchive/Products/Applications/Zerus.app"
if [ -n "$identity" ]; then
    codesign -d --extract-certificates="$work/archived-certificate" "$app" 2> /dev/null
    if [ "$(sha1 -inform DER -in "$work/archived-certificate0")" != "$identity" ]; then
        echo "The archive is signed by an unexpected identity; Xcode may have created a development certificate." >&2
        exit 1
    fi
fi
plist="$app/Info.plist"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$plist")" = app.zerus.mobile
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist")" = "$version"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$plist")" = "$BUILD_NUMBER"
test "$(/usr/libexec/PlistBuddy -c 'Print :ZerusBuildChannel' "$plist")" = "$CHANNEL"
test "$(/usr/libexec/PlistBuddy -c 'Print :ZerusApnsEnvironment' "$plist")" = production

echo "Uploading to App Store Connect…"
xcodebuild -exportArchive -archivePath "$work/Zerus.xcarchive" -exportOptionsPlist "$work/ExportOptions.plist" \
    -exportPath "$work/export" "${auth[@]}"
echo "Uploaded Zerus $version ($BUILD_NUMBER). TestFlight processes it before testers receive it."
