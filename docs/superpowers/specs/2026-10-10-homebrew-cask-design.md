# Homebrew Cask Distribution for macOS Design

Tracking epic: `zerus-6a2u`.

## Goal

Let anyone with an Apple Silicon Mac install and update Zerus with Homebrew:

```sh
brew install --cask ufna/zerus/zerus
zerus-setup
```

Installation must not show Gatekeeper warnings, must not require local
compilation and must keep the guarantees of the Arch packages: installing or
upgrading never stops native agents, tmux servers, DeepSeek hosts or the running
GUI, and never edits a home directory without an explicit user command.

Non-goals for this stage: Intel Macs, universal binaries, a nightly cask, a
CLI-only formula for headless Macs, submission to the official `homebrew/cask`
repository, Sparkle or any other in-app updater, and Mac App Store distribution
(Qt WebEngine is incompatible with the App Store sandbox).

## Owner decisions (2026-10-10)

| Topic | Decision |
| --- | --- |
| Audience | Public users, one-command install and `brew upgrade` |
| Package type | Cask in the owner's tap `ufna/homebrew-zerus` |
| Signing | Developer ID signature, hardened runtime, notarization and stapling |
| Architecture | Apple Silicon (`arm64`) only |
| Channels | Stable first; nightly is a later stage reusing the same jobs |
| Qt | Pinned official Qt from the Qt online repository, embedded in the bundle |
| Signing location | Hosted CI, dedicated main-only `macos-signing` environment |
| Tap writes | Write deploy key held only by the `release` environment |

## User-visible behavior

### Install

`brew install --cask ufna/zerus/zerus` downloads the notarized
`zerus-<version>-macos-arm64.zip`, moves `Zerus.app` to `/Applications` and links
`hgs` and `zerus-setup` into `$(brew --prefix)/bin`. Homebrew installs `tmux` as a
formula dependency. Nothing is started and nothing is written to the home
directory. The cask caveats tell the user to run `zerus-setup` once.

### `zerus-setup` on macOS

Run once as the desktop user, without sudo. It is idempotent and checks every
conflict before changing anything.

Default actions:

- Create `~/.local/bin/hgs` and `~/.local/bin/hgs_state.py` symlinks into
  `/Applications/Zerus.app`. Remote SSH protocols call `~/.local/bin/hgs`; the
  bundle path is stable across upgrades.
- Install the `com.hgdev.hgs.user-session` LaunchAgent. It is an on-demand Mach
  service, not a background process. Without it an `hgs` started over SSH cannot
  join the desktop security session, so Claude cannot reach the login Keychain.

Opt-in flags:

- `--autostart`: install the `com.hgdev.hgs-tray` LaunchAgent for the bundled GUI.
- `--workers`: install the recovery and swarm LaunchAgents.
- `--remove`: unload and remove only the LaunchAgents and links that point into
  `/Applications/Zerus.app`. Recommended before `brew uninstall`.

Setup never passes `--restart` to the service installers, never deletes a file it
did not create and refuses to replace an existing file or a symlink that points
elsewhere. It prints the existing path and stops before any change, exactly like
`packaging/linux/zerus-setup`.

Migration from a source installation is explicit. Setup detects
`~/Applications/hgs-tray.app`, a regular file at `~/.local/bin/hgs` and existing
LaunchAgents whose program path is outside the bundle. It reports each one with
the documented manual migration steps and changes nothing. Two bundles with the
same bundle ID produce a warning, because LaunchServices may open either one.

### GUI locating `hgs`

When neither `--hgs` nor the config file sets a path, the macOS GUI first uses an
executable `hgs` next to its own executable (`Contents/MacOS/hgs`), then `PATH`.
A Finder or Spotlight launch has no Homebrew directory in `PATH`; without this
step the bundled GUI would show an empty fleet. Source installations have no
sibling `hgs` and keep the current behavior.

### Upgrade

`brew upgrade --cask zerus` replaces `/Applications/Zerus.app`. The cask has no
`uninstall quit:` or `uninstall launchctl:` directives, because upgrade can apply
the previous version's uninstall directives; the running GUI, workers, session
bridge, tmux and agents keep running. Running processes keep their mapped code.

