# Zerus Android

Settings → About shows the Android app version and each connected computer's
reported `hgs` CLI version. Offline reports are marked `last known`; older CLIs
without version metadata show `version unavailable`, and an unreachable peer
without a report shows `version unavailable while offline`. The optional `hgs_version`
field travels with the existing computer snapshot and keeps API v1 compatible.

Native Kotlin and Jetpack Compose companion for the Zerus gateway protocol in
[the mobile architecture](../../docs/mobile-architecture.md). Android 8.0 or later
is supported. The app works without Google services.

Pair an HTTPS gateway with a short-lived invitation created by its administrator.
You can pair several gateways and browse logical projects and sessions across
their machines. Projects use workspace-scoped stable IDs; empty projects stay
visible, and uncatalogued sessions remain accessible by their folder or local
project label. An
optional `zerus://pair?server=...&code=...` link prefills the pairing dialog and
requires the user to review and confirm it. New connections suggest `https://relay.zerus.dev`; self-hosted HTTPS gateways
remain editable. No credential is compiled into the app. “Try demo” is an explicitly labelled, read-only preview.

The top-bar plus creates a new session. Workspace pairing lives in Machines;
empty Sessions and Projects views link there. Machine labels use the same badge
in session cards and the creation dialog. “Name on this phone” saves an encrypted
private label for the exact workspace and machine UUID. Reset restores the latest
reported name, including after an offline restart; it changes no hostname, SSH
configuration, project membership, session or delivery identity.
Machine label colors use the desktop's seven-color palette and tonal styling.
The encrypted phone-local override is scoped to the same workspace and machine
UUID and works offline. Automatic reset restores a UUID-derived color that stays
stable when the label is renamed.

Machines shows each reported gateway with its direct peer computers visibly
indented beneath it. A peer's “Through” caption uses the gateway's private phone
label when available. These computers belong to the same workspace connection;
each keeps its own name, color, status and controls, and session and project
filters still select individual computers. Each machine's action menu opens
Rename machine and Label color. Cached routes show “Last known” until
the catalog refreshes. A configured peer that its gateway cannot reach stays
beneath it as “Offline”, with “last seen” when the relay reports a time, under
the identity that gateway last verified. It keeps its label, color and filter;
New session lists it disabled, and Sessions shows no project-catalog warning for
it. A peer the gateway never verified is not shown. The tree describes the
selected one-hop route, not a permanent master computer.

The retired official relay origin is a narrow transport alias for
`https://relay.zerus.dev`. Existing encrypted connection URLs remain stored as
original identity; requests, receipts, push setup and displayed endpoints use the
canonical transport. Other origins, paths and nondefault ports do not migrate.
Drafts, encrypted history and pending request UUIDs keep their original keys.

Messages and answers carry the exact computer, native session, run, conversation,
and request identity. Questions also carry their native content hash. An
interrupted delivery stays saved as uncertain. Receipt checks read the original
request; they never send a replacement automatically. Archive browsing uses an
explicit immutable archive UUID and verifies the returned identity. Archived
conversations have no message, answer, or interrupt composer. Scoped restore,
rename, fork, and forget require fresh native capability and action evidence;
destructive actions require a confirmation naming their target.

Question cards can expand into a full-screen form while keeping the selected
options, text and current part. Sending, Answer submitted, Not sent and Delivery
unknown distinguish an active attempt from its result. A submitted answer stays
locked through a stale inspection or an application restart; checking delivery
reads the original receipt. Approval review time counts only while its disclosure
is visible in the focused foreground form.

The message composer clears after the text and immutable attachment snapshots
are atomically saved in the private outgoing queue. It stays editable while the
previous message is sending. Each bubble shows Sending, Sent, Not sent, or
Delivery unknown; unknown delivery blocks another send to the same conversation
until an explicit receipt check or review. Sent means the computer accepted the
input, not that the model read or answered it. New text is never cleared by an
older receipt. Native user delivery rows reconcile bubbles by exact request UUID.
Without such a row, reconciliation requires an unambiguous exact text/time match
after a successful native receipt.

