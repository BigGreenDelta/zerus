# iOS distribution

The iOS app (`mobile/ios`, bundle ID `app.zerus.mobile`) has three ways to reach
a phone. All three install the same app ID, so a phone keeps one Zerus and its
Keychain pairing across them.

| Channel | Source | Version shown in About |
| --- | --- | --- |
| Local Dev | `mobile/ios/scripts/dev-install.sh` on the Mac the phone is attached to | `X.Y.Z-local` and the Git revision |
| Dev | Successful full Nightly verification, uploaded to TestFlight for internal testers; chosen builds also reach the public beta | `X.Y.Z-dev.N`, N = the CI run number |
| Stable | A maintainer-dispatched upload, submitted for App Store review | `X.Y.Z` |

`mobile/ios/DEV_VERSION` names the next planned release. `mobile/ios/VERSION`
appears with the first Stable release and must then stay below `DEV_VERSION`.
App Store versions are three integers; the Dev sequence travels as the build
number (`CFBundleVersion`), never in the marketing version.

## How uploads work

`.github/workflows/ios-testflight.yml` archives the exact main revision with the
newest Xcode 26 or later and uploads it with `xcodebuild -exportArchive`
(`method app-store-connect`, `destination upload`). Signing is automatic: the
archive is signed with an Apple Development identity, and the upload re-signs it
with a cloud-managed distribution certificate through an App Store Connect API
key. `mobile/ios/scripts/testflight-upload.sh` does the work and checks the
archived bundle ID, versions and channel before uploading.

A runner starts without a development identity, and automatic signing would then
create a new development certificate on every run until the team reaches Apple's
certificate limit. CI therefore imports one dedicated development identity into a
temporary keychain. The script refuses to archive without it, requires exactly one
valid Apple Development identity of the team in that keychain, and stops before
uploading when the archive is signed by any other certificate. It removes the
keychain, the identity file and the API key and restores the keychain search list
on every exit. No certificate or profile is stored in the repository. On a
maintainer's Mac the script uses that Mac's own development identity and refuses
to import one.

If an archive reports that the account has reached the maximum number of
certificates, some run archived without the dedicated identity. Revoke only an
`Apple Development: Created via API` certificate after proving that it belongs
to an ephemeral CI archive, its private key was not retained or reused, and no
retained signing identity or profile depends on it. Matching its fingerprint to
the `codesign --sign` leaf in an archive log establishes attribution, not safe
revocation by itself. Never revoke the dedicated CI identity or a personal,
distribution or Developer ID certificate.

- **Dev:** `nightly.yml` calls the upload workflow after full Windows/iOS and
  shared checks succeed, when `IOS_TESTFLIGHT_ENABLED=true`. Build numbers come
  from successful upstream main CI for the exact source SHA; Nightly's own
  sequence never resets them. A read-only GET-only App Store Connect preflight
  checks the app, marketing version and build number before any archive or
  signing identity import. Known valid/processing builds are kept without
  uploading again; failed/expired/ambiguous records or API errors refuse.
  Internal testers with automatic distribution receive processed new builds.
- **Public beta:** until the App Store release, anyone can join through the
  public TestFlight link of the external group **Public beta**. A maintainer
  promotes a chosen processed Dev build by adding it to that group (App Store
  Connect → TestFlight); Apple's Beta App Review approves it before external
  testers receive it. Dev uploads are therefore no longer marked
  internal-testing-only; only promoted builds leave the internal group.
- **Stable:** dispatch **iOS TestFlight** from `main` with channel `stable`
  after adding `mobile/ios/VERSION` and once that commit's main CI has succeeded.
  The manual workflow additionally requires full exact-source platform verification:
  the candidate's build number is that CI run number, so it is higher than every
  Dev build uploaded with the same version before the Dev base advanced. The uploaded build is then submitted for
  review in App Store Connect, together with the store listing.

The upload job runs in the `ios-testflight` environment and verifies that it runs
for `refs/heads/main` of `ufna/zerus`. Nightly forwards the five secret names;
manual Stable uses the same protected environment. PR code and other workflows
never receive them. The workflow has no cache access.

## One-time setup (maintainer)

1. **App Store Connect app record.** In App Store Connect → Apps → New App:
   platform iOS, bundle ID `app.zerus.mobile` (register it under Certificates,
   Identifiers & Profiles first if it is not offered), a unique app name, primary
   language English, and a SKU such as `zerus-ios`. The record cannot be created
   through the API.