A running GUI can still lazily load plugins or start `QtWebEngineProcess` from
the new bundle. The GUI therefore compares its own version with the bundle's
on-disk `CFBundleShortVersionString` when it checks for updates and when it is
activated. On a mismatch it shows **Restart to finish update** in the tray menu and
status bar. Restarting is the user's choice; composer drafts are already
persisted by `ComposerDraftStore`. Workers keep running the previous binary until
they are restarted deliberately, as with Arch package upgrades; the documentation
lists the commands.

### Update checks

Desktop update ownership detection gains a Homebrew cask owner: the canonical
running executable is inside `/Applications/Zerus.app` and
`$(brew --prefix)/Caskroom/zerus/<installed version>` exists, with the prefix
taken from the executable's real path or the standard `/opt/homebrew`. The UI
copies `brew upgrade --cask zerus` and never runs it. The sentence "There is
currently no published macOS installer" in `docs/public-distribution.md` is
replaced.

### Uninstall

`zerus-setup --remove` followed by `brew uninstall --cask zerus` removes the
application, links and Zerus LaunchAgents. `brew uninstall --zap` additionally
removes `~/Library/Logs/hgs-tray.log`, `~/Library/Logs/hgs`, the saved
application state and the four Zerus LaunchAgent plists. Nothing removes
`~/.config/hgs`, agent state, conversations or the tmux configuration.

## Release asset and bundle

The stable GitHub release `v<version>` gains one asset,
`zerus-<version>-macos-arm64.zip`, created with `ditto -c -k --keepParent` from
the stapled bundle. It is listed in `SHA256SUMS` and `release-info.json`.
Existing v0.37.0 assets are immutable, so the first cask ships with the next
stable product version.

`Zerus.app` layout:

| Path | Content |
| --- | --- |
| `Contents/MacOS/hgs-tray` | GUI executable; name unchanged |
| `Contents/MacOS/hgs` | CLI executable |
| `Contents/Frameworks/` | Qt frameworks from `macdeployqt`, including `QtWebEngineCore.framework` and its `QtWebEngineProcess.app` helper |
| `Contents/PlugIns/` | Qt plugins required at runtime (cocoa platform, SVG image format, others reported by `macdeployqt`) |
| `Contents/Resources/bin/zerus-setup` | macOS setup command |
| `Contents/Resources/setup/` | `hgs_state.py`, the three LaunchAgent installer scripts and the tray plist template |
| `Contents/Resources/doc/` | `tmux.conf` sample and README |
| `Contents/Resources/licenses/` | Zerus MIT, vendored libvterm and md4c, Cargo dependency notices, Qt LGPLv3 and module attributions, Chromium third-party notices |

`Info.plist` keeps `CFBundleIdentifier` `com.hgdev.hgs-tray`, sets
`CFBundleName` to `Zerus` and sets `LSMinimumSystemVersion` to `13.0`, the Qt 6.11
minimum. `QCoreApplication` organization and application names are unchanged,
so `QSettings` and draft storage paths stay the same.

Qt is the official **Qt 6.11.2** `clang_64` build plus the `qtwebengine`
extension, `qtwebchannel` and `qtpositioning`. It is the version macOS CI already
tests through Homebrew, Qt 6.11 supports macOS 13 or later (Qt 6.12 requires
14.4), and its exact sources are public (`qt-everywhere-src-6.11.2` and `v6.11.2`
tags). Qt 6.11.3 binaries exist in the online repository, but its sources were not
published on 2026-10-10, so it cannot be redistributed with an LGPL source pointer.
The QtNetwork and QXmlStreamReader security fixes published as 6.11 patches in
September are already present in the v6.11.2 sources; the remaining patches touch
modules the bundle does not ship (QtXml, QtConnectivity) or QtQuick SVG parsing.
The official frameworks are universal; the bundle is thinned to `arm64` with
`ditto --arch arm64` before signing.

## Cask

`packaging/homebrew/zerus.rb.in` is the template. Candidate assembly substitutes
only `@VERSION@` and `@SHA256@`.

