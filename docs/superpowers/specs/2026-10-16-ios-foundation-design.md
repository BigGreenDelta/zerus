# iOS client: architecture and foundation (#0)

Status: design for epic `zerus-gfph`, sub-project #0 (`zerus-gfph.1`). The owner
approved the scope, approach and decomposition on 2026-10-16, confirmed the
bundle ID, delegated the remaining choices and asked for autonomous completion
("работай до конца пока не потребуется явное мое вмешательство"). Choices marked
*builder's call* were made without owner review and can be revisited.

## Intent

Zerus has a full-featured Android companion. iOS users need the same client:
pair a gateway, browse projects and sessions across machines, read and answer
native conversations, control sessions, launch new ones, attach files, use the
terminal, record voice and video messages and receive alerts. The iOS app must
keep the delivery-safety guarantees of `docs/mobile-architecture.md` exactly:
durable request identities, no automatic replay, exact-target verification and
honest uncertain states.

## Owner decisions

- Full feature parity with Android, not an MVP.
- A separate native Swift/SwiftUI client in `mobile/ios`. Port logic from the
  Kotlin client; do not refactor Android. Shared JSON contract fixtures, read by
  both test suites, keep subtle rules aligned.
- Sub-projects in order: #0 foundation and Dev build on the attached iPhone,
  #9 TestFlight/App Store, #1 sessions and machines, #2 conversation, #3-#7
  (controls, launch, attachments/files, terminal, voice/video), #8 notifications.
- Bundle ID `app.zerus.mobile`, one app ID for local Dev, TestFlight and the
  App Store, like the single Android package.
- Minimum iOS: delegated. *Builder's call:* **iOS 18**. It supports the same
  devices as iOS 17 (iPhone XS/XR and later) and adds scroll-visibility APIs that
  the read-position contract needs.

## Goals

- Protocol version 1 compatibility with existing relays and connectors; no
  relay or connector change is required for #0-#7.
- The same safety rules as Android: a mutation is durably recorded before
  transport, uncertain results are only checked through the original receipt,
  and cached data never authorizes an action.
- A fast, battery-friendly native app: parsing, merging and filtering run off
  the main actor; lists use stable identities; polling runs only while visible.
- Reproducible Dev builds on the attached test iPhone with one command, and the
  same command from an agent on another computer.

## Non-goals and anti-goals

