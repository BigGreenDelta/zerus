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
Case entry/completion breadcrumbs go only to the disposable emulator's `ZerusApprovalUI` log. After a failed approval
instrumentation attempt, the driver tries one five-second bounded read of the last 400 crash/main/system log records,
only while the owned emulator and ADB server remain live. This uses the original 1200-second end and 20-second cleanup
reserve. The read filters to fixture and Android-runtime tags. The interceptor atomically retains its first typed guard
failure with bounded method/encoded-path metadata only, emits that named diagnostic, and returns a fixed local failure
response without network forwarding. Every wait and final qualification refuses a retained interceptor failure.
Missing, late or failed diagnostics remain unknown and cannot replace the primary failure, qualify a missing
case, relax the strict result parser, or authorize a retry. The instrumentation cap remains 90 seconds.
The fixture's [native text search](https://developer.android.com/reference/android/view/accessibility/AccessibilityNodeInfo#findAccessibilityNodeInfosByText(java.lang.String))
returns approximate matches, so candidates must refresh and retain the exact visible text/content-description check.
The original subtree traversal remains the fallback. Monotonic phase records cover card opening, physical touch and the
first disabled/zero-answer assertion; they never override the three-second policy or establish UI acceptance alone.

The three early-window assertions first use the same real Home/resume route as the lifecycle case, with the exact owned
Activity required to lose and regain focus. Only these assertions use a scoped cached Send node captured for the verified
card: refresh, exact visible label, window and card/run/conversation identity must still match, without a fallback search.
The cache is cleared in `finally`. Monotonic time begins before resume and must remain below the original three seconds
after the early assertion or Enter; a slow fixture cannot qualify that precondition. The independent lifecycle case keeps
its original Home/resume boundary exactly once, with the same cache and elapsed check around its original
focus/question/confirmation-retired/count/disabled assertions; it never performs an additional reset. No production
visibility, foreground or review clock is set.

Ordinary Home/resume can also initiate the production catalog refresh. The interceptor admits only GET
`/v1/capabilities` and `/v1/computers` at the exact reserved HTTPS origin, default port, empty userinfo/query/fragment/body,
and exact existing fixture authorization. Capabilities advertise only protocol1, `inspect`, `answer`, and no features.
Computers returns one synthetic computer and a synchronized clone of the current synthetic session/card, never an empty
or stale catalog. Each fixed path has at most64 observations with first/last monotonic timestamps. The original POST
receipt/answer guards and first-failure refusal remain unchanged; no request is forwarded. The observed prior
capabilities refusal does not establish the cause of a missing broader dialog. The broader case independently verifies
its exact selected draft, option2, empty feedback, card/run/conversation and native scope before the original Send.
Failure-only diagnostics inspect at most4 windows,128 nodes/16 levels and8 fixed labels per owned app window, with
160-character records. Foreign trees are omitted, unknown/overflow/refresh failures are diagnostic UNKNOWN, and the
original exception stays primary. No extra ADB subject or diagnostic success can qualify a failed/missing case.

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

Both disposable targets import the public Apple `<notify.h>` declarations through the same fixed fixture-only bridging
header. Its bytes join the exact project source identities. The production application does not reference this header;
no handwritten C ABI, SDK-header replacement, or new runtime dependency is introduced.