2. **App Store Connect API key.** Users and Access → Integrations → App Store
   Connect API → Team Keys: create a key with the **Admin** role for this
   runbook's cloud-managed distribution signing. The key needs
   [access to cloud-managed distribution certificates](https://developer.apple.com/help/account/certificates/cloud-managed-certificates).
   A Developer-role notarization key without that access may work for local
   development signing yet fail this export. Download the `.p8` once and note
   its key ID and the issuer ID.
3. **GitHub environment.** Create the `ios-testflight` environment with a
   deployment branch policy that admits only `main`. Add the secrets
   `APP_STORE_CONNECT_API_KEY_P8` (the `.p8` file contents),
   `APP_STORE_CONNECT_API_KEY_ID` and `APP_STORE_CONNECT_API_ISSUER_ID`, and the
   environment or repository variable `IOS_TEAM_ID` (the Apple Developer team ID).
   Create one Apple Development certificate for CI alone: generate its key and
   certificate signing request outside any login keychain, create the
   certificate from the request, and pack the key and certificate into a PKCS#12
   file with a random password and the legacy encryption that macOS
   `security import` accepts. Store the base64 file as `IOS_DEVELOPMENT_P12` and
   its password as `IOS_DEVELOPMENT_P12_PASSWORD`, note the certificate ID and
   fingerprint privately, and delete the local files. A personal identity must not
   be used.
4. **TestFlight.** Add yourself to an internal testing group with automatic
   distribution, and install TestFlight on the phone. For the public beta, create
   an external group with a public link and fill in Test Information (beta
   description, feedback email, the privacy policy URL) and the Beta App Review
   contact; the review notes point to **Try demo** as for App Review.
5. **Enable Dev uploads.** Set the repository variable `IOS_TESTFLIGHT_ENABLED`
   to `true`. The next successful full Nightly uploads a new Dev build.
6. **Push notifications.** In Certificates, Identifiers & Profiles enable Push
   Notifications for `app.zerus.mobile`; automatic signing then includes the
   app's `aps-environment` entitlement (development for Local Dev builds,
   production for TestFlight and App Store builds). Under Keys create an Apple
   Push Notifications service key with the environment **Sandbox & Production**
   and the team-scoped restriction: the relay signs both `apns` and
   `apns_sandbox` wakes with one key. Download its `.p8` once, and give it to the
   relay privately as `apns_key`, `apns_key_id` and `apns_team_id` (see
   [mobile services](../services/mobile/README.md#push-configuration)). Without
   it the app still works; alerts then arrive only while Zerus is open or when
   iOS runs its background refresh.

For Local Dev on a Mac, either sign in to Xcode with an Apple ID of the team
(Xcode → Settings → Accounts) or pass the same API key to `dev-install.sh`
through `ZERUS_IOS_ASC_KEY_PATH`, `ZERUS_IOS_ASC_KEY_ID` and
`ZERUS_IOS_ASC_ISSUER_ID`; see [the iOS README](../mobile/ios/README.md).

## App Store submission checklist

- **Privacy:** the app has no tracking, ads or analytics. Messages, voice
  recordings and photos or videos travel through the relay, which keeps them
  until delivery and in its seven-day backups, so the published App Privacy
  label is **User Content** (Other User Content, Audio Data, Photos or Videos):
  App Functionality only, linked to the paired device, not used for tracking.
  `PrivacyInfo.xcprivacy` declares the same types, and `scripts/ci/check-source.py`
  keeps them in step. The privacy policy at `https://zerus.dev/privacy` describes
  the relay trust model in [the mobile architecture](mobile-architecture.md#trust-and-authorization).
- **Export compliance:** `ITSAppUsesNonExemptEncryption` is `false`; the app uses
  only standard HTTPS and the operating system's encryption.
- **App Review access:** reviewers cannot pair a computer. The review notes point
  to **Try demo** in Machines, which opens the labelled read-only preview, and
  explain that real use needs the user's own Zerus relay.
- **Store listing:** screenshots for the required iPhone sizes, description,
  keywords, support URL (`https://zerus.dev`) and age rating.
- **Creative assets:** the optional product page header (21:9, 3840 × 1646 PNG)
  and search results image (3:2) shown on iOS 27 and later. Keep the logo, text
  and screens inside the safe area of Apple's templates. Place them before
  submitting: App Store Connect refuses new placements while the version waits
  for review, so adding one later means removing the version from review.