- No shared Kotlin code, no Compose Multiplatform, no web views for messages.
- No in-app updater: iOS updates through TestFlight and the App Store.
- No foreground "live connection" service: iOS has none. Background behavior
  comes from APNs (#8).
- Failure even if it works: an iOS build that silently resends a message,
  answers a different question revision, treats cached history as authority,
  stores credentials outside the Keychain or leaks conversation text into push
  payloads, logs or analytics.

## Architecture

```
mobile/ios/
  Zerus.xcodeproj          hand-written project, file-system synchronized groups
  Zerus/                   app target: SwiftUI views, app model, platform services
  ZerusKit/                local Swift package: platform-neutral core + tests
  Config/                  xcconfig files; Local.xcconfig (ignored) holds the team
  scripts/                 dev-install.sh, remote-dev-install.sh
  DEV_VERSION              next planned iOS release (marketing version)
  README.md
```

### ZerusKit (core package)

Pure Swift 6 (strict concurrency) with Foundation, CryptoKit and Security only,
so `swift test` runs on a macOS host in seconds and CI needs no simulator for
logic tests. It contains:

- `JSON`: an immutable JSON value with the Android `string("a","b")` style
  accessors. Native inspection results stay raw, as on Android (`raw: JSONObject`),
  because the client must preserve and compare unknown fields.
- Protocol models (`Connection`, `Machine`, `Target`, `Session`, `Event`,
  `Question`, drafts, outgoing envelopes, session actions, context operations)
  and the parsers ported from `RelayApi.kt`/`NativeParser`.
- `RelayClient` (actor): URLSession without redirects, cookies or URL cache;
  10 s connect / 35 s request timeouts; 1 MiB response cap; 29 MiB request cap
  for `/v1/requests`, 1 MiB otherwise; receipt identity checks; `await` polling.
- Policies ported one-to-one from Kotlin objects (filters, presentation, merge,
  question tracking, read state, notifications, launch, worktrees, terminal ANSI).
  Each keeps the Kotlin name so reviewers can diff behavior.
- Persistence: `OrderedStatePersistence` (actor) with the Android semantics:
  immediate in-memory edits, a 200 ms debounce and 800 ms maximum delay for
  ordinary writes, and `durable` writes that complete before transport.
  `SealedFileStore` encrypts JSON with AES-GCM (CryptoKit) under a 256-bit key
  stored in the Keychain (`AfterFirstUnlockThisDeviceOnly`), writes atomically
  with `completeUntilFirstUserAuthentication` protection and excludes files
  from backup. Device credentials are separate Keychain items.

### App target

- `AppModel` (`@MainActor @Observable`) mirrors `ZerusViewModel`: it owns
  navigation identity, flights and gates, and hands heavy work to ZerusKit on
  background tasks. Feature controllers (speech, attachments, computer files,
  launch, terminal) are separate observable objects, as on Android.
- SwiftUI screens follow the Android information architecture with native iOS
  idioms: a tab bar (Sessions, Projects, Drafts, Accounts, Machines), a
  navigation stack per tab, the conversation pushed full width, Settings as a
  sheet. Colors come from the shared dark palette (mint `#67E8CB`, ink
  `#101517`, surface `#1A2225`, muted `#9AACB2`, amber `#FFCB7D`). Desktop
  glyphs (`DesktopIcons.kt`) are ported as SwiftUI `Shape`s.
- Platform services: Keychain, notifications, audio (AVAudioEngine 16 kHz mono
  PCM), camera (AVFoundation), QR scanning (AVCaptureMetadataOutput), Quick Look
  and share sheets, `UIDocumentPicker`.
- No third-party dependencies in #0. Markdown uses Foundation's CommonMark/GFM
  parser (`AttributedString` with full syntax) rendered by native views, with the
  Android limits (256 KiB source, 32 nesting levels, 8192 nodes, 4096 cells).
  *Builder's call*; it keeps the app free of third-party notices for now.

### Shared contracts

Android fixtures under `mobile/android/app/src/test/resources/` are read by the
ZerusKit tests through a path relative to the test source file. No Android
build change is needed. New shared fixtures go to the same folder.

## Foundation (#0) deliverables

1. Xcode project (objectVersion 77, synchronized groups, no generator) with
   one app target `Zerus` and the local `ZerusKit` package; Swift 6; iOS 18.
2. Versions: `MARKETING_VERSION` from `mobile/ios/DEV_VERSION`;
   `CURRENT_PROJECT_VERSION` is the CI sequence for CI and TestFlight builds
   and `0` for local builds. Info.plist keys `ZerusBuildChannel` (`local`,
   `dev`, `stable`) and `ZerusSourceRevision` (commit, `+dirty` when
   applicable). About shows `X.Y.Z-dev.N` for Dev and `X.Y.Z-local (commit)`
   locally; a local build never claims a CI number. App Store versions stay
   three integers, as Apple requires.
3. Signing: automatic, with `DEVELOPMENT_TEAM` read from the ignored
   `Config/Local.xcconfig` or `ZERUS_IOS_TEAM_ID`. Simulator and CI builds do
   not sign. No team ID, certificate or device identifier is committed.
4. `scripts/dev-install.sh`: builds Release with development signing and
   `-allowProvisioningUpdates`, into one bounded DerivedData folder
   (`~/Library/Caches/zerus-ios/DerivedData`, serialized by a lock), installs
   with `xcrun devicectl device install app` and launches with
   `devicectl device process launch`. It selects the only connected iPhone or an
   explicit `--device`. It never touches desktop installations, LaunchAgents or
   other apps.
5. `scripts/remote-dev-install.sh HOST`: from Linux, rsyncs `mobile/ios` (with
   the local Git revision metadata) to a scratch folder on the Mac over
   `ssh -o BatchMode=yes` and runs `dev-install.sh` through a login shell.
6. Walking skeleton proving the stack on a device: pairing (`POST /v1/pair`,
   HTTPS only, credential in the Keychain), `zerus://pair` links with explicit
   confirmation, `GET /v1/capabilities`, `GET /v1/computers`, machines list,
   disconnect (`DELETE /v1/device`), and the read-only demo.
7. CI: an `ios` job in `checks.yml` on a standard `macos-15` runner, selecting
   the newest installed Xcode 26 or later. It runs `swift test` for ZerusKit
   and builds the app for the simulator without signing. It is part of the
   suite gate whenever macOS coverage runs.
8. Docs: `mobile/ios/README.md`, the README mobile section and the status line
   of `docs/mobile-architecture.md`.

## Later sub-projects (summary)

- #9 TestFlight/App Store: archive and upload from trusted main with an App
  Store Connect API key in a protected environment, privacy manifest, export
  compliance (`ITSAppUsesNonExemptEncryption = NO`; only standard HTTPS/CryptoKit),
  App Store metadata. Owner actions: create the app record, the API key and the
  environment secrets. *Builder's call:* no Firebase on iOS; Apple's crash and
  usage reporting (App Store Connect, MetricKit) replaces Crashlytics/Analytics,
  so the privacy label declares no tracking and no third-party collection.
- #1-#7 port the Android features listed in `mobile/android/README.md`.
- #8 notifications: APNs. *Builder's call:* the relay gains an additive
  `apns` push provider (token-based APNs HTTP/2 with private server
  configuration), advertised in `push_providers`, so the app needs no Firebase
  SDK. Wakes stay generic and the app fetches authenticated events, as Android.

## Testing

- ZerusKit unit tests mirror the Kotlin unit tests for every ported policy and
  read the shared fixtures. They run with `swift test` locally and in CI.
- The app builds for the simulator in CI. Device checks (Keychain persistence,
  notifications, microphone, camera) are manual on the test iPhone and are
  recorded in the sub-project's Beads notes.

## Left to the builder

File-level structure inside the targets, view composition, and the exact iOS
presentation of each Android screen, as long as the behavior and safety rules
match the Android README and `docs/mobile-architecture.md`.
