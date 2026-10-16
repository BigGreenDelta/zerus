# iOS foundation (#0) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create `mobile/ios` with a Swift package core, an Xcode app, a
one-command Dev install on the attached iPhone, a pairing/machines walking
skeleton and a CI job.

**Architecture:** `ZerusKit` (Swift package, Foundation/CryptoKit/Security only)
holds models, policies, the relay client and sealed persistence and is tested
with `swift test`. The `Zerus` app target holds SwiftUI and platform services.

**Tech Stack:** Swift 6, SwiftUI, iOS 18, Xcode 26+, XCTest, URLSession, CryptoKit.

**Spec:** `docs/superpowers/specs/2026-10-16-ios-foundation-design.md`

## Global Constraints

- Bundle ID `app.zerus.mobile`; deployment target iOS 18.0; Swift 6 language mode.
- HTTPS only; no redirects, cookies or URL cache; 1 MiB response cap; 29 MiB
  (30,408,704 bytes) request cap for `/v1/requests`, 1 MiB for other bodies.
- Pairing sends `device_name` `Zerus iOS`; the suggested relay is `https://relay.zerus.dev`.
- Credentials only in the Keychain (`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`).
- No team ID, UDID, certificate or absolute home path in the repository.
- No third-party dependencies.

## Review Focus

- A pasted `zerus://pair` link with a duplicated or percent-encoded key must be
  rejected, not partially applied (PairingInvite tests).
- A dropped connection during `POST /v1/requests` must not be retried
  (RelayClient test counts requests).
- A response above 1 MiB must fail before decoding (RelayClient test).
- A gateway answering with another `request_id` must surface as unconfirmed
  (RelayClient receipt identity test).
- A Keychain failure while pairing must not save a half connection (AppModel
  pairing saves token first, then the connection list; failure leaves neither).

---

### Task 1: ZerusKit package, JSON value and endpoint/pairing policies

**Files:**
- Create: `mobile/ios/ZerusKit/Package.swift`
- Create: `mobile/ios/ZerusKit/Sources/ZerusKit/JSON.swift`, `Pairing.swift`, `Models.swift`
- Test: `mobile/ios/ZerusKit/Tests/ZerusKitTests/JSONTests.swift`, `PairingTests.swift`

**Interfaces:**
- Produces: `enum JSON: Sendable, Hashable` with `parse(Data) throws`,
  `data() throws -> Data`, subscript by key/index, `string(_ keys: String...)`,
  `bool(_:)`, `int(_:)`, `double(_:)`, `object(_:)`, `array(_:)`.
- Produces: `EndpointPolicy.normalize(_:) throws -> String`,
  `ManagedRelay.current`, `ManagedRelay.transport(_:) -> String`,
  `PairingInvite(server:code:)` with `static parse(_:) -> PairingInvite?`.
- Produces: `struct Connection: Codable, Sendable, Hashable { id, label, url, token }`
  with `endpoint` and `displayName`; `struct Target` with `key` and `json()`.

- [ ] Port `PairingInviteTest`, `ManagedRelayTest.onlyExactManagedHttpsRootMigrates`,
  `RelaySafetyTest.serverUrlRequiresHttps…` and `destinationIncludes…` assertions
  verbatim to XCTest.
- [ ] Run `swift test` in `mobile/ios/ZerusKit`; expect failures (missing types).
- [ ] Implement; Java `URI` semantics: reject userinfo, query, fragment, port
  outside 1-65535, whitespace/control characters, non-`https` schemes.
- [ ] Run `swift test`; expect PASS. Commit.

### Task 2: RelayClient

**Files:**
- Create: `mobile/ios/ZerusKit/Sources/ZerusKit/RelayClient.swift`
- Test: `mobile/ios/ZerusKit/Tests/ZerusKitTests/RelayClientTests.swift` (URLProtocol stub)

