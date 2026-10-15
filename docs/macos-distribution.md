# macOS distribution

[Install on macOS](installation.md#macos-homebrew) | [CI and Arch packages](ci-and-aur.md) | [Design](superpowers/specs/2026-10-10-homebrew-cask-design.md)

Apple Silicon Macs install Zerus from the tap
[ufna/homebrew-zerus](https://github.com/ufna/homebrew-zerus):

```sh
brew install --cask ufna/zerus/zerus@nightly   # daily verified builds from main
zerus-setup
```

The first public macOS build is
[nightly 0.42.2.r1291.g5d2e5d0.n17](https://github.com/ufna/zerus/releases/tag/nightly-38794972859).
The stable `zerus` cask will appear with the first owner-triggered signed stable
release; it is not yet available. After that, install it with
`brew install --cask ufna/zerus/zerus`.

| Cask | Built by | Version | Download |
| --- | --- | --- | --- |
| `zerus` | manual `release.yml` candidate, `publish.yml` | `VERSION` | `releases/download/vVERSION/` |
| `zerus@nightly` | daily `nightly.yml`, `publish-nightly.yml` | `VERSION.rCOUNT.gCOMMIT.nNUMBER` | `releases/download/nightly-RUN_ID/` |

Each cask ships with the AUR packages of the same candidate: `zerus` with the
stable packages, `zerus@nightly` with `zerus-ade-nightly-bin`. Both install the
same `Zerus.app` and declare `conflicts_with` each other. The Dev channel, built on
every main push, has no cask: signing and notarizing every push and committing it
to the tap would cost a macOS runner and a tap commit per push, and dev artifacts
live only on the website mirror. Intel Macs and a CLI-only formula are not offered.

Both casks are built only while the repository variable `MACOS_CASKS_ENABLED` is
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
| `Contents/Frameworks`, `Contents/PlugIns` | Qt frameworks and plugins deployed by `macdeployqt` |
| `Contents/Resources/bin/zerus-setup` | Per-user setup, linked by the cask |
| `Contents/Resources/setup` | `hgs_state.py`, the three LaunchAgent installers with their `launchd_agent.py` helper and the tray LaunchAgent template |
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
only logs, saved window state and the four Zerus LaunchAgents.

## Pinned Qt

`packaging/macos/qt.json` pins official Qt **6.11.2** (`qtbase`, `qtsvg`,
`qtmultimedia`) by archive size and SHA-256. Qt WebEngine and the modules only it
needs (`qtdeclarative`, `qtwebchannel`, `qtpositioning`) are not part of the
bundle: the embedded DeepSeek Native UI is an optional Linux plugin, so on macOS
Native UI opens in the browser. `check-macos-bundle.py` rejects any Qt WebEngine
file. `scripts/install-qt-macos.py` downloads the archives from
`download.qt.io` (mirrors may serve them), verifies every archive before any
extraction and extracts them with the system `bsdtar`, through
`scripts/qt_archives.py`, which the [Windows build](ci-and-aur.md#windows-client-desktop-and-portable-zip)
shares. Framework links must stay inside the prefix. The official Qt is
relocatable and needs no path patching.

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
`nightly` whenever the variable is `true`. Both callers explicitly forward only
the five Apple secret names declared by the reusable workflow. GitHub requires
this forwarding even for environment secrets; without it their values are empty.
The values remain in the main-only `macos-signing` environment and are resolved
only by the signing job. The callers never use `secrets: inherit`. See
[GitHub's reusable workflow secret rules](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows#using-inputs-and-secrets-in-a-reusable-workflow).
It runs three jobs after full CI:

1. **macOS bundle** (`macos-15`, no secrets) selects the release version (the
   product version, or the nightly version computed exactly like
   `prepare-nightly.py`), installs the pinned Qt and runs
   `scripts/ci/macos-bundle.sh`. It builds the CLI and GUI with a macOS 14.0 target,
   runs every Qt suite against the bundled Qt, assembles the production bundle,
   runs the bundle checks and self-tests and uploads `Zerus-unsigned.zip`.
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
to Zerus. Without Qt WebEngine there is no helper process that would need JIT.

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

Still requiring interactive or subsequent release qualification:

- Terminal automation in the signed application.
- Installation from the public tap on a clean account, and an upgrade from one
  cask version to the next with running GUI, tmux session and native agent.
- The channel switch between stable and nightly once both casks are published.
