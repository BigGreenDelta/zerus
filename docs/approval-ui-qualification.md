# Native approval UI qualification

This gate covers the native frontends of the existing approval-choice implementation. It does not grant a native tool
permission, use an operator's device, install a maintained application, or qualify provider delivery. The Android runner
uses the real Compose card and `ZerusViewModel`, durable private drafts, and intercepted reserved-origin receipts. The
separate iOS fixture links the exact production `QuestionCard.swift`, `Theme.swift`, `MarkdownView.swift`, and `ZerusKit`
policies to a memory-only callback sink. It excludes the production entry point, `AppModel`, `ConversationModel`, relay
client, keychain, notifications, and storage. The desktop QTest uses the real `QuestionCard` and private draft directory.

All three native gates are required. SDK-free harness controls are admission evidence only. A build, import, setup,
timeout, cleanup, missing case, or skipped case cannot substitute for a UI assertion or count as a behavioral RED.

## Android: seven ordered cases

Use `.github/workflows/android-approval-ui.yml` on the exact independently reviewed commit. It preserves the hosted
image, Java, emulator, API35 image, foreground owned ADB server, private profile, source/tool/APK seal, actual app/test
certificate equality, AXML, defined-DEX-class, and cleanup checks from the project-picker gate. It selects the new finite
runner using `scripts/ci/android-project-ui.py --approval-choices`; arbitrary runner names are refused. The original
project-picker invocation remains the default and retains its six-case result schema.

The runner executes exactly: `countdown_once`, `broader_cancel`, `broader_confirm`, `card_replacement`,
`feedback_keyboard`, `lifecycle_no_submit`, and `readonly_stale_identity`. It requires actual physical taps, the unchanged
three-second review, a separate native confirmation, exact option/text payloads and request/run/conversation/hash
binding, four unique synthetic answer receipts, zero unexpected operations, and preservation of the unrelated composer.
Lifecycle uses the actual Activity and window, rather than an injected foreground Boolean. The stale-run case changes
the intercepted inspection before the UI send; no answer is admitted in that case. The fixture cannot contact its
reserved relay: every permitted request is completed inside the interceptor, while unknown operations throw.

Before dispatch, review this workflow and the SDK-free `tests/test_android_project_ui.py` and
`tests/test_approval_ui_harness.py` controls. The new app/test are built under a fresh synthetic debug signer; this does
not establish compatibility with the operator's installed signer. Do not reuse an old private D01/ISO resource grant,
APK, AVD, profile, key, lease, 22-subject producer, or result.

## Desktop: real review, feedback and second click

On an isolated hosted Qt checkout, run the normal `test_questioncard` suite, including
`kimiNativeChoicesReviewFeedbackAndReplacementGuards`, `claudeApprovalListsNativeOptionsAndConfirmsBroaderOnes`,
`reviewCountdownAndNextNavigation`, and `nativeCountdownPreservesScrollChoiceAndReview`. The new test does not shorten
the production review delay. It checks hide/show resets the review, first-click zero callbacks, explicit second-click
payload, changing choices cancels confirmation, a new hash cannot consume old confirmation, early Enter cannot send,
feedback has no selected native option, and a read-only replacement stays locked. This does not run a native provider.

## iOS: separate unsigned simulator fixture

Use only a fresh standard GitHub-hosted macOS VM. Root must select and record the exact Xcode build, installed simulator
runtime identifier and device-type identifier before a separate execution grant. The example variables below are
required reviewed inputs, not fallback values. Do not install a new runtime, select a physical device, use a distribution
environment, import a signing identity, or access Keychain. No product Xcode target or scheme is changed.

Create a fresh private work directory, then generate the project once:

```sh
test "$RUNNER_ENVIRONMENT" = github-hosted
test "$(git rev-parse HEAD)" = "$EXPECTED_SOURCE"
test -z "$(git status --porcelain)"
umask 077
gate=$(mktemp -d "$RUNNER_TEMP/approval-ios.XXXXXXXX")
python3 -I -B scripts/ci/ios-approval-ui-project.py "$gate/project"
```