A message waiting in the agent's native queue appears as one compact expandable
card. Expand it to read and select the full text. Send now remains a manual
action, available only when the current agent and queued message allow it.

Attach files through the system document picker. The app copies and encrypts
selected bytes privately rather than sending document URIs or trusting mutable
files. The supported gateway limits are eight files, 10 MiB per file, 20 MiB total,
and 64 KiB of message text. The picker remains bound to its original composition
if navigation or typing changes; interrupted selection can be canceled in Saved
drafts. Files remain recoverable with unsent and uncertain messages.

An empty conversation immediately shows “Loading conversation…” while its
messages load. Background loading status and spinners appear only after five
seconds; manual refresh shows progress immediately and reuses any matching
in-flight read. A thin progress bar accompanies the existing circular refresh
indicator in catalogs and conversations. Navigation cancels
ownership of earlier reads so stale results cannot replace a new conversation.
Matching history reads continue in a bounded shared queue and are reused on
reopening. Exact-target encrypted history is cached for seven days, up to 20
conversations and 20 MiB. Cached messages appear immediately when already
prepared in memory; cached history permits no action until a fresh inspection
verifies the conversation. History cache failures do not prevent live reads.

Agent messages and ordinary questions render Markdown with headings, emphasis,
lists, quotes, links, tables, strikethrough, static tasks and code blocks. Code and
wide tables scroll horizontally inside the message. Approval commands and native
answers stay literal. Only tapped HTTP(S) links open a browser; local file links
stay visible text, HTML stays literal and images do not download. Parsing and
render preparation use bounded background work and an exact-content memory cache;
over-limit or malformed content falls back to its complete original text.

Draft edits update the editor immediately and save through an ordered background
writer with a 200 ms debounce. Navigation and backgrounding request a flush.
Edits awaiting a successful write are still in memory and can be lost on an
abrupt process exit. Accepted outgoing and attachment transactions finish before
transport or file cleanup, even if the screen is disposed; failed outgoing persistence sends
nothing. Successful outgoing records remain durable but are omitted from Saved
drafts.

Tap an attachment in a conversation, draft or outgoing message to open it.
Files still held in this phone's encrypted store open without network access;
others are read from the owning computer with `attachment_read` in 512 KiB
ranges, with progress on the row, and must match the computer's SHA-256. Images
open in a full-screen preview (pinch or double-tap to zoom, Open with, Share);
other types open in another app through a FileProvider URI with a temporary
read grant, or the row explains that no app can open the type. Opened files stay
in the private `cacheDir/attachment-view/` cache for at most 24 hours and
100 MiB, least recently opened first, and are cleared on disconnect. A failed
read is never retried automatically; tap the row to try again. A relay or
computer without the operation shows "Update Zerus on this computer to open
attachments."

Gateway tokens, drafts, outgoing messages, attachments, and push endpoints are encrypted with an Android Keystore
AES-GCM key. Android backup is disabled. Drafts remain available after disconnect
or session disappearance. Disconnect revokes this phone's gateway credential
before removing it locally; an already-revoked credential can also be removed.
An unavailable gateway leaves the connection intact.

## Sessions and projects

Sessions is the initial view. Its compact filters use the desktop glyphs and
native semantics: All sessions, Needs attention, Active, Saved sessions, and
Archive. Active combines working sessions, sessions needing attention, and sessions
with local unsent text or attachment drafts, counting each session once. Drafts
(including visible subagent drafts) stay active offline and update the count as
they are edited, cleared or sent; archived sessions remain exclusive to Archive.
Drafts stay on their device; desktop and Android share the filter rules and IDs.
Work requires a reachable computer and a current running agent;
input, approval, non-recovering errors, and blocked recovery require attention.
Optional native questions and explicit child requests also count as attention.
Routine child failures and legacy hook-profile observations do not. Saved
sessions are paused or stopped; Archive contains immutable archived histories.