```ruby
cask "zerus" do
  version "@VERSION@"
  sha256 "@SHA256@"

  url "https://github.com/ufna/zerus/releases/download/v#{version}/zerus-#{version}-macos-arm64.zip"
  name "Zerus"
  desc "ADE for working with a fleet of AI coding agents"
  homepage "https://zerus.dev"

  depends_on arch: :arm64
  depends_on macos: ">= :ventura"
  depends_on formula: "tmux"

  app "Zerus.app"
  binary "#{appdir}/Zerus.app/Contents/MacOS/hgs"
  binary "#{appdir}/Zerus.app/Contents/Resources/bin/zerus-setup"

  zap launchctl: [
        "com.hgdev.hgs-recovery",
        "com.hgdev.hgs-swarm",
        "com.hgdev.hgs-tray",
        "com.hgdev.hgs.user-session",
      ],
      trash:     [
        "~/Library/LaunchAgents/com.hgdev.hgs-recovery.plist",
        "~/Library/LaunchAgents/com.hgdev.hgs-swarm.plist",
        "~/Library/LaunchAgents/com.hgdev.hgs-tray.plist",
        "~/Library/LaunchAgents/com.hgdev.hgs.user-session.plist",
        "~/Library/Logs/hgs",
        "~/Library/Logs/hgs-tray.log",
        "~/Library/Saved Application State/com.hgdev.hgs-tray.savedState",
      ]

  caveats <<~EOS
    Run this once as your user, without sudo:
      zerus-setup
    It creates ~/.local/bin/hgs for SSH and the Keychain session bridge.
    Optional: zerus-setup --autostart --workers
    Before uninstalling: zerus-setup --remove
  EOS
end
```

There is no `postflight`, `uninstall quit:`, `uninstall launchctl:` or
`auto_updates`. `zap` runs only on an explicit `brew uninstall --zap`; it unloads
and removes these four labels even when a source installation created them, and
the documentation says so. `brew style` and `brew audit --cask --strict` must pass on the
rendered cask. `tmux` 3.8 is the current Homebrew version; the existing runtime
check keeps rejecting delivery when tmux lacks the required input mode.

## Build and publication pipeline

The existing structure stays: manual `release.yml` builds a sealed candidate,
manual `publish.yml` publishes that exact candidate.

### 1. `macos-bundle` job (release.yml, `macos-15`, no secrets)

1. Install the pinned Qt with `scripts/install-qt-macos.py`. The pin file
   `packaging/macos/qt.json` records the Qt version, the repository packages and
   the size and SHA-256 of every archive, locked from Qt's official `.sha256`
   files. Each archive is verified before any is extracted with the system
   `bsdtar`; mirrors may serve the bytes. The official Qt is relocatable and needs
   no path patching.
2. Build the CLI with `cargo build --locked --release` and the GUI with
   `BUILD_TESTING=ON` against that Qt; run all CTest suites and the macOS CLI
   integration suite.
3. Build the production GUI with `BUILD_TESTING=OFF` from the same commit, run
   `macdeployqt`, add `hgs`, setup files and notices, thin to `arm64` and rename
   the bundle to `Zerus.app`.
4. `scripts/check-macos-bundle.py` rejects any Mach-O load command or rpath that
   points outside the bundle or `/System`/`/usr/lib`, any missing notice, a wrong
   bundle ID or version, and any non-`arm64` slice. It also runs
   `Zerus.app/Contents/MacOS/hgs-tray --selftest --hgs Zerus.app/Contents/MacOS/hgs`
   and `hgs --version`.
5. Upload the unsigned bundle as a short-lived job artifact.

### 2. `macos-sign` job (release.yml, `macos-15`, environment `macos-signing`)

The job compiles nothing. It checks out the same trusted commit and runs only
`scripts/sign-macos.py` on the downloaded unsigned bundle:

1. Create a temporary keychain with a random password, import the Developer ID
   Application certificate and allow `codesign` access. The keychain and the
   decoded key files are deleted in an `always()` step.
2. Sign inside-out: dylibs and plugins, frameworks, `QtWebEngineProcess.app`,
   `hgs`, then `Zerus.app`, all with `--options runtime --timestamp`.
   `QtWebEngineProcess` receives `allow-jit`; the application receives the Apple
   Events entitlement because Terminal integration runs `osascript`, whose events
   TCC attributes to Zerus (see verification item V2).
3. Submit with `xcrun notarytool submit --wait` using the App Store Connect API
   key; on rejection, fetch and print the notarization log and fail.
