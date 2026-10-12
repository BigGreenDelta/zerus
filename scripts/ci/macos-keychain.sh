#!/usr/bin/env bash
# Create or delete the temporary Developer ID keychain of one macos-signing job.
# Runs only on ephemeral hosted runners; it changes the user's keychain search list.
set -euo pipefail
directory="${RUNNER_TEMP:?RUNNER_TEMP is required}/zerus-signing"
keychain="$directory/signing.keychain-db"
case "${1:-}" in
create)
    : "${P12:?}" "${P12_PASSWORD:?}" "${NOTARY_KEY:?}" "${TEAM_ID:?}"
    if ! [[ "$TEAM_ID" =~ ^[A-Z0-9]{10}$ ]]; then
        echo "Configure the macos-signing variable MACOS_TEAM_ID first." >&2
        exit 1
    fi
    install -d -m 700 "$directory"
    password="$(openssl rand -hex 24)"
    printf '%s' "$P12" | base64 --decode > "$directory/identity.p12"
    printf '%s\n' "$NOTARY_KEY" > "$directory/notary.p8"
    chmod 600 "$directory/identity.p12" "$directory/notary.p8"
    security create-keychain -p "$password" "$keychain"
    security set-keychain-settings -lut 21600 "$keychain"
    security unlock-keychain -p "$password" "$keychain"
    security import "$directory/identity.p12" -k "$keychain" -P "$P12_PASSWORD" -T /usr/bin/codesign
    security set-key-partition-list -S apple-tool:,apple: -s -k "$password" "$keychain" > /dev/null
    rm -f "$directory/identity.p12"
    existing=()
    while IFS= read -r line; do
        existing+=("$(printf '%s' "$line" | sed -e 's/^ *"//' -e 's/"$//')")
    done < <(security list-keychains -d user)
    security list-keychains -d user -s "$keychain" "${existing[@]}"
    identity="$(security find-identity -v -p codesigning "$keychain" \
        | sed -n "s/.*\"\(Developer ID Application: .* ($TEAM_ID)\)\"$/\1/p" | head -n 1)"
    if [ -z "$identity" ]; then
        echo "The certificate is not a Developer ID Application identity of team $TEAM_ID." >&2
        exit 1
    fi
    printf '%s' "$identity" > "$directory/identity"
    ;;
delete)
    security delete-keychain "$keychain" 2> /dev/null || true
    rm -rf "$directory"
    ;;
*)
    echo "usage: macos-keychain.sh create|delete" >&2
    exit 2
    ;;
esac
