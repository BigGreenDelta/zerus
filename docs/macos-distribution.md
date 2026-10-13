# macOS distribution

[Install on macOS](installation.md#macos-homebrew) | [CI and Arch packages](ci-and-aur.md) | [Design](superpowers/specs/2026-10-10-homebrew-cask-design.md)

Apple Silicon Macs install Zerus from the tap
[ufna/homebrew-zerus](https://github.com/ufna/homebrew-zerus):

```sh
brew install --cask ufna/zerus/zerus           # stable releases
brew install --cask ufna/zerus/zerus@nightly   # daily verified builds from main
zerus-setup
```

| Cask | Built by | Version | Download |
| --- | --- | --- | --- |
| `zerus` | manual `release.yml` candidate, `publish.yml` | `0.42.0` | `releases/download/v0.42.0/` |
| `zerus@nightly` | daily `nightly.yml`, `publish-nightly.yml` | `0.42.0.r1234.gabcdef0.n56` | `releases/download/nightly-RUN_ID/` |

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
extraction and extracts them with the system `bsdtar`. The official Qt is
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
`nightly` whenever the variable is `true`. Neither caller passes secrets. It runs
three jobs after full CI:

1. **macOS bundle** (`macos-15`, no secrets) selects the release version (the
   product version, or the nightly version computed exactly like
   `prepare-nightly.py`), installs the pinned Qt and runs
   `scripts/ci/macos-bundle.sh`. It builds the CLI and GUI with a macOS 14.0 target,
   runs every Qt suite against the bundled Qt, assembles the production bundle,
   runs the bundle checks and self-tests and uploads `Zerus-unsigned.zip`.
2. **macOS signing and notarization** runs in the `macos-signing` environment.
   It compiles nothing and executes only scripts from the same main commit.
   `scripts/ci/macos-keychain.sh` creates a temporary keychain;
   `scripts/sign-macos.py` signs inside-out with the hardened runtime and a secure
   timestamp, submits to the notary service, staples and writes `macos-info.json`.
   The job verifies the Team ID, Gatekeeper and the stapled ticket, then deletes the
   keychain and key files even on failure. A team API key passes its issuer ID; an
   individual API key leaves `APPLE_NOTARY_API_ISSUER_ID` empty.
3. **macOS cask installation** (no secrets) renders the channel's cask, runs `brew style`,
   installs it from a local tap pointing at the candidate bytes, repeats the bundle
   checks on `/Applications/Zerus.app`, runs `zerus-setup` and `--remove` in a
   scratch home and uninstalls the cask.

Only entitlements with a concrete need are granted, and only to the application:
`com.apple.security.automation.apple-events` with `NSAppleEventsUsageDescription`
because Terminal integration runs `osascript`, whose Apple Events TCC attributes
to Zerus. Without Qt WebEngine there is no helper process that would need JIT.

The Arch assembly job then adds the signed zip and the rendered cask through
`prepare-release.py --macos` (`homebrew/zerus.rb`) or `prepare-nightly.py --macos`
(`homebrew/zerus@nightly.rb`). The candidate records release-info schema 2: every
schema 1 field is unchanged, plus `macos` (asset, Qt version, Team ID, notarization
ID, minimum macOS) and `homebrew_casks` (`["zerus"]` or `["zerus@nightly"]`).
`SHA256SUMS` covers the zip, and validation requires the cask to equal its template
in `packaging/homebrew/` with only the version, digest and, for a nightly, the
immutable `nightly-RUN_ID` tag substituted.

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
with a macOS asset. Like the Windows nightly zip, the macOS zip stays on GitHub:
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

Still required from the Apple account holder (`zerus-6a2u.2`): a **Developer ID
Application** certificate and an App Store Connect API key (team or individual)
for notarization. Store them without passing values through chat; commands without
`--body` or stdin prompt for the value:

```sh
base64 -i DeveloperIDApplication.p12 | gh secret set MACOS_DEVELOPER_ID_P12 --env macos-signing --repo ufna/zerus
gh secret set MACOS_DEVELOPER_ID_P12_PASSWORD --env macos-signing --repo ufna/zerus
gh secret set APPLE_NOTARY_API_KEY_P8 --env macos-signing --repo ufna/zerus < AuthKey_KEYID.p8
gh secret set APPLE_NOTARY_API_KEY_ID --env macos-signing --repo ufna/zerus
gh secret set APPLE_NOTARY_API_ISSUER_ID --env macos-signing --repo ufna/zerus   # team keys only
gh variable set MACOS_TEAM_ID --env macos-signing --repo ufna/zerus --body TEAMID
```

Then redeploy the mirror's `update_feed.py`, enable both casks and start a manual
nightly as the first signed build:

```sh
gh variable set MACOS_CASKS_ENABLED --repo ufna/zerus --body true
gh workflow run nightly.yml --ref main
```

If signing or notarization fails, that nightly publishes nothing; fix the
credentials and rerun, or unset the variable to return to Arch-only candidates. Rotate the certificate or
API key by replacing the secrets and revoking the old credential.

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

Not yet verified, because they need the Developer ID credentials or a published
release:

- Notarization, Gatekeeper acceptance and launch under library validation with
  one Team ID (an ad-hoc signature cannot satisfy it by design).
- Terminal automation in the signed application.
- Installation from the public tap on a clean account, and an upgrade from one
  cask version to the next with running GUI, tmux session and native agent.
- `brew style` of both casks and the channel switch between them (CI runs the
  style check in the cask installation job).