The computer filter supports several machines across workspaces. Counts and
cards use the same computer, project, and search scope. Results form collapsible
project groups; empty groups are hidden in Sessions. Filter, query, collapsed
groups, and list position survive opening a conversation and background polling.
Provider badges match the desktop identity motifs.

Session cards follow the desktop cards. A checkout line shows a small Git branch
mark and the branch, or detached HEAD; ordinary folders have none. As on the
desktop, the worktree name is not repeated on the card. A subagent counter uses
the desktop's compact active count, such as `3`, `2+` or `?`. Tapping it lists
the children that are working, waiting for input, paused, failed or unknown,
in stable ID order. Finished turns stay hidden, as with the desktop default,
unless the child has an unsent draft on this phone. Profile groups are counted
but not listed. Tapping a child opens its session and then the child's activity
once that conversation is verified; Back returns to the session. Open subagent
lists persist with the other Sessions state.

Long-pressing a session card, or using its accessibility long-press action,
opens its actions in the desktop context menu order: Open terminal, Mark as read
or Review later, Pause, Resume or Restore, Rename, Fork, Move to archive for
saved sessions, copying its name, reference or folder, and Terminate or Forget.
Unavailable items stay listed with their reason. Mark as read applies the Read
all evidence rules to that session and may start its bounded metadata probe; a
reminder without reply evidence can be cleared alone. Native items are gated
only by the catalog row: choosing one opens the session, where the existing
confirmation stays disabled until a fresh inspection verifies its exact
identity and eligibility.

Projects shows the canonical logical catalog, including empty projects, and its
saved folders grouped by machine. Overview cards show each folder's name and
selectable full wrapping path under one machine header, limited to two folders
on each of the first two machines with counts for remaining folders and machines.
Details show every folder using the same rows. Ordinary folders and local aliases
remain visible in Sessions.
Opening a project shows its folder details; View sessions opens a scoped Sessions
view. Back returns to project details and preserves the normal Sessions query
and filter. Archives are assigned by their UUID, so an archived and live session
with the same name can belong to different projects.

## Conversation history and native controls

The conversation keeps a bounded hot window rather than imposing a total chat
length limit. Nodes advertising canonical history support can fetch older and
newer pages and restore an opaque saved reading anchor. Native page order and
exact aliases distinguish repeated messages, even with identical timestamps.
Initial indexing keeps the recent inspected conversation visible; incomplete
sources and shortened message bodies are disclosed. Search covers the loaded
window. A confirmed own message stays visible when it falls out of the native
inspection tail.

Encrypted page storage retains up to 128 pages and 64 MiB for seven days, separate
from the recent-conversation cache. Cached retained pages can be browsed offline;
network reads require the corresponding native capability. Read positions and
markers survive page eviction. Automatic read progress acknowledges incoming replies only when they are
actually visible in the foreground with no modal covering the conversation.
Fresh complete canonical counters are exact; partial or stale evidence is shown
as a lower bound or unknown. First visits show the recent page without marking
it read merely because it opened. Mark read and Review later are local choices
and never answer a native question. Read all applies to the current conversation
or the Sessions scope on this phone, respecting the current project, machines
and search. It captures each conversation before changing local markers:
fresh complete heads mark the captured replies,
partial evidence marks only loaded replies, and unknown, unavailable or offline
rows without loaded evidence are skipped. Its result reports marked conversations, partial marks and skipped
rows. Reminders without usable reply evidence remain. Read all is disabled when
its scope has nothing it would clear: no reminder and no reply counted unread
that its current evidence covers. New replies arriving
after the captured evidence remain unread; reading anchors and drafts stay put.
When the computer advertises `mark_read`, reaching the newest reply (a fresh
complete head read through on this phone), Mark read and Read all also send the
exact run, conversation, reply and attention signature to the session's
computer, which shares them with every desktop and phone. A reply or request read
elsewhere is shown as read here. Review later stays on this phone.
Bounded background metadata probes can prepare canonical heads for live sessions
with reported reply evidence, without opening their conversations or fetching
full history. Previously unvisited sessions with unknown evidence are not
silently marked read.

