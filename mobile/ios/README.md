# Zerus iOS

Native Swift and SwiftUI companion for the Zerus gateway protocol in
[the mobile architecture](../../docs/mobile-architecture.md). It targets full
feature parity with the [Android client](../android/README.md) and keeps the same
delivery-safety rules: a request is saved before transport, an uncertain result
is only checked through its original receipt, and cached history never
authorizes an action. iOS 18 or later is supported; the bundle ID is
`app.zerus.mobile`, shared by local Dev, TestFlight and App Store builds.

Design and roadmap: [iOS architecture and foundation](../../docs/superpowers/specs/2026-10-16-ios-foundation-design.md)
(epic `zerus-gfph` in Beads).

## Layout

- `ZerusKit/` is a Swift package with the platform-neutral core: JSON values,
  protocol models, policies ported from the Kotlin client, the relay client and
  sealed persistence. It uses Foundation, CryptoKit, Security and
  UniformTypeIdentifiers, plus the project task replica from
  `crates/replica-ffi` (module `ZerusReplica`, built by
  `scripts/build-replica.sh`), so its tests run on a Mac with `swift test`.
- `Zerus/` holds the SwiftUI app and platform services. `Zerus.xcodeproj` uses
  file-system synchronized folders, so new files are picked up without editing
  the project.
- `Config/` holds the xcconfig files and the Info.plist additions.
- `scripts/` builds and installs Dev builds.

## Private data on the phone

Paired gateways and their device credentials form one Keychain item that is
available after first unlock on this device only, never synchronized or restored
to another phone. Other private state (drafts, outgoing messages, read positions)
is sealed with AES-GCM under a Keychain key, written atomically, protected until
first unlock and excluded from backup. Each sealed file authenticates its own
name. Selected files are copied into separate sealed files whose names, types
and digests are authenticated; a message sends their verified bytes, and a copy
is removed only when no draft or outgoing message still references it. Opened
attachments and computer files are verified against their digest into bounded
caches (100 MiB, one day) and leave the app only through Quick Look, Share or an
explicit Save to Files. Voice and round video recordings use the same sealed
store: a stopped recording is a durable preview, nothing is uploaded before an
explicit Send, and an unconfirmed upload is only ever checked by its original
identity. Speech provider API keys are entered only in memory and sealed for the
processing computer before upload. Notification state (event cursor, posted card
evidence and preferences) is sealed like other private state; push wakes carry
no conversation content. Production builds require HTTPS; the relay client never follows redirects
and keeps no cookies or cached responses.

## Build and test

Requires Xcode 26 or later, and Rust 1.99.1 with the Apple targets for the
project task replica, which ZerusKit links:

```sh
rustup toolchain install 1.99.1 --profile minimal \
  --target aarch64-apple-ios,aarch64-apple-ios-sim,x86_64-apple-ios,aarch64-apple-darwin,x86_64-apple-darwin
mobile/ios/scripts/build-replica.sh             # once, and after the replica's crates change
cd mobile/ios/ZerusKit && swift test          # core tests on the Mac
xcodebuild -project mobile/ios/Zerus.xcodeproj -scheme Zerus \
  -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build
```

The shared Android contract fixtures in
`mobile/android/app/src/test/resources/` are read by the ZerusKit tests.

`build-replica.sh` builds `crates/replica-ffi` as a static library for the
iPhone, the simulator and macOS, wraps it in `ZerusReplicaFFI.xcframework` and
generates its UniFFI Swift bindings (module `ZerusReplica`) into
`ZerusKit/Replica/`, which git ignores. Xcode resolves the package before any
build phase runs, so the library must exist first. `dev-install.sh`,
`remote-dev-install.sh` (which also copies the crates to the Mac) and CI run
the script; the TestFlight workflow runs it in a step of its own, before any
signing secret is in the environment. An unchanged source and toolchain skip
the build.

## Project tasks

