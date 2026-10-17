# iOS distribution

The iOS app (`mobile/ios`, bundle ID `app.zerus.mobile`) has three ways to reach
a phone. All three install the same app ID, so a phone keeps one Zerus and its
Keychain pairing across them.

| Channel | Source | Version shown in About |
| --- | --- | --- |
| Local Dev | `mobile/ios/scripts/dev-install.sh` on the Mac the phone is attached to | `X.Y.Z-local` and the Git revision |
| Dev | Every successful main CI run, uploaded to TestFlight for internal testers | `X.Y.Z-dev.N`, N = the CI run number |
| Stable | A maintainer-dispatched upload, submitted for App Store review | `X.Y.Z` |

`mobile/ios/DEV_VERSION` names the next planned release. `mobile/ios/VERSION`
appears with the first Stable release and must then stay below `DEV_VERSION`.
App Store versions are three integers; the Dev sequence travels as the build
number (`CFBundleVersion`), never in the marketing version.

## How uploads work

`.github/workflows/ios-testflight.yml` archives the exact main revision with the
newest Xcode 26 or later and uploads it with `xcodebuild -exportArchive`
(`method app-store-connect`, `destination upload`). Signing is automatic and
cloud-managed through an App Store Connect API key: no certificate, profile or
keychain is stored in the repository or on the runner, and the key file is
removed after the job. `mobile/ios/scripts/testflight-upload.sh` does the work and
checks the archived bundle ID, versions and channel before uploading.

- **Dev:** `ci.yml` job `dev-ios` calls the workflow after the CI gate of a main
  push succeeds, when the repository variable `IOS_TESTFLIGHT_ENABLED` is `true`.
  Dev builds are marked for TestFlight internal testing only, so they never reach
  external testers or review. Internal testers with automatic distribution enabled
  receive each processed build.
- **Stable:** dispatch **iOS TestFlight** from `main` with channel `stable`
  after adding `mobile/ios/VERSION`. The uploaded build is then submitted for
  review in App Store Connect, together with the store listing.

The upload job runs in the `ios-testflight` environment and verifies that it runs
for `refs/heads/main` of `ufna/zerus`. Only `ci.yml` forwards the three secret
names; PR code and other workflows never receive them. The workflow has no cache
access.

## One-time setup (maintainer)

1. **App Store Connect app record.** In App Store Connect → Apps → New App:
   platform iOS, bundle ID `app.zerus.mobile` (register it under Certificates,
   Identifiers & Profiles first if it is not offered), a unique app name, primary
   language English, and a SKU such as `zerus-ios`. The record cannot be created
   through the API.
2. **App Store Connect API key.** Users and Access → Integrations → App Store
   Connect API → Team Keys: create a key with the **Admin** role. The export
   signs with cloud-managed distribution certificates; a Developer-role key (such
   as a notarization key) installs Local Dev builds but fails the export with a
   cloud signing permission error. Download the `.p8` once and note its key ID
   and the issuer ID.
3. **GitHub environment.** Create the `ios-testflight` environment with a
   deployment branch policy that admits only `main`. Add the secrets
   `APP_STORE_CONNECT_API_KEY_P8` (the `.p8` file contents),
   `APP_STORE_CONNECT_API_KEY_ID` and `APP_STORE_CONNECT_API_ISSUER_ID`, and the
   environment or repository variable `IOS_TEAM_ID` (the Apple Developer team ID).
4. **TestFlight.** Add yourself to an internal testing group with automatic
   distribution, and install TestFlight on the phone.
5. **Enable Dev uploads.** Set the repository variable `IOS_TESTFLIGHT_ENABLED`
   to `true`. The next main push uploads a Dev build.
6. **Push notifications.** In Certificates, Identifiers & Profiles enable Push
   Notifications for `app.zerus.mobile`; automatic signing then includes the
   app's `aps-environment` entitlement (development for Local Dev builds,
   production for TestFlight and App Store builds). Under Keys create an Apple
   Push Notifications service key, download its `.p8` once, and give it to the
   relay privately as `apns_key`, `apns_key_id` and `apns_team_id` (see
   [mobile services](../services/mobile/README.md#push-configuration)). Without
   it the app still works; alerts then arrive only while Zerus is open or when
   iOS runs its background refresh.

For Local Dev on a Mac, either sign in to Xcode with an Apple ID of the team
(Xcode → Settings → Accounts) or pass the same API key to `dev-install.sh`
through `ZERUS_IOS_ASC_KEY_PATH`, `ZERUS_IOS_ASC_KEY_ID` and
`ZERUS_IOS_ASC_ISSUER_ID`; see [the iOS README](../mobile/ios/README.md).

## App Store submission checklist

- **Privacy:** the app declares no tracking and collects no data for its own
  use (`PrivacyInfo.xcprivacy`); session content travels between the phone, the
  user's own relay and computers. The App Privacy answers in App Store Connect
  are "Data Not Collected". The privacy policy URL must describe the relay trust
  model in [the mobile architecture](mobile-architecture.md#trust-and-authorization).
- **Export compliance:** `ITSAppUsesNonExemptEncryption` is `false`; the app uses
  only standard HTTPS and the operating system's encryption.
- **App Review access:** reviewers cannot pair a computer. The review notes point
  to **Try demo** in Machines, which opens the labelled read-only preview, and
  explain that real use needs the user's own Zerus relay.
- **Store listing:** screenshots for the required iPhone sizes, description,
  keywords, support URL (`https://zerus.dev`) and age rating.