The four fixture-only control modes use Darwin notifications through `notify_register_dispatch`, `notify_post`, and
`notify_cancel` ([Apple API header](https://raw.githubusercontent.com/apple-oss-distributions/Libnotify/main/notify.h),
[notification semantics](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/notify.3.html)).
The shared namespace does not authenticate a sender. This channel is confined to the disposable simulator fixture:
exact names bind a fresh test-launch UUID and a separate random app-generation UUID, with only the action needed by
the chosen mode registered. Each action consumes and cancels its own token once, and the UI must acknowledge its
action/serial. Posting success alone proves no delivery. Explicit fixture closure checks cancellation before termination;
a failed registration, acknowledgement, timing precondition, or cleanup cannot pass. No production target uses this API.

Countdown mode additionally starts unavailable, selects the original native option while ordinary card inputs remain
editable, then admits only its one-time `begin-countdown-review` action. That action sets fixture availability true;
the exact production eligibility path starts the unchanged review. No Home/activation latency or artificial clock is
part of this countdown fixture. Feedback and countdown use a fixed reverse notification bound to the launch UUID,
actual app generation, a separate fresh ACK UUID and a fixed action code. A main-actor UITest receiver registers before
post, consumes/cancels its exact token once, and acknowledges through an XCTest expectation. Its checked teardown block
runs even after a failing test. The app commits the original visible ACK before posting the reverse ACK; posting alone
is not proof. A monotonic timestamp before the original action post is a conservative lower bound for review start.
The original disabled/zero or real short edit/Return must finish below three seconds measured from that timestamp;
only afterward is the unchanged visible action/serial/time/generation ACK read. There is no pre-action accessibility
ACK poll. A late or missing callback, cancellation, visible ACK or timing guard fails without a bypass claim.

Feedback mode seeds only the fixed memory draft prefix and keeps the synthetic card unavailable while the real keyboard
opens. The one-time `begin-feedback-review` notification makes that card available through its ordinary production
eligibility path; a real short edit and Return must finish inside the original three-second window and leave zero
callbacks. After the real review, another Return must record exactly the original answer. A missed early window fails
the fixture without a bypass claim. The early Return can resign native keyboard focus. After its original timing/ACK/
zero-callback proof, the test checks the full retained text, taps that actual field once, checks unchanged text and zero
callbacks again, then runs the original final review/Return and exact payload assertions. It does not replace the text
or assume [`typeText`](https://developer.apple.com/documentation/xcuiautomation/xcuielement/typetext(_:)) focuses a field.
Replacement mode posts its one-time `replace` only after native Confirm is observed,
then requires the same native retirement and no-submit assertions. The broad disclosure check expects the entire
production message, including its description. Unavailable mode likewise observes the original native Confirm, full
disclosure and zero callbacks after the real review and Send, then posts its one-time `become-unavailable` event. It sets
only the fixture snapshot's availability false through the existing mode/generation/token guard. The original
four-second Confirm-retirement, zero-callback and disabled-Send assertions remain mandatory, together with the exact
action/serial/generation acknowledgement and checked closure. A physical timer trigger before Send allowed the first
Confirm lookup to occur after its two-second window; that failure did not reach the retirement oracle. No timer,
longer observation cap or retry substitutes for observing the original dialog before this event.
These controls never set a review clock, foreground flag, native dialog,
or provider permission. Monotonic phase records and exact ACKs are evidence only; all seven UI tests still have to pass.

Broader cancellation first uses an actual native Cancel when present. If absent, the test requires exactly one actual
native popover containing Confirm and the full production disclosure, a recognized actual horizontal size class, and
a positively identified passive fixture header in the same unique owned app window. The popover, header, window and
app canvas must have positive finite rectangles. Clip the actual header rectangle to that window and app canvas, then
derive the first positive region above, below, left or right of the actual popover. Full occlusion or an empty clip
fails. The centre of this unoccluded region supplies a strictly interior normalized offset relative to the actual
header; its dynamic [`XCUICoordinate`](https://developer.apple.com/documentation/xcuiautomation/xcuicoordinate) must resolve
to a finite screen point strictly inside that region, header, window and app canvas, and outside the popover, before
one physical tap. All original rectangles and the actual size class are logged before geometry admission, and each
geometry criterion records its result before an assertion can abort. A partially overlapping header rectangle alone
does not reject a proven visible outside point. The prior combined positive-size/nonintersection failure printed no
rectangles, so its exact failed conjunct remains unknown; this source change makes no native-success or cause claim.
Background `isHittable` is deliberately not an admission predicate for this outside-dismissal gesture: Apple documents
that [popover presentation](https://developer.apple.com/documentation/uikit/uipopoverpresentationcontroller/passthroughviews)
normally disables underlying view interaction. [SwiftUI confirmation dialogs](https://developer.apple.com/documentation/swiftui/view/confirmationdialog(_:ispresented:titlevisibility:presenting:actions:)-9ibgk)
use an outside tap instead of the standard dismiss action for regular-size popovers. This replaces a new fixture-only
guard that failed before the actual geometry/tap; original product and callback oracles are unchanged. Unknown role/geometry/traits
fails instead of guessing a screen point or altering presentation. Confirm retirement and zero callbacks are required
before the original separate Send/Confirm and final option2 payload. This path is source-only until actually qualified.

The configured XCTest default/maximum allowance stays30 seconds. Apple documents that
[`executionTimeAllowance`](https://developer.apple.com/documentation/xctest/xctestcase/executiontimeallowance) rounds to
minute granularity; a reported43.737-second case alone does not prove a violation of the configured value. The original
900-second driver end,60-second cleanup reserve and every command cap remain unchanged.

Retain the source map, raw stdout/stderr, result bundle and actual test inventory before cleanup. Re-hash every source
entry and the built fixture products. Shut down and delete only the newly returned simulator UUID after matching its
name/runtime to the recorded creation. An unknown shutdown, changed identity or surviving runner is a failed/unknown
gate, with evidence retained; never use `shutdown all`, broad process kills or numeric-PID fallbacks. This gate makes no
all-descendant, native-keychain, production-model, provider, operator-device or installed-delivery claim. Xcode's shared
SwiftPM cache behavior on the disposable VM is not a hermeticity claim.

The Android fixture distinguishes its interceptor observing an answer request from production applying that receipt.
After each of the four existing request-count waits, it requires the current exact target/card/run/conversation,
the recorded request ID and full answers in a submitted draft, no answer flight or pending consent, and an idle verified
detail read before advancing. This reads ordinary production state; it neither acknowledges nor fabricates a receipt.
The intentionally unsent old replacement card has no such receipt gate. Every broader Send also proves its saved option2
and empty-feedback draft before the existing real touch. Refreshed touch window/state/bounds/centre records are bounded
diagnostics, not proof that a touch activated a control. Failure window evidence and the existing screenshot capture run
before the Activity's back/finish cleanup; diagnostic or cleanup errors cannot replace the original throwable. The driver
does not retrieve a PNG or add any host operation. These fixture admission/diagnostic corrections do not establish the
cause of the earlier missing-modal failure or a product defect; all seven cases, original actions and budgets remain.