The generator refuses an existing output. It references production source files directly, records their hashes and the
local package sources, and creates only an iOS-simulator application and one UI-test target. Its package is entirely
local. Record `xcodebuild -version`, `xcrun simctl list runtimes --json`, and `xcrun simctl list devicetypes --json`, and
refuse unless the separately reviewed exact build/runtime/device inputs are available. No search or latest fallback is
admitted. Create exactly one new simulator and retain its returned UUID as the sole lifecycle target:

```sh
simulator=$(xcrun simctl create "Zerus approval $GITHUB_RUN_ID" "$EXPECTED_DEVICE_TYPE" "$EXPECTED_RUNTIME")
xcrun simctl boot "$simulator"
xcrun simctl bootstatus "$simulator" -b
xcodebuild -project "$gate/project/ApprovalUIFixture.xcodeproj" -scheme ApprovalUIFixture \
  -configuration Debug -sdk iphonesimulator -destination "platform=iOS Simulator,arch=arm64,id=$simulator" \
  -derivedDataPath "$gate/derived" -clonedSourcePackagesDirPath "$gate/packages" \
  -packageCachePath "$gate/package-cache" -disableAutomaticPackageResolution -disablePackageRepositoryCache \
  -parallel-testing-enabled NO -maximum-concurrent-test-simulator-destinations 1 -jobs 2 \
  -test-timeouts-enabled YES -default-test-execution-time-allowance 30 \
  -maximum-test-execution-time-allowance 30 -resultBundlePath "$gate/result.xcresult" \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= test
```

The executable gate is `scripts/ci/ios-approval-ui.py` through `ios-approval-ui.yml`. A PR touching this gate or its exact
production source inputs selects the PR head with the reviewed fixed Xcode 26.3/build 17C529, iOS 26.2 and iPhone 16.
Manual dispatch retains explicit required inputs. Both paths refuse absent or mismatched installed selections without
fallback, use read-only credentials and no Actions caches, and run only on a disposable hosted VM. Review the workflow before
publishing its PR; the shell lines above explain its commands and are not an alternative bypass. The driver requires the
exact selected `DEVELOPER_DIR` and `EXPECTED_XCODE_BUILD`, not the existing product selector's newest-Xcode fallback.
It refuses a non-arm64 hosted process before allocation, selects the arm64 simulator destination explicitly, and builds
the separate Debug fixture with `ONLY_ACTIVE_ARCH=YES`, matching the production Debug configuration and local package.
Apple tools retain the VM's validated standard `HOME`, `TMPDIR`, `USER`, and `LOGNAME`; those exact environment values
are recorded with command timings. Evidence, DerivedData, package outputs and XDG paths remain private. The standard
hosted Apple account and its caches are in scope; an artificial empty home is not an isolation guarantee. Earlier
CoreSimulator inventory timeouts remain setup/unknown outcomes, with no proven cause attributed to the home directory.
The first cold `simctl list runtimes` has a 120-second cap without retry; later inventory and cleanup caps stay unchanged.
The driver imposes one original 900-second budget, reserves 60 seconds for cleanup, bounds boot to 120 seconds and build/test to
600 seconds or the remaining shared budget, caps raw output at 32 MiB per command, owns the unreaped direct child before
later fallible calls, and retains failures without retry. A forced child stop is failed/unknown, never passing cleanup
evidence. The exact native assertions are the seven
methods of `ApprovalChoicesUITests`: countdown/once, broader cancel/confirm, card replacement, unavailable replacement,
feedback keyboard, actual background/foreground, and hidden/read-only cards. Require seven executed tests, zero errors,
failures or skips; inspect the raw XCTest results and callback assertions, rather than accepting exit zero alone.
Keyboard and dialog behavior must be observed, not inferred from the existing SwiftPM policy tests.

Retain the source map, raw stdout/stderr, result bundle and actual test inventory before cleanup. Re-hash every source
entry and the built fixture products. Shut down and delete only the newly returned simulator UUID after matching its
name/runtime to the recorded creation. An unknown shutdown, changed identity or surviving runner is a failed/unknown
gate, with evidence retained; never use `shutdown all`, broad process kills or numeric-PID fallbacks. This gate makes no
all-descendant, native-keychain, production-model, provider, operator-device or installed-delivery claim. Xcode's shared
SwiftPM cache behavior on the disposable VM is not a hermeticity claim.