Context details show native size, limits, and cache evidence without inferring
warmth from cache hits. Compact and continue waits for the exact native request
to complete, then sends only its unchanged captured draft; editing, navigation,
uncertainty, or cancellation prevents the send. Clear requires a named
confirmation and preserves unsent text and files. Confirmed rename and resume
can move an ordinary editing draft only to their verified same-conversation
result. Saved drafts offers explicit recovery if that destination was unavailable.

Native controls are enabled only when both gateway and computer advertise them
and a fresh inspection permits the action. These include session lifecycle,
queue Send now, model and effort changes, process output and Stop requested,
scoped recovery controls, and exact child history. Scheduled model settings have
one durable authorized apply attempt when native readiness is observed, including
when another session is open. Uncertain actions expose their original receipt;
they are never retried automatically.

Terminal uses a separate saved input buffer and a fresh exact native binding.
Snapshots poll adaptively while visible; input sends literal text or fixed keys,
with confirmation for Ctrl+C and Ctrl+D. Submitted means native input handoff,
not command completion. New session uses the selected computer's native provider,
account catalog and an explicitly chosen folder. Ordinary folders remain usable
without creating a Git repository. A verified repository defaults to a generated
worktree, created only after Create session; None and existing or manually
created worktrees remain available. A launch is confirmed only by its own native
request identity.

## Voice input