4. `xcrun stapler staple Zerus.app`, then verify: `codesign --verify --deep
   --strict`, TeamIdentifier equals `MACOS_TEAM_ID`, `spctl -a -t exec -vv`
   reports a notarized Developer ID source, and `xcrun stapler validate`.
5. Create the release zip and upload it with its SHA-256, the notarization
   submission ID and the Qt pin as the signed artifact.

### 3. Candidate sealing (existing assembly job)

Candidate assembly adds the macOS zip to the release assets, renders the cask
from the template, and records in `release-info.json` (schema 2) the macOS asset
name and digest, Qt version, Team ID, notarization submission ID and source
commit. Schema 1 candidates stay readable by the publisher. `SHA256SUMS` covers
the zip. `macos_signed=false` is an explicit candidate input for dry runs before
the Apple secrets exist; such a candidate cannot be published.

### 4. Publication (publish.yml, `release` environment)

In addition to the current checks, the publisher verifies the zip digest against
the seal and GitHub's artifact digest, and verifies that the candidate's
`zerus.rb` equals the repository template with exactly the sealed version and
digest substituted. No candidate code is executed.

Order: GitHub release (all assets verified anonymously), AUR recipes, then the
tap. The tap step clones `git@github.com:ufna/homebrew-zerus.git` with
`HOMEBREW_TAP_DEPLOY_KEY`, strict host checking against
`packaging/homebrew/known_hosts` (GitHub's published Ed25519 key), and writes
`Casks/zerus.rb`. It refuses a version downgrade, treats an identical file as a
no-op and pushes without force. A failed tap push is retried by rerunning the
publisher with the same candidate run; nothing is rebuilt.

## GitHub configuration

Completed on 2026-10-10 by the owner's request:

- Public repository `ufna/homebrew-zerus` with README, MIT license and
  `Casks/.gitkeep`; issues, wiki and projects disabled. Ruleset "Protect main"
  blocks deletion and non-fast-forward pushes of the default branch.
- Write deploy key "Zerus publish.yml (release environment)"; its private key
  exists only as the `release` environment secret `HOMEBREW_TAP_DEPLOY_KEY`.
- Environment `macos-signing` with the same custom branch policy as `release`:
  branch `main` only.

Owner action (`zerus-6a2u.2`): the keychain on the owner's Mac currently holds
only an Apple Development identity, which cannot sign Developer ID distribution.
The Apple account holder creates a **Developer ID Application** certificate and
an App Store Connect **team API key** for notarization, then stores them without
passing values through chat:

```sh
base64 -i DeveloperIDApplication.p12 | gh secret set MACOS_DEVELOPER_ID_P12 --env macos-signing --repo ufna/zerus
gh secret set MACOS_DEVELOPER_ID_P12_PASSWORD --env macos-signing --repo ufna/zerus
gh secret set APPLE_NOTARY_API_KEY_P8 --env macos-signing --repo ufna/zerus < AuthKey_KEYID.p8
gh secret set APPLE_NOTARY_API_KEY_ID --env macos-signing --repo ufna/zerus
gh secret set APPLE_NOTARY_API_ISSUER_ID --env macos-signing --repo ufna/zerus
gh variable set MACOS_TEAM_ID --env macos-signing --repo ufna/zerus --body TEAMID
```

Commands without `--body` or stdin prompt for the value without echoing it.

## Licensing

Arch packages link system Qt; the Mac bundle distributes Qt. Qt and Qt WebEngine
are used under LGPLv3 as unmodified, dynamically linked frameworks, which keeps
them replaceable. The bundle includes the LGPLv3 and GPLv3 texts, attributions
for every bundled Qt module, Chromium's third-party notices for the pinned
WebEngine version and a written pointer to the exact Qt 6.11.2 source archive on
`download.qt.io`. `scripts/check-macos-bundle.py` fails when any of these files
is missing. `THIRD_PARTY_NOTICES.md` documents the bundled set.

## Testing and validation

Automated, in the release candidate run:

- Unit tests for `zerus-setup` on macOS in a synthetic home: default links,
  session bridge plist, `--autostart`, `--workers`, `--remove`, idempotence,
  conflict detection before any change, foreign symlink preservation, root
  refusal and source-install detection. They run under `/usr/bin/python3` on
  the runner; Homebrew requires the Command Line Tools, so that interpreter is
  available to users, and the scripts must stay compatible with Python 3.9.
- Qt tests for sibling `hgs` discovery and for the Restart to finish update
  state.
- Update ownership tests for a synthetic Caskroom layout.
- Publication contract tests: schema 2 seal, template-only cask differences,
  tap downgrade refusal, identical-file no-op, retry after a failed tap push and
  candidates built with `macos_signed=false` being rejected.
- After signing, a separate job without secrets installs the cask from a local
  tap with `brew install --cask`, repeats the bundle, Gatekeeper and self-test
  checks on `/Applications/Zerus.app`, runs `zerus-setup` and `--remove` in a
  scratch home and uninstalls the cask. Loading a WebEngine view and Terminal
  automation in the signed application are part of the manual validation below,
  because the GUI has no headless WebEngine entry point.

Manual, before the first public cask:

- On a clean user account of a real Apple Silicon Mac: install from the public
  tap with quarantine applied, confirm no Gatekeeper prompt, run setup, launch
  from Finder, start an SSH-launched Claude session that reaches the Keychain.
- Upgrade test: install version N, start the GUI, a tmux session and a native
  agent, upgrade to N+1 and confirm all processes survive, the drafts persist
  and Restart to finish update appears.

## Verification items before implementation details are fixed

- **V1 Qt download integrity.** Resolved on 2026-10-10: archives are pinned by
  size and SHA-256 from Qt's official `.sha256` files and verified before
  extraction; no third-party installer is used.
- **V2 WebEngine entitlements.** Start with `com.apple.security.cs.allow-jit` on
  `QtWebEngineProcess` only. Add another entitlement only with evidence that a
  notarized hardened-runtime WebEngine view fails without it.
- **V3 Python 3.9.** Run the three existing LaunchAgent installers and the new
  setup under the runner's `/usr/bin/python3`.
- **V4 Upgrade semantics.** Confirm on a real install that `brew upgrade --cask`
  without uninstall directives leaves running processes untouched.

## Documentation

- `docs/installation.md`: a macOS section with the cask, setup, autostart,
  migration from a source install and uninstall.
- New `docs/macos-distribution.md`: bundle layout, Qt pin, signing, secrets,
  candidate/publish flow, retry rules and the validation record.
- `docs/ci-and-aur.md`: links to the macOS publication document and the new
  release job.
- `docs/public-distribution.md`: Homebrew ownership in update checks.
- `README.md`: the one-line Homebrew install next to AUR.

## Rollout

1. Bundle job, bundle checks, setup command and runtime GUI changes; candidate
   runs with `macos_signed=false`.
2. Signing job and verification items V1–V4 once the Apple secrets exist.
3. Publication tap step and contract tests.
4. Next stable release with the first public cask, followed by the manual
   validation above.
5. Later stages, each separately approved: nightly cask, CLI-only formula and
   submission to `homebrew/cask`.

## Addendum: nightly channel (2026-10-12)

The owner asked for Homebrew tracks matching the update channels ("stable /
nightly / dev, or stable only if not possible"). Implemented:

- `zerus@nightly` cask from `packaging/homebrew/zerus@nightly.rb.in`, published with
  every Nightly run next to `zerus-ade-nightly-bin`. Its version is the nightly
  version (`0.42.0.r1234.gabcdef0.n56`); its URL names the immutable
  `nightly-RUN_ID` prerelease. `zerus` and `zerus@nightly` install the same
  `Zerus.app` and conflict with each other.
- The macOS jobs moved into the reusable `macos-cask.yml` (input `channel`), called
  by `release.yml` and `nightly.yml`. Both run them only while the repository
  variable `MACOS_CASKS_ENABLED` is `true`, so candidates stay Arch-only until the
  Apple credentials exist. A failed cask fails the candidate.
- Nightly candidates use schema 2 with `homebrew_casks: ["zerus@nightly"]`; the tap
  publisher updates only the channel's cask file and orders nightlies by Nightly
  run number.
- The bundle records `ZerusReleaseVersion`; the GUI offers **Restart to finish
  update** when it changes, recognizes either cask and switches casks with
  `brew uninstall --cask A && brew install --cask ufna/zerus/B`.

No Dev cask: Dev builds run on every main push and publish only to the website
mirror. A cask would need a signed, notarized bundle and a tap commit per push;
`zerus@nightly` covers prebuilt builds from main.