The Tasks button in a conversation's tools row, after Terminal, opens the
project's tasks full-screen, as on Android (`Zerus/Tasks/`, modelled by
ZerusKit's `TasksPresentation`); its badge counts the tasks In review. The
phone keeps a replica of every catalogued project's tasks in
`Application Support/task-replicas-v1/`, one folder per connection and swarm,
excluded from backup and protected until first unlock, without further
encryption; removing a connection removes its replicas. While the app is
active it checks every 15 seconds and exchanges a project with each online
computer that advertises `task_exchange` and names another digest
(`TaskSync`); edits are exchanged at once. Send now first exchanges the project
with the session's computer, then asks it for `task_send`, then exchanges
again; a receipt that cannot be confirmed, or a lost answer, holds Send now for
that task until an exchange with that computer completes. Cancel falls back to
another online computer of the project while the session's computer is out of
reach, without the cancel message. Done comes from a computer in pages of 50
(`task_done`): the last 7 days, then "Show older (N)", and with Only this
session only the tasks sent to it; opening a Done task fetches it into the
replica. The task input has no microphone, and tasks send no notifications.
See `docs/superpowers/specs/2026-10-20-task-tracker-ios-design.md`.

## Dev builds on an attached iPhone

On the Mac the phone is attached to, set the Apple Developer team once in the
ignored `Config/Local.xcconfig` (copy `Local.xcconfig.example`), in
`~/.config/zerus-ios/Local.xcconfig`, or in `ZERUS_IOS_TEAM_ID`. Xcode needs a
signed-in Apple ID of that team (Xcode → Settings → Accounts) to create the
development provisioning profile; alternatively provide an App Store Connect API
key through `ZERUS_IOS_ASC_KEY_PATH`, `ZERUS_IOS_ASC_KEY_ID` and
`ZERUS_IOS_ASC_ISSUER_ID`. Then:

```sh
mobile/ios/scripts/dev-install.sh                 # Release build, install and launch
mobile/ios/scripts/dev-install.sh --device NAME   # when several iPhones are attached
mobile/ios/scripts/dev-install.sh --simulator     # the booted simulator, Debug
```

Builds share one DerivedData folder in `~/Library/Caches/zerus-ios/` and wait
for each other through a lock, so parallel sessions do not corrupt it. A local
build reports `X.Y.Z-local` with its Git revision (`+dirty` for uncommitted
changes) in Settings → About and never claims a CI build number.

From another computer, build the current worktree (including uncommitted
changes) on that Mac:

```sh
mobile/ios/scripts/remote-dev-install.sh mac-ssh-alias
```

The wrapper copies `mobile/ios` to `~/Library/Caches/zerus-ios/src/` on the Mac
with `rsync` over `ssh -o BatchMode=yes` and runs `dev-install.sh` in a login
shell. The Mac's team must be configured in `~/.config/zerus-ios/Local.xcconfig`.

Debug builds accept `-zerus-demo` (open the read-only preview) and
`-zerus-tab NAME` launch arguments for screenshots, for example
`dev-install.sh --simulator -- -zerus-demo -zerus-tab machines`.

## TestFlight and the App Store

Dev builds reach TestFlight internal testers from every successful main CI run,
and promoted ones the [public beta](https://testflight.apple.com/join/fywRuW6y);
Stable builds are uploaded on demand and submitted for review. Uploads use an App
Store Connect API key in a main-only environment. See
[iOS distribution](../../docs/ios-distribution.md) for the channels, the one-time
setup and the submission checklist.

## Versions

`DEV_VERSION` names the next planned iOS release; `Config/Base.xcconfig`
repeats it as `MARKETING_VERSION` (checked by `scripts/ci/check-source.py`). App
Store versions are three integers, so a Dev build carries its CI sequence as the
build number and shows `X.Y.Z-dev.N` in About.

## App icon

`scripts/render-app-icon.swift` renders `Zerus/Assets.xcassets/AppIcon.appiconset/AppIcon.png`
from the Swarm blades of `tray/resources/icons/hgs-zerus-symbolic.svg`, with the
Android adaptive-icon transforms: a mint `#67E8CB` mark on ink `#101517`.