Settings includes **Voice input**, with an independently selected computer and
host-configured recognition profile. Both the gateway and selected computer must
advertise `speech_profiles`, `speech_submit`, `speech_status` and `speech_cancel`;
older installations remain usable and show why voice is unavailable. Profile
discovery never loads a model. When supported, `speech_stats` displays the selected
computer's aggregate local voice counts; audio and transcription are excluded from
Firebase events. See [mobile architecture](../../docs/mobile-architecture.md#voice-input)
for the transport contract and [analytics](../../docs/analytics.md) for counter
definitions and the broader local/fleet design.

The composer microphone requests Android microphone permission and records at most
two minutes. Stop sends 16 kHz mono signed little-endian PCM through the gateway to
the chosen computer. Capture and bounded 48 kHz fallback conversion run off the
main thread. Leaving the app stops capture and retains it in memory for explicit
Transcribe or Discard. Audio is not saved on the phone; process death during capture
loses that recording. The gateway can read relayed audio; this transport has no E2EE.

Denied microphone access opens a focused explanation with **Open app settings**.
This opens Zerus's Android settings; choose **Permissions → Microphone** and allow
access while using the app. Return and tap the microphone again to record.
Returning from system settings never starts recording automatically. If the
settings page is unavailable, the explanation retains the manual navigation path.

Before upload, the phone saves the exact request UUID, speech host/profile and
original draft identity in encrypted storage. Interrupted submissions use status
queries for that original job and never upload again automatically. Completed text
is encrypted before insertion. An unchanged draft on the same navigation receives
the text as an editable append; changes, navigation and app restarts require review
with **Insert into draft** or **Copy**. Insertion retains attachments and saves its
deduplication identity atomically with the draft. Recognition never sends a message.
Settings retains recovery controls even when the original conversation disappears.

## Build and install

Install JDK 17 or 21 and Android SDK platform 36 / build tools 35.0.0. Set
`JAVA_HOME` and `ANDROID_HOME` for those tools, then run:

```sh
cd mobile/android
./gradlew assembleDebug testDebugUnitTest lintDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The isolated microphone smoke uses a synthetic in-process relay while exercising
real AudioRecord and encrypted Android storage. On a disposable emulator only:

```sh
./gradlew -PzerusInstrumentationRunner=app.zerus.mobile.SpeechSmokeInstrumentation assembleDebug assembleDebugAndroidTest
adb -s emulator-5580 install -r app/build/outputs/apk/debug/app-debug.apk
adb -s emulator-5580 install -r app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
adb -s emulator-5580 shell pm clear app.zerus.mobile
adb -s emulator-5580 shell am instrument -w app.zerus.mobile.test/app.zerus.mobile.SpeechSmokeInstrumentation
```

The clear command removes app data on that emulator. The smoke covers a completed
submit acknowledgement without text, delayed recognition while editing, unmirrored
IME edits at insertion and during persistence, explicit recovery insertion,
background capture stop, cancellation and encrypted draft persistence. It also
denies microphone access twice, opens Android's app permission settings, grants
access there and verifies recording requires a new explicit tap after returning.
It does not measure recognition accuracy or replace a physical microphone test.

Private previews may override only their build identity using
`-PzerusVersionName=0.1.31-voice.236 -PzerusVersionCode=10236`. Choose a code greater
than the installed build; the next regular update must exceed the preview code.
These properties do not change the signing key or checked-in release version.

The checked-in Gradle wrapper uses Gradle 8.13 with an official distribution
checksum. Dependencies come from Google Maven and Maven Central. The debug APK
uses the local Android debug signing key; public distribution requires an
owner-controlled release key. Generated APKs and local configuration are ignored.

For distribution and device performance validation, run
`./gradlew assembleRelease lintRelease`. It produces
`app/build/outputs/apk/release/app-release.apk` with R8 and resource shrinking,
signed with the same development key, so published APKs upgrade one another.
Release builds require a private signing Properties file selected by
`-PzerusSigningProperties`, `ZERUS_ANDROID_SIGNING_PROPERTIES`, or the default
`~/.config/hgs/mobile/dev-signing/signing.properties`. Its fields are `storeFile`,
`storePassword`, `keyAlias`, and `keyPassword`; relative key paths resolve beside
that file. Keep the file, key and backup outside the repository. Existing dev
installations must keep their exact signing key. Ordinary debug builds use the
local test key independently. Release builds are not store production releases.

## Updates

The main toolbar places New session, Refresh and Settings in that order.
Settings opens a full screen with Privacy, About Zerus and Updates, including
before pairing. About shows the installed version and build beside its heading,
wrapping onto the next line when needed, and a GitHub-marked `ufna/zerus` link.
Offline bundled Third-party notices include selectable license text.
Back returns to the same app content; notification and push controls stay in Machines.
Anonymous checks use `https://zerus.dev/updates/v1/android/dev.json` at startup
and through WorkManager every six hours when a network is available. Android
may defer background work. An available update or pending installation badges
the Settings button. The indicator remains available without notification
permission. Check, Download and Install are separate actions; no update silently
repoints or removes a paired workspace. The suggested relay for new pairing is
`https://relay.zerus.dev`, and custom HTTPS relays remain editable.

Downloads are bounded, cancellable and verified by exact size and SHA-256. Before
installation, the APK must match the advertised package, version, Android minimum
and the current installed signer. Saved verified downloads work offline.
Installation saves private drafts first and waits for active deliveries or
commands. Android's install-source permission and confirmation remain required;
the updater explicitly requests user action through
[PackageInstaller](https://developer.android.com/reference/android/content/pm/PackageInstaller.SessionParams#setRequireUserAction(int)).
An installer session is recorded before commit and never recommitted after
interruption. A lost confirmation screen requires explicit cancellation before
another attempt. Success is shown only after the installed package version is
observed. Update metadata, APK files and recovery state are app-private and
excluded from Android backup.

## App icons

Launcher icons reuse the selected Swarm blades from
[the canonical symbolic SVG](../../tray/resources/icons/hgs-zerus-symbolic.svg),
with the original curves and three rotations. The adaptive foreground is mint
`#67E8CB` on a full-bleed ink `#101517` background. Both layers use 108 dp; the
mark is centered at (54, 54), has an outer radius below 33 dp and bounds about
61 × 59 dp. Android supplies the launcher mask and shadow. Android 13 and later
receive the same geometry as a white monochrome layer for themed icons. The
round icon uses the same adaptive resource.

Notification icons use transparent white Swarm geometry in a 24 dp viewport,
with a roughly 20 dp outer diameter. Android 12 and later use the adaptive mark
on an ink splash background. The existing in-app brand image is unchanged.
Sizing and layer choices follow Android's
[adaptive icon design guidance](https://developer.android.com/develop/ui/compose/system/icon_design_adaptive)
and [launcher asset documentation](https://developer.android.com/studio/write/create-app-icons).

## Notifications

In Machines, Session alerts has independent settings for input and approvals,
errors, and turn completion. Input and errors are on by default; completed turns
are off. The master switch disables all session alerts. Disabling a type also
removes its existing cards.

There is one updated card per workspace, computer and live session slot. The
normal shade shows the authenticated session title and the computer's private
name on this phone. The lock-screen public version is generic. No question text
or conversation content is included. Repeated wakes do not reannounce the same
unresolved question; historical backlog is silent and old completed turns are
suppressed. A batch can sound once, with a cooldown between new audible alerts.
When many sessions need attention, a bounded set of individual cards is kept
and one summary represents the remaining sessions. The summary opens Zerus;
individual cards retain their exact workspace, computer and session destination.
Cards refresh with events, settings changes and app startup. A resolved input
request can remain until the next refresh when the relay emits no new event.

When the relay advertises `presence_alerts`, a new card needs a relay `alert`
event for the same workspace, computer and session: an attention or error alert
for input, approvals or errors (the relay and the phone rank a session that
both asks and fails differently), or completed for an opted-in finished turn.
The relay sends it only while the item is unread and unresolved, and only when
the owner is away from every Zerus desktop or chose immediate delivery. A card
already shown stays without a new alert and follows detail changes of the same
run silently, such as a failed recovery changing state. When an ordinary event
announces a new question or error without an alert, the card is withdrawn until
that alert arrives. Relays without the feature keep the behavior above, so this
build ships before the relay.

“Keep a live connection” starts an explicit foreground remote messaging service
with a visible Stop action. It listens to the gateway event stream and follows
additional paired gateways. It is the fallback for builds without Firebase and
phones without Google Play services. Android battery saving or network loss can
delay this connection; it is not a Doze wake-up guarantee.

Firebase builds register FCM automatically; there is no setup button. Machines
shows each workspace's push status, including when Firebase is unavailable on
the phone or in the build. UnifiedPush was removed on 2026-10-10.
Push content acts only as a wake-up hint and schedules an authenticated event
fetch without a separate generic alert. Bounded pages are coalesced and checked
against a current catalog; a failed catalog read retains pending events for
retry. Tapping a card refreshes and opens the current live session in that exact
workspace and computer, without archive fallback. The event schema does not
identify historical runs, so optional completion alerts conservatively filter
recent events against a previously observed matching current conversation.

Firebase remains an optional build for Google-free installations; published APKs include Crashlytics, Analytics and FCM. Create an Android
Firebase project for `app.zerus.mobile`, place its private local configuration in
`app/google-services.json`, and run:

```sh
./gradlew -PzerusFirebase=true assembleDebug testDebugUnitTest lintDebug
```

The gateway must advertise and configure FCM separately. The app checks that
capability before registering its token. FCM initializes independently of telemetry,
registers automatically for eligible paired gateways, and retries each gateway
separately after network loss. Token changes re-register existing connections;
new connections register the current token. Only data messages should be sent by the
gateway, allowing the app to show a private alert and fetch authenticated events.
No Firebase project credentials are included in this repository.

The full-screen Settings Privacy section exposes Usage analytics and Crash reports. Both
start enabled in Firebase builds and persist separately on this phone. Analytics
stops through the SDK when disabled. OFF is authoritative at cold startup:
the app does not instantiate Analytics while the saved preference is off, and
eager Analytics connector registration is removed. The measurement receiver,
service and job service follow the saved switch before Firebase initialization;
their manifest declarations remain enabled for SDK validation, while settings
disable their runtime components durably before saving OFF. This does not rely on the asynchronous
SDK flag reaching disk before process termination. Crash reporting changes require an app
restart. SDK automatic uploads stay disabled; the app's default-on crash setting
uses documented manual send/delete APIs after startup checks. Normal ON-period
reports upload on the next launch. Opting out persists a discard boundary and
requests deletion. Re-enabling preserves that boundary until a later startup
confirms no pending reports remain; diagnostics during this transition may be
discarded. A boundary persistence failure never grants upload permission. FCM and session notification preferences are independent.
Firebase receives standard app/device metadata and SDK installation identifiers.
Custom events are only app_open and screen_open with a fixed screen name;
nonfatal diagnostics accept fixed failure categories. Custom reports and events
exclude conversation content, identifiers, names, paths, URLs, accounts and
credentials. Automatic crash reports may include thread names and technical network metadata,
including gateway addresses. Fatal exceptions pass a message/cause-free bounded stack to the
Crashlytics handler. Pairing URIs and campaign/referrer extras are removed before
activity lifecycle callbacks; automatic screen and advertising ID collection
are disabled and advertising consent is denied. These controls do not provide
an anonymous SDK installation.

Minified release builds use the Crashlytics Gradle plugin for exact R8 mapping
upload. Retain a private copy for every shipped APK by passing `--mapping` and
`--diagnostics-output` to `scripts/prepare-mobile-release.py`. Diagnostics stay
outside the public candidate directory; the sealer verifies the mapping ID
against the APK's embedded R8 marker and records APK/mapping hashes and signing
identity in private metadata. Do not publish mapping files or configuration.

## Local validation

Unit tests cover exact destination identity, interrupted-delivery recovery,
HTTPS validation, one mutation attempt on a dropped connection, read-only receipt
polling, bounded responses, native user/assistant message merging, attention phases,
archive filtering, and verified composer readiness. Additional tests cover stable
project scoping and empty groups, delayed progress and stale operation ownership,
atomic composer/outgoing transitions, legacy draft migration, first-conversation
creation, ambiguous event reconciliation, immutable encrypted file copies,
tamper/cancellation handling, attachment presentation, attachment read identities,
range verification and the bounded view cache, and large bounded streamed
JSON envelopes. Android lint checks the app.
Device/emulator checks are still needed for push delivery,
Keystore persistence, notification permissions, and native conversation controls.

Pairing QR codes use CameraX 1.6.2 and ZXing core 3.5.4 without Google Play services. In Machines, open Pair a workspace and choose Scan QR to request camera access. Scanning only prefills the editable HTTPS gateway and invitation code; Pair remains an explicit action. The shared bounded parser also validates pairing deep links. Camera images and invitation codes are not saved or logged; leaving the scanner stops its camera. Manual entry remains available when access is denied or a camera is unavailable.

CameraX is Apache 2.0 and includes libyuv under BSD-3-Clause; ZXing is Apache 2.0. Full license texts and source attribution are available in Third-party notices. Pins follow the [official CameraX releases](https://developer.android.com/jetpack/androidx/releases/camera) and [ZXing 3.5.4 release](https://github.com/zxing/zxing/releases/tag/zxing-3.5.4). CameraX requires compile SDK 36 and AGP 8.9.1 or newer, covered by this build.

Accounts is a read-only viewer grouped by workspace and provider-reported account identity, matching desktop accounts. Each card contains its machine labels, using the same names and colors as Sessions. Provider account IDs and organization scope own identity; email-only reports join an ID only when unambiguous. Signed-out and unidentified profiles stay machine-local. Compact cards show account type and aligned period, remaining reset time and usage columns, with recurring-period and hourglass glyphs; tap a card for the full identity, limits, per-machine profiles and update times. Usage selects one best-quality report and never adds quotas across machines. It shows reported account identity, plan, usage windows and wallet balances when the connector provides them. Unknown limits stay unknown; ended windows need a provider refresh. Account snapshots use a separate encrypted private cache, so offline values remain available with their original provider update time and stale styling; offline and authentication problems stay visible. A connector advertising `accounts_snapshot` is required; Refresh reads its latest snapshot and does not force a provider refresh.

## Project and account selection

New session is a full-screen form with Project, Folder, Worktree, Agent and
account, and Session name sections. Tapping outside never closes it; Close or
Back asks before discarding edits and is unavailable while a worktree or session
is being created. A disabled **Create session** button shows one reason.
A `work-` name with eight hexadecimal digits is generated once per form; it is
the editable session name and Auto's branch.
Projects use the selected computer's fresh canonical swarm/project IDs, and
only its local folders are offered. The project picker searches names, folder
names and paths in catalog order and opens at the current project. A project
without a folder on that computer says so and offers Browse. The native browser
verifies a chosen folder again before creation. A folder explicitly chosen
through Browse remains selected when changing projects in the same form; a
listed project folder, chosen worktree or created worktree gives way to the next
project's folder. Choosing an outside folder explicitly adds it to the selected
project, as the form states.

Worktree mirrors desktop. **Auto** is the default when a fresh, complete catalog
shows the selected folder inside an available checkout with a named root. The
form shows the sibling `<root>-<branch>` path and new branch before launch.
Create session then locks the form, creates that worktree from `HEAD` with a
durable receipt, checks its folder and catalog, and launches there. A worktree
of a project folder is placed as a related worktree; otherwise the form states
that the new folder will be added. Opening the form and checking folders never
create anything. **None** starts in the selected folder; ordinary non-Git folders, catalog errors
and older computers use it, and never initialize Git. The options menu offers
**Choose existing…** and **Create manually…** (branch, base and destination);
both select that checkout for a direct launch. Auto runs at most once per form.
A failed or unknown creation is never repeated and leaves its destination
available for inspection or an explicit direct-folder launch; Check delivery
reads the original receipt. Closing the form keeps a created folder and branch.
A related worktree is recognized only when a current native catalog, or the
form's confirmed creation, contains both the selected checkout and a saved
folder of the chosen project; it is not added as another project folder. The
connector checks this again with a fresh native catalog before launching.

Mobile worktree creation has a 25-second native deadline and a 15-second checkout
budget. A lost SSH connection or cancelled connector attempt drains its own
bounded operation, preserving partial work and unrelated processes. Desktop
worktree creation keeps its existing timeout.

Accounts remain distinct native profiles. The actual reported default is
preselected, and an offered concrete account is always sent by its exact ID.
Identical labels include profile IDs; known cached sign-in status is shown
without treating credential-file presence as proof of sign-in. No synthetic
“Use native default” choice is added to a known catalog.

Project assignment requires the connector's `launch_project` feature and the
native `hgs swarm assign-launch --json` ABI. Older computers can still create
sessions, with an explicit project-assignment-unavailable notice. Creation is
confirmed by its launch UUID before the native helper checks the exact run,
session and current canonical project. Assignment is a separate metadata outcome;
it never retries session creation. A created session whose assignment failed or
is unknown remains a completed, nonblocking action, with a durable Drafts notice
offering Open created session and Mark reviewed.

The native helper records a private write-ahead intent in
`HGS_CONFIG_DIR/swarm/launch-assignments.json` before saving project metadata.
An interrupted intent is permanently uncertain and never reapplied. Recorded
outcomes survive later manual project moves, and the shared catalog keeps its
existing schema for older desktop binaries. The sidecar is bounded to 10,000
permanent receipts; at capacity new assignments fail before changing metadata.
Receipts never enter swarm exports or peer synchronization.