**Interfaces:**
- Produces: `actor RelayClient` with `init(session: URLSession = .zerusRelay)`,
  `call(url:token:path:body:method:) async throws -> JSON`, `pair(url:code:) async throws -> Connection`,
  `computers(_:)`, `capabilities(_:)`, `submit(_:target:operation:payload:id:)`,
  `receipt(_:id:)`, `await(_:initial:maxPolls:pollDelay:)`, `revoke(_:)`;
  `struct RelayError: Error { status: Int; message: String }`.

- [ ] Tests: one request on a dropped connection; 1 MiB + 1 response rejected;
  mismatched receipt rejected; legacy managed host sent to `relay.zerus.dev`;
  bearer header; `await` reads only `GET /v1/requests/{id}`.
- [ ] Implement with a no-redirect delegate and streamed byte counting. PASS. Commit.

### Task 3: Sealed persistence and credentials

**Files:**
- Create: `Sources/ZerusKit/SealedFileStore.swift`, `OrderedStatePersistence.swift`, `CredentialStore.swift`
- Test: `Tests/ZerusKitTests/PersistenceTests.swift`

**Interfaces:**
- Produces: `protocol KeyProvider: Sendable { func key() throws -> SymmetricKey }`,
  `KeychainKeyProvider`, `SealedFileStore(directory:keys:)` with
  `read<T: Decodable>(_:as:)`, `write<T: Encodable>(_:_:)`, `remove(_:)`;
  `actor OrderedStatePersistence<S: Sendable>` with `edit`, `durable`, `flush`;
  `protocol CredentialStore` with Keychain and in-memory implementations.

- [ ] Tests: round trip; tampered ciphertext throws; wrong key throws; durable
  completes after write; edits coalesce (debounce) and flush writes the latest.
- [ ] Implement (AES-GCM combined format, atomic writes, backup exclusion). PASS. Commit.

### Task 4: Xcode project and walking skeleton

**Files:**
- Create: `mobile/ios/Zerus.xcodeproj/project.pbxproj`, shared scheme,
  `mobile/ios/Config/{Base,Debug,Release}.xcconfig`, `Local.xcconfig.example`,
  `mobile/ios/DEV_VERSION`, `mobile/ios/Zerus/Info.plist`, `PrivacyInfo.xcprivacy`,
  `Assets.xcassets` (app icon from the Swarm mark, accent mint),
  `Zerus/App/ZerusApp.swift`, `AppModel.swift`, `Theme.swift`, `BuildInfo.swift`,
  `Zerus/Machines/MachinesView.swift`, `PairingView.swift`, `Zerus/Settings/SettingsView.swift`.

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces: `@MainActor @Observable final class AppModel` with `connections`,
  `machines`, `storageReady`, `demo`, `error`, `pair(url:code:)`, `disconnect(_:)`,
  `refresh(explicit:)`, `preview()`, `stopPreview()`, `handle(url:)`.

- [ ] Build for the simulator:
  `xcodebuild -project mobile/ios/Zerus.xcodeproj -scheme Zerus -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build`
  Expected: `** BUILD SUCCEEDED **`.
- [ ] Commit.

### Task 5: Dev install scripts and device run

**Files:**
- Create: `mobile/ios/scripts/dev-install.sh`, `mobile/ios/scripts/remote-dev-install.sh`,
  `mobile/ios/README.md`; Modify: `.gitignore`.

- [ ] `bash -n` both scripts; run `mobile/ios/scripts/dev-install.sh` on the Mac;
  expected: installed and launched on the attached iPhone (devicectl output).
- [ ] Commit.

### Task 6: CI and documentation

**Files:**
- Modify: `.github/workflows/checks.yml` (job `ios`, gate entry), `README.md`,
  `docs/mobile-architecture.md`, `docs/ci-and-aur.md`, `scripts/ci/check-source.py`
  (iOS DEV_VERSION format check).

- [ ] Validate workflow locally (`python3 scripts/ci/check-source.py`; actionlint
  if available). Push branch, open PR, wait for `CI gate` SUCCESS. Commit.
