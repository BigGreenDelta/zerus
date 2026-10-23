# macOS distribution

[Install on macOS](installation.md#macos-homebrew) | [CI and Arch packages](ci-and-aur.md) | [Design](superpowers/specs/2026-10-10-homebrew-cask-design.md)

Apple Silicon Macs install Zerus from the tap
[ufna/homebrew-zerus](https://github.com/ufna/homebrew-zerus):

```sh
brew install --cask ufna/zerus/zerus
zerus-setup
```

Signed stable builds begin with
[0.43.0](https://github.com/ufna/zerus/releases/tag/v0.43.0).
For daily verified builds, install `ufna/zerus/zerus@nightly`; for Dev builds
from successful main CI, install `ufna/zerus/zerus@dev`.

| Cask | Built by | Version | Download |
| --- | --- | --- | --- |
| `zerus` | manual `release.yml` candidate, `publish.yml` | `VERSION` | `releases/download/vVERSION/` |
| `zerus@nightly` | daily `nightly.yml`, `publish-nightly.yml` | `VERSION.rCOUNT.gCOMMIT.nNUMBER` | `releases/download/nightly-RUN_ID/` |
| `zerus@dev` | successful main `ci.yml`, `publish-dev.yml` | `DEV_VERSION-dev.NUMBER` | `zerus.dev/downloads/dev-desktop-RUN_ID/` |

Stable and nightly casks ship with the AUR packages of the same candidate:
`zerus` with stable packages, `zerus@nightly` with `zerus-ade-nightly-bin`.
Dev follows successful main CI and uses the existing bounded website mirror,
without creating a GitHub Release for each push. All casks install the same
`Zerus.app`; only one channel can be installed. Intel Macs and a CLI-only formula
are not offered.

Mac casks are built only while the repository variable `MACOS_CASKS_ENABLED` is
`true`; until the signing credentials exist, stable and nightly candidates stay
Arch-only and publish as before.

## Bundle

For direct installation, GitHub Releases also carries
`zerus-VERSION-macos-arm64.dmg`. Open the image and drag `Zerus.app` to its
`Applications` shortcut. The read-only image contains the same stapled app as
the Homebrew ZIP; the image itself is separately signed, notarized and stapled.
See [DMG installation](installation.md#macos-dmg) for CLI and local-session setup.

`zerus-VERSION-macos-arm64.zip` contains one signed, notarized and stapled
`Zerus.app`:

| Path | Content |
| --- | --- |
| `Contents/MacOS/hgs-tray` | GUI; the executable keeps its name |
| `Contents/MacOS/hgs` | CLI, linked by the cask into Homebrew's `bin` |
| `Contents/MacOS/zerus-mobile-connector` | Rust mobile connector, signed with the other code; its LaunchAgent is opt-in through `zerus-setup --mobile-connector install` |
| `Contents/Frameworks`, `Contents/PlugIns` | Qt frameworks and plugins deployed by `macdeployqt` |
| `Contents/Resources/bin/zerus-setup` | Per-user setup, linked by the cask |
| `Contents/Resources/setup` | `hgs_state.py`, the three LaunchAgent installers with their `launchd_agent.py` helper, the mobile connector's service installer and the tray LaunchAgent template |
| `Contents/Resources/licenses` | Zerus, libvterm, md4c, Codex and Cargo notices; `qt/` holds LGPL-3.0, GPL-3.0, Qt's SPDX documents including Chromium components, and the Qt source location |

The bundle ID stays `com.hgdev.hgs-tray`, so settings, drafts and LaunchAgent
labels are shared with source installations. `CFBundleShortVersionString` is the
product version; `ZerusReleaseVersion` holds the stable or nightly release version,
which the GUI compares to offer **Restart to finish update** after an upgrade, even
between nightlies of one product version. `CFBundleName` is `Zerus` and
`LSMinimumSystemVersion` is 14.0, the desktop deployment floor in
`tray/CMakeLists.txt`. Every Mach-O file is arm64 only.
`scripts/check-macos-bundle.py` enforces the layout, notices, `Info.plist`
identity, minimum macOS version and that every library reference resolves inside
the bundle or the system.

The cask has no `postflight`, `uninstall quit:`, `uninstall launchctl:` or
`auto_updates`: an upgrade must not quit the GUI or unload agents. `zap` removes
only logs, saved window state and the five Zerus LaunchAgents, the mobile
connector's included; never `connector.json` or the connector's journal in
`~/.local/state/hgs/mobile-connector`, whose tombstones keep phone requests from
running twice.

`zerus-setup --mobile-connector install` writes the
`com.hgdev.zerus-mobile-connector` LaunchAgent for the bundled connector
(`AbandonProcessGroup`, `KeepAlive` on failure only, logs in `~/Library/Logs/hgs`),
only when none exists or it is its own, and prints the `launchctl bootstrap`
command instead of loading it. `zerus-setup --remove` unloads and removes it with
the other agents of the bundle; the agents and tmux servers it launched keep
running.

Under this agent, macOS attributes the connector's file access, and that of its
`hgs` and `git` children, to Zerus (`com.hgdev.hgs-tray`) rather than to
`zerus-mobile-connector`. Folder browsing and worktree catalogs in Documents,
Desktop and Downloads therefore use Zerus's Files and Folders consent; the
connector needs no consent of its own and no Full Disk Access. The stored consent
carries Zerus's Developer ID requirement, which later builds signed with the same
identifier and team satisfy. This was rehearsed on 2026-10-17 with a signed main
build under a fixture label, on a Mac where Zerus already had that consent, and
no prompt appeared (`zerus-nbzl.23`).

## Pinned Qt

`packaging/macos/qt.json` pins official Qt **6.11.2** (`qtbase`, `qtsvg`,
`qtmultimedia`, `qttools`, `qtdeclarative`) by archive size and SHA-256. `qttools`
supplies the build-time `lrelease` compiler for the optional interface catalog;
it does not change the English default. `lrelease` loads QtQml, so the build
prefix also needs `qtdeclarative`; the GUI links neither module. Qt WebEngine and
the other modules only it needs (`qtwebchannel`, `qtpositioning`) are not pinned:
the embedded DeepSeek Native UI is an optional Linux plugin, so on macOS
Native UI opens in the browser. `check-macos-bundle.py` rejects any Qt WebEngine
file. `scripts/install-qt-macos.py` downloads the archives from
`download.qt.io` (mirrors may serve them), verifies every archive before any
extraction and extracts them with the system `bsdtar`, through
`scripts/qt_archives.py`, which the [Windows build](ci-and-aur.md#windows-client-desktop-and-portable-zip)
shares. Framework links must stay inside the prefix. Before any Qt tool runs,
the installer reads the Mach-O load commands of `qmake`, `lrelease`,
`macdeployqt`, `moc` and `rcc` and stops when one of them, or a framework it
loads, needs a Qt framework the prefix lacks. The official Qt is relocatable
and needs no path patching.

Qt 6.11.2 is the version macOS CI tests through Homebrew, supports macOS 14 and
has public sources (`qt-everywhere-src-6.11.2`, `v6.11.2` tags) for the LGPL
source pointer. On 2026-10-10 the Qt 6.11.3 binaries were available without
published sources, so they cannot be shipped. Its relevant QtNetwork and
QXmlStreamReader security fixes are already in the v6.11.2 sources. Qt 6.12.0 also
passed all Qt suites but requires macOS 14.4.

To move to another Qt version, change the version and package names in
`qt.json`, confirm that its exact sources are published, then refresh the pins:

```sh
python3 scripts/install-qt-macos.py --lock
python3 scripts/install-qt-macos.py --output /tmp/zerus-qt
```

Build and test the tray against that prefix (`ZERUS_QT_PREFIX=/tmp/zerus-qt bash
scripts/ci/macos-bundle.sh`) and review the module SBOMs before committing.

## Candidates

`macos-cask.yml` is a reusable workflow with one input, `channel`. `release.yml`
calls it with `stable` when `macos_cask=true` (the default; it then requires
`run_macos=true`) and `MACOS_CASKS_ENABLED` is `true`; `nightly.yml` calls it with
`nightly` whenever the variable is `true`. Trusted main CI calls it with `dev`
after the full gate succeeds, when both `DEV_UPDATES_ENABLED` and
`MACOS_CASKS_ENABLED` are enabled. All callers explicitly forward only
the five Apple secret names declared by the reusable workflow. GitHub requires
this forwarding even for environment secrets; without it their values are empty.
The values remain in the main-only `macos-signing` environment and are resolved
only by the signing job. The callers never use `secrets: inherit`. See
[GitHub's reusable workflow secret rules](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows#using-inputs-and-secrets-in-a-reusable-workflow).
The same three stages remain required. Stable/Nightly start them after full
verification. Dev overlaps only the credential-free bundle with its core gate;
`macos-cask.yml` reuses that exact run's checked artifact and waits for both before
signing. An unsigned version from another Dev run is refused before key import.
The bundle packages the Release app already exercised by its pinned-Qt test build,
so it no longer compiles the same GUI again with `BUILD_TESTING=OFF`:

1. **macOS bundle** (`macos-15`, no secrets) selects the release version (the
   product version, the nightly version computed exactly like
   `prepare-nightly.py`, or `DEV_VERSION-dev.<CI run_number>`), installs the pinned Qt and runs
   `scripts/ci/macos-bundle.sh`. It builds the CLI and GUI with a macOS 14.0 target,
   runs every Qt suite against the bundled Qt, assembles the production bundle,
   runs the bundle checks and self-tests and uploads `Zerus-unsigned.zip`.
   Dev passes the same CI sequence to CMake. The unsigned, mounted and installed
   apps report their embedded build identity through `--build-info`; validation
   requires the expected version, Dev sequence and clean source commit.
   Bundle and installation runners install `tmux` explicitly. Self-tests use a
   private home and existing private socket directory, with only tmux exposed
   alongside system tools; they cannot inspect a developer's running sessions.
2. **macOS signing and notarization** runs in the `macos-signing` environment.
   It compiles nothing and executes only scripts from the same main commit.
   `scripts/ci/macos-keychain.sh` creates a temporary keychain;
   `scripts/sign-macos.py` signs inside-out with the hardened runtime and a secure
   timestamp, submits the app to the notary service and staples its ticket.
   It then creates the DMG, signs and notarizes it separately, staples its ticket
   and writes both release assets with `macos-info.json` only after both succeed.
   The job verifies the Team ID, Gatekeeper and the stapled ticket, then deletes the
   keychain and key files even on failure. A team API key passes its issuer ID; an
   individual API key leaves `APPLE_NOTARY_API_ISSUER_ID` empty.
   Stapling and ticket validation retry a lost CloudKit connection
   (`NSURLErrorDomain -1005`, exit 68) at most four times, after 5, 15, 30 and
   60 seconds, on the same signed file. The bundle checker and cask image check
   use the same validation policy. Other failures stop immediately; an exhausted
   retry or invalid ticket prevents publication. Signing and notary submissions
   are not retried by this policy.
3. **macOS cask installation** (no secrets) checks the DMG signature and ticket,
   mounts it read-only and verifies the contained app, self-tests and Applications
   shortcut. It then renders the channel's cask, runs `brew style`,
   installs it from a local tap pointing at the candidate bytes, repeats the bundle
   checks on `/Applications/Zerus.app`, runs `zerus-setup` and `--remove` in a
   scratch home and uninstalls the cask.
   After validating the image and mounted app with Gatekeeper and their stapled
   tickets, CI installs the cask with Homebrew's default quarantine and assesses
   the installed copy again before execution. Only after it passes does CI clear
   that disposable copy's download attribute, so the normal first-open
   confirmation cannot block unattended self-tests. Public install commands,
   casks and release archives retain their normal quarantine behavior.

Only entitlements with a concrete need are granted, and only to the application:
`com.apple.security.automation.apple-events` with `NSAppleEventsUsageDescription`
because Terminal integration runs `osascript`, whose Apple Events TCC attributes
to Zerus, and `com.apple.security.device.audio-input` and
`com.apple.security.device.camera` with `NSMicrophoneUsageDescription` and
`NSCameraUsageDescription` because the hardened runtime blocks voice and video
message recording without them. Without Qt WebEngine there is no helper process
that would need JIT.

The Arch assembly job then adds the signed ZIP, DMG and rendered cask through
`prepare-release.py --macos` (`homebrew/zerus.rb`) or `prepare-nightly.py --macos`
(`homebrew/zerus@nightly.rb`). The candidate records release-info schema 2: every
schema 1 field is unchanged, plus `macos` (asset, Qt version, Team ID, notarization
ID, minimum macOS, and `dmg: {asset, notarization_id}`) and `homebrew_casks`
(`["zerus"]` or `["zerus@nightly"]`). Older schema-2 releases without `dmg`
remain valid; all new signed builds require both formats.
`SHA256SUMS` covers both assets, and validation requires the cask to equal its template
in `packaging/homebrew/` with only the version, digest and, for a nightly, the
immutable `nightly-RUN_ID` tag substituted.
The cask continues to download the ZIP. Deploy DMG-aware `update_feed.py` on
the website mirror before publishing the first DMG candidate: both macOS files
remain on GitHub and are excluded from the existing desktop catalog artifact kinds.

When the variable is `true`, a skipped or failed cask fails the candidate: a stable
release needs `-f macos_cask=false` to stay Arch-only, and a nightly that cannot
build its bundle publishes neither AUR nor Homebrew.

## Publication

`publish.yml` and `publish-nightly.yml` check the candidate as before. For a schema 2
candidate they also require the tap deploy key. Before any GitHub write,
`publish-release.py` prepares the AUR changes and the tap commit, each with a
dry-run push. It then publishes the GitHub release or prerelease, verifies every
asset anonymously, pushes AUR and finally pushes the channel's cask file,
`Casks/zerus.rb` or `Casks/zerus@nightly.rb`; the other cask stays untouched. The
tap step refuses a lower version (stable versions compare numerically, nightlies by
Nightly run number) and a changed cask for an already published version, treats an
identical cask as done and never force-pushes; the tap's ruleset also blocks force
pushes and branch deletion. If any push fails, rerun the same publisher with the
same candidate run. Never rebuild or replace published assets.

The website mirror must run the current `scripts/update_feed.py` before
`MACOS_CASKS_ENABLED` is set: older versions reject a stable or nightly release
with a macOS asset. Like the Windows zip, the macOS zip stays on GitHub:
the mirror checks its sealed name and GitHub digest but neither stores it nor lists
it in catalog artifacts; see [public distribution](public-distribution.md).

Dev assembly adds the signed ZIP and DMG through `prepare-dev-update.py --macos`
to the existing desktop candidate. Its optional top-level `macos` object contains
the signing provenance plus exact `downloads` entries (`name`, `url`, `size`,
`sha256`); the source/Arch `artifacts` array stays compatible with older clients.
The mirror retains these bytes under the same three-build/72-hour grace/20 GiB
policy as other Dev files. Deploy `macos_release.py`, `dev_updates.py` and
`update_feed.py` together before the first Mac Dev candidate arrives.

After the mirror job succeeds, `publish-dev.yml` runs `publish-dev-cask.py` in
the protected release environment. It independently verifies the successful main
CI run, sealed candidate, exact public catalog and both public Mac file hashes,
then renders and pushes only `Casks/zerus@dev.rb`. Its `release-publication`
concurrency group serializes tap writes with stable/nightly publishers. An older
superseded candidate leaves the tap unchanged; changed bytes under an existing
version fail. Retry the publisher with the same CI run, without rebuilding.

Stable/nightly v1 catalogs expose the same optional `macos` download projection
with GitHub URLs. Existing catalogs may gain only this projection from their
already sealed release; all prior fields and release bytes stay immutable.
Settings > Updates recognizes actual Homebrew ownership and direct DMG installs,
defaults to the installed channel unless the user already chose another, compares
the full installed release identity and offers a copyable Homebrew command or
DMG download. A channel without a Mac build offers neither an upgrade command nor
a download. Installation and GUI restart remain explicit.

## Configuration and credentials

Configured on 2026-10-10:

- Public tap `ufna/homebrew-zerus` with README, MIT license and `Casks/`; issues,
  wiki and projects disabled; ruleset "Protect main" blocks deletion and
  non-fast-forward pushes.
- Write deploy key "Zerus publish.yml (release environment)"; its private key
  exists only as the `release` environment secret `HOMEBREW_TAP_DEPLOY_KEY`.
- Environment `macos-signing`, restricted to the `main` branch like `release`.
- `packaging/homebrew/known_hosts` pins GitHub's Ed25519 host key
  (`SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU`, from
  `api.github.com/meta` and GitHub's published fingerprints).

Provisioned and tested on 2026-10-15 (`zerus-6a2u.2`): a **Developer ID
Application** certificate issued by the G2 intermediate and a Developer-role
App Store Connect team API key for notarization. All five secrets below and
`MACOS_TEAM_ID` are configured in the main-only `macos-signing` environment.
Private encrypted exports and API key backups remain outside the repository.

For rotation, store values from private files or prompts, never through chat or
literal command arguments. Commands without `--body` or stdin prompt for the value:

```sh
base64 -i DeveloperIDApplication.p12 | gh secret set MACOS_DEVELOPER_ID_P12 --env macos-signing --repo ufna/zerus
gh secret set MACOS_DEVELOPER_ID_P12_PASSWORD --env macos-signing --repo ufna/zerus
gh secret set APPLE_NOTARY_API_KEY_P8 --env macos-signing --repo ufna/zerus < AuthKey_KEYID.p8
gh secret set APPLE_NOTARY_API_KEY_ID --env macos-signing --repo ufna/zerus
gh secret set APPLE_NOTARY_API_ISSUER_ID --env macos-signing --repo ufna/zerus   # team keys only
gh variable set MACOS_TEAM_ID --env macos-signing --repo ufna/zerus --body TEAMID
```

For initial activation, verify that the deployed mirror's `update_feed.py`
supports schema 2 and the macOS asset; redeploy it if needed. Then enable both
casks and start a manual nightly as the first signed build:

```sh
gh variable set MACOS_CASKS_ENABLED --repo ufna/zerus --body true
gh workflow run nightly.yml --ref main
```

If signing or notarization fails, that nightly publishes nothing; fix the
credentials and rerun, or unset the variable to return to Arch-only candidates. Rotate the certificate or
API key by testing its replacement before changing the secrets or revoking the
old credential. Never revoke an unrelated team certificate.

The original Developer ID intermediate expires on 2027-02-01. Xcode issued a
certificate from that old chain during setup, so CI was switched to a
portal-issued G2 replacement before any release. Its expiration is 2027-11-14.
Check the leaf certificate's issuer and expiration during each rotation; select
**G2 Sub-CA** when the portal offers a choice. See
[Apple's replacement procedure](https://developer.apple.com/help/account/certificates/replace-developer-id-certificates/).

### Local source builds

Raw CMake build bundles use `com.hgdev.hgs-tray.local`, so an uninstalled ad hoc
copy cannot replace the installed application's privacy identity. Do not grant
that copy permissions as a substitute for the installed app. Settings, native
profiles and session identities are unchanged.

`tray/install.sh` copies the raw bundle to a disposable staging directory and
signs that copy through `tray/sign-local-macos.py` before replacing or restarting
the installed application. Only after selecting a compatible certificate does the helper promote
the staged copy to the installed `com.hgdev.hgs-tray` identifier; failed preparation
restores the local identifier and the installer removes its temporary copy.
The build-tree bundle stays local. The helper uses a valid
application-signing identity already in the local keychain; it never imports or
unlocks credentials and never falls back to ad hoc signing. `mac-install.sh`
uses the destination Mac's keychain and forwards the optional
`HGS_MACOS_SIGN_IDENTITY` exact certificate name or SHA-1 selector.

When a certificate-signed app is already installed, selection follows its team
and signing class. A local rebuild of the published app therefore needs a
compatible Developer ID Application identity. Apple Development can retain its
own local identity, but has a different default designated requirement from
Developer ID. Without an installed certificate identity, selection prefers one
unambiguous Developer ID Application certificate, then Apple Development.
Missing or ambiguous identities stop installation with setup instructions.

The keychain must be unlocked in the signing context and allow `codesign` to use
the private key. An SSH login does not unlock it. For `errSecInternalComponent`
or a key-access error, test from Terminal on the Mac and respond to the system's
unlock or key-access dialog. Follow [Apple's signing troubleshooting](https://developer.apple.com/forums/thread/712005)
before testing SSH again. Do not put keychain passwords in environment variables,
repository files or command arguments; the installer never edits key ACLs.

The helper verifies the candidate signature and checks that each app satisfies
the other's designated requirement, as described in
[Apple TN3127](https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements).
A shared bundle identifier or team alone is insufficient. An incompatible
signature stops before the maintained GUI updater can replace the installed app.
An old ad hoc installation can move to certificate signing with fresh consent
once; the installer does not reset TCC or grant permissions.

The source bundle contains one executable and links external Homebrew Qt.
It is signed without the hardened runtime or a network timestamp, for local
development. Unexpected nested code or symbolic links are refused. Official
packaging also promotes only its private staged copy to the unchanged public
identifier. Public distribution continues to use the separate inside-out hardened-runtime signer,
entitlements, timestamp and notarization checks above.

An opt-in native regression uses two different synthetic executables to verify
mutual designated-requirement compatibility, compares them to the installed app
read-only, and proves that an ad hoc replacement is rejected:

```sh
ZERUS_TEST_LOCAL_MACOS_SIGNING=1 python3 -m unittest discover -s tests -p test_macos_local_signing.py -v
```

### macOS privacy permissions after a signature change

macOS attributes agent-launched Apple Events, Accessibility and screen capture
to the containing `com.hgdev.hgs-tray` application, displayed as **Zerus**.
An enabled Settings switch is not proof that the current binary is trusted.
During qualification, the TCC log showed `SecStaticCodeCheckValidity -67050`:
the stored requirement still matched an older ad-hoc build's code hash. Toggling
the existing entry retained that stale requirement.

After verifying this exact mismatch, reset only the affected service for this
bundle, request access again through its public API, and let the user grant fresh
consent in System Settings:

```sh
tccutil reset Accessibility com.hgdev.hgs-tray
tccutil reset ScreenCapture com.hgdev.hgs-tray
```

These are separate services; run only the required reset. Confirm access with
`AXIsProcessTrusted()` and `CGPreflightScreenCaptureAccess()` respectively.
Retain a stable Developer ID designated requirement across updates. Do not reset
all privacy permissions, grant consent on the user's behalf or restart native
agents to repair GUI permissions.

## Mac App Store

Mac App Store publication is deferred by the owner as of 2026-10-15, tracked in
`zerus-8reb`. The active scope is Homebrew and a downloadable signed macOS DMG
in GitHub Releases. The Homebrew Developer ID signature and notarization do not qualify
an app for the store. Apple requires App Sandbox, inherited sandbox constraints
for embedded command-line helpers, and store-managed application updates. See
[App Review Guidelines 2.4.5](https://developer.apple.com/app-store/review/guidelines/#hardware-compatibility)
and [sandboxed command-line helpers](https://developer.apple.com/documentation/xcode/embedding-a-helper-tool-in-a-sandboxed-app).

The current entitlements do not enable App Sandbox. Local provider executables,
tmux, per-user services and shared state need an explicit sandbox-compatible
design and isolated qualification before a store build can be submitted.
Do not silently remove local functionality or change the Homebrew bundle to
obtain a store build.

Apple Distribution and Mac Installer Distribution certificates were issued on
2026-10-15. Encrypted PKCS#12 exports were tested in a disposable keychain using
a synthetic app and installer package; the code and package signatures verified.
This establishes credential readiness only. Store-specific provisioning,
product scope, metadata, an upload and App Review remain outstanding.

## Validation record

On 2026-10-10, local checks on an Apple Silicon Mac covered the setup command
(also under `/usr/bin/python3` 3.9), bundled `hgs` discovery, Homebrew ownership
and restart detection, candidate schema 2, the tap publication contracts and the
mirror. All 46 Qt suites passed against the pinned official Qt.
`scripts/ci/macos-bundle.sh` produced a 136 MB unsigned zip that passed the
bundle checks and self-tests. Ad-hoc signing with the hardened runtime verified
signing order, nested entitlements and `codesign --verify --deep --strict`.
Pinned actionlint 1.7.12 reported no findings.

On 2026-10-12, after rebasing onto main with the Dev channel and adding
`zerus@nightly`: all macOS, publication, mirror, CI-security and privacy Python
contracts passed; all 54 Qt suites passed against Homebrew Qt and, inside
`macos-bundle.sh`, against the pinned official Qt. A nightly bundle built with
`ZERUS_RELEASE_VERSION=0.42.0.r999.gabcdef0.n77` passed the bundle checks and
self-tests and carried that `ZerusReleaseVersion`, product version 0.42.0, macOS
14.0 and `launchd_agent.py`. Pinned actionlint 1.7.12 reported no findings.

On 2026-10-15, the G2 certificate and intermediate were imported from the encrypted
PKCS#12 into a disposable keychain. A native probe passed hardened-runtime signing
with Apple's timestamp, strict signature verification and execution. Apple's
notary service accepted that signed probe using the team API key. The temporary
keychain was removed and the original keychain search list retained.
The deployed mirror already supported macOS schema-2 releases. Casks were enabled
and [Nightly run 38767851278](https://github.com/ufna/zerus/actions/runs/38767851278)
was started for the first full bundle qualification.
That run passed full CI and built the bundle, but its final self-test could not
find tmux in the restricted PATH. The workflow now installs that dependency,
and the checker exposes it through a private bin directory with an isolated
tmux socket directory. Fixture and real installed GUI/CLI checks verify this
environment without modifying the installed application or live sessions.

DMG qualification on 2026-10-15 used a synthetic native application and the
production G2 credentials in a disposable keychain. Apple accepted separate app
and DMG submissions; both tickets were stapled and validated. Gatekeeper accepted
the signed image and the app mounted read-only inside it, and that app executed
successfully. The drag-to-Applications link and nested framework links survived
image creation. Publication contracts cover both channels, immutable DMG
checksums, old ZIP-only releases, and exclusion from website mirror storage.

Production qualification later on 2026-10-15 used the exact unsigned artifact
from [Nightly 38776672013](https://github.com/ufna/zerus/actions/runs/38776672013),
source `8f7f68f67c6eadc0eb9bf6c4946ffa0e614a6bff`. Full CI and bundle tests
passed, but that run stopped before signing because the reusable workflow's
callers did not forward the environment secret names. Both callers now forward
the explicit five-name allowlist; credential values remain in `macos-signing`.
The production signing script independently qualified the saved Qt bundle in a
temporary local keychain: Apple accepted the app and DMG, both tickets validated,
and Gatekeeper accepted both. The read-only mounted app passed the CLI and GUI
self-tests under library validation and reported the exact clean source commit.
This local qualification was not published. Both rendered cask templates passed
`brew style` in disposable local taps.

The first published DMG comes from
[Nightly 38794972859](https://github.com/ufna/zerus/actions/runs/38794972859),
version `0.42.2.r1291.g5d2e5d0.n17`, from clean source
`5d2e5d0955ab34f1e81ebd237a9b7eed2761eb7f`. Its complete CI matrix and all
67 production Qt test suites passed. Apple accepted separate app and DMG
submissions; the signing job stapled and verified both. An independent download
matched the Actions artifact digest, and its read-only mounted app passed
signature, ticket, Gatekeeper, CLI/GUI self-tests and exact source metadata checks.
A transient CloudKit connection loss during CI ticket validation was resolved by
retrying installation verification on the same signed artifact, without a rebuild.
The clean macOS runner passed Homebrew style, installation, installed bundle and
Gatekeeper checks, CLI/GUI self-tests, isolated setup/removal and uninstallation.
[Publisher 38802915468](https://github.com/ufna/zerus/actions/runs/38802915468)
published the immutable nightly, synchronized AUR and committed the nightly cask
to `ufna/homebrew-zerus`. Anonymous downloads of the public DMG and ZIP match
their Actions artifact bytes, `SHA256SUMS` and GitHub digests; the public cask
matches the reviewed template, version and ZIP digest. `brew fetch --cask
ufna/zerus/zerus@nightly` also downloaded and verified the public package without
installing over the developer's app. The desktop/nightly mirror synchronized
successfully and serves the same release and source commit; macOS downloads
remain on GitHub.

The first public macOS Dev build, `0.42.3-dev.693`, was qualified on 2026-10-16
from clean source `f16d73bcd0730157b43a3ca2b8476f0319bf6193`.
[CI 38813489733](https://github.com/ufna/zerus/actions/runs/38813489733)
passed the full platform matrix, all 68 production Qt suites, Apple signing and
notarization, and clean Homebrew installation and removal. Both the read-only
mounted DMG and the installed cask passed bundle identity and CLI/GUI self-tests.
[Publisher 38820507139](https://github.com/ufna/zerus/actions/runs/38820507139)
published the sealed candidate through the bounded Dev mirror and verified its
public ZIP/DMG bytes before adding `zerus@dev` to the tap. No GitHub Release was
created for this Dev build.

Independent downloads on macOS matched the published sizes and SHA-256 values;
the public cask matched the reviewed template and ZIP digest. Gatekeeper and
stapler accepted the downloaded DMG, and its read-only mounted application passed
the self-tests with the exact clean source and Dev identity. `brew fetch --cask
ufna/zerus/zerus@dev` verified the public package without installing it. The
compiled desktop update controller also parsed the live Dev catalog and exposed
its correct ZIP and DMG links. The shared developer GUI was left unchanged;
its coordinated installation remained a separate rollout.

The first signed macOS Stable release, [0.43.0](https://github.com/ufna/zerus/releases/tag/v0.43.0),
was published on 2026-10-16 with hgs 1.52.0 from clean source
`48dff262f46be71bbd9cd114577239ee85f55573`.
[Candidate 38908051629](https://github.com/ufna/zerus/actions/runs/38908051629)
passed the complete platform matrix, all 75 production Qt suites, the pinned
real-agent matrix without skips, both Arch package builds, Apple signing and
notarization, and clean Homebrew installation. Retries kept the same source;
the CLI lifecycle and transcript-polling test races remain tracked separately
in `zerus-d84a`. The final source archive matched `git archive` exactly. Gitleaks
8.30.1 passed the complete Git history, tracked working tree and candidate source.

[Publisher 38920602130](https://github.com/ufna/zerus/actions/runs/38920602130)
published the candidate unchanged as the latest Stable release and synchronized
the AUR packages and `zerus` cask. Independent anonymous downloads matched the
candidate, GitHub asset digests and checksums. The public DMG and its read-only
mounted application passed signature, ticket, Gatekeeper, exact source identity
and isolated CLI/GUI self-tests. `brew fetch --cask ufna/zerus/zerus` verified the
public cask without installing it. The Stable mirror synchronized successfully;
the compiled desktop update controller accepted its live catalog and exposed the
correct ZIP and DMG links. Stable, Nightly and Dev are now published Homebrew
channels. Shared developer GUI replacement remains a separate coordinated action.

Still requiring interactive or subsequent release qualification:

- Terminal automation in the signed application.
- Installation from the public tap on a clean account, and an upgrade from one
  cask version to the next with running GUI, tmux session and native agent.
- The channel switch between stable and nightly from the public tap.

### Public cask upgrade qualification

The manual **macOS public cask upgrade qualification** workflow tests already
published bytes on a disposable standard `macos-15` runner. It has no signing,
publication, deployment or cache credentials. Supply three full commits from
`ufna/homebrew-zerus`: an older Stable recipe, a newer Stable recipe and the
signed Nightly recipe. The defaults retain the public 0.43.0 and 0.44.0 Stable
recipes; after publishing 0.45.0, repeat with the exact 0.44.0 and 0.45.0 commits.
The Nightly commit must also be supplied explicitly.

The existing ephemeral `contents: read` job token is used only for exact official
GitHub REST tap-commit and release-tag metadata GETs, avoiding the shared anonymous
rate limit. The requester rejects redirects and every other host, route or method.
The script removes the token from its environment before starting any subprocess;
raw casks and release payloads remain anonymous, and evidence never includes it.

Before Homebrew evaluates a cask, the script checks its complete bytes against
the current or retained pre-connector template, public release metadata and
seals, bounded ZIP paths and the native Developer ID signature, Gatekeeper and
stapled ticket. It installs from the real public tap, performs `brew upgrade`
between the Stable versions and switches Stable to Nightly and back using
ordinary uninstall/install, without `--zap` or `zerus-setup --remove`.

An official SHA-512-pinned Claude executable runs against the existing
localhost-only keyless fixture in a private tmux server. Its exact process start,
pane, run and conversation must survive every package operation, together with
synthetic saved preferences and an immutable attachment draft. The old private
GUI must remain running during package operations; the fixture then explicitly
restarts only that GUI and checks retained state. The receipt distinguishes this
from interactive editor input. Native-format preferences belong to the disposable
runner account; setting `HOME` alone is not claimed to isolate CFPreferences.

This job must never run on an owner's account. It does not grant or reset TCC
permissions and does not qualify Terminal automation. That remaining case needs
an actual signed application's explicit **Open in Terminal** action against an
owned synthetic session in a user-authorized interactive test: accept any normal
Apple Events consent explicitly, verify the new owned Terminal window attaches
the exact private tmux session, then close only that window. An `osascript`
response over SSH or a fake Terminal backend is not that evidence.
