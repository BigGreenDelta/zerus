# Zerus Android

Independent-session waits declared through `hgs wait` appear as **Waiting for
acceptance** or **Waiting for result**, with producer, computer and elapsed time.
They remain in Active while the exact waiter is live and online; work and
attention retain priority, and intermediate reply badges wait for settlement.
The additive snapshot metadata uses the shared desktop filter contract. See
[Agent coordination](../../docs/agent-coordination.md).

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

An inline question card fits its content, up to three quarters of the
conversation area. Its answer field and actions are laid out first and stay
whole; a longer prompt and its options scroll inside the card. The history
ends one message gap above the context footer.
Question cards can expand into a full-screen form while keeping the selected
options, text and current part. Sending, Answer submitted, Not sent and Delivery
unknown distinguish an active attempt from its result. A submitted answer stays
locked through a stale inspection or an application restart; checking delivery
reads the original receipt. Approval review time counts only while its disclosure
is visible in the focused foreground form.
OpenCode form items follow the shared `opencode-form-contract.json`: a plain text
item is an **Answer** field limited to the form's `max_length`, with a character
counter; `default_option_ids` start selected, and length and `min_items`/`max_items`
bounds are checked before Send; an item that is not `required` may stay empty.
A card with `can_answer: false` is read-only and shows its `unavailable_reason`,
such as **Answer this form in OpenCode**, and no Send, also when the computer
omitted its items. Form text may start with `/` or `!`: the computer sends it
through the OpenCode form reply, never to a terminal. Other cards, approvals
included, still ask for such answers on the computer.
An OpenCode permission card offers **Reject**, which rejects every pending
request in that session, and **Allow once** after the approval review. When the
request offers it, **Always allow…** follows the same review and then asks for a
separate confirmation, which shows what it saves when the computer describes it
(for example `Allows without asking again: printf *`). The confirmation sends
nothing and closes if the request changed, can no longer be answered, or the app
left the foreground meanwhile; opening it again needs a fresh review.
An approval marked `approval_choices`, such as a Claude terminal panel or a
Kimi tool approval, lists the agent's own options verbatim, in its order, and
sends the chosen one with **Send** once the approval review finishes, a
rejection included, as on the desktop. An option with `scope: "broader"`, such
as Kimi's **Approve for this session**, says that it applies beyond the request
and then asks for a separate confirmation that shows the agent's description of
it; the confirmation closes without sending if the request or the chosen option
changed, the request can no longer be answered, or the app left the foreground.
When the card offers an Other answer, such as Kimi's **Reject with feedback**,
its one-line text is sent instead of an option. A card the computer cannot
answer shows its options read-only with its reason. Other approvals without
Allow and Deny remain answerable only on the computer.

Conversation messages use 16 sp text and a 20 sp baseline; the editor uses 18 sp
text and a 22 sp baseline. Both keep natural character spacing. Compact Markdown
spacing, bubble padding and a single attachment control leave more room for text.
The message field grows from one to six lines; **Expand message**
at the top-right opens a taller scrollable editor without changing its draft or
selection. Attachment and send/microphone controls stay at the bottom with 48 dp
touch targets. The send/microphone circle is 40 dp, centered inside its 48 dp
target with a 4 dp inset from the field. System font scaling remains available,
and long text scrolls within the field rather than covering its actions.

The message composer clears after the text and immutable attachment snapshots
are atomically saved in the private outgoing queue. It stays editable while the
previous message is sending. The field keeps its **Message** placeholder; routine
sending feedback belongs to the outgoing bubble rather than repeated composer
notices. The send action explains a blocked submission on request; unknown
delivery and saved-draft recovery remain visible. Each bubble shows Sending, Sent, Not sent, or
Delivery unknown; unknown delivery blocks another send to the same conversation
until its exact receipt or native delivery row confirms it, or an explicit review.
Opening a verified conversation checks an uncertain message's saved receipt once;
an unavailable receipt leaves it saved and never resends it. The composer identifies
that original message with **Check delivery** and **Review**, leaving the next
message's input hint clear. Sent means the computer accepted the
input, not that the model read or answered it. New text is never cleared by an
older receipt. Native user delivery rows reconcile bubbles by exact request UUID.
Without such a row, reconciliation requires an unambiguous exact text/time match
after a successful native receipt.

A message waiting in the agent's native queue appears as one compact expandable
card. Expand it to read and select the full text. Send now remains a manual
action, available only when the current agent and queued message allow it.
An OpenCode message in the session inbox is a user bubble labelled **Queued**;
after Stop it is labelled **Not delivered**. OpenCode dates a queued message by
its delivery, so a phone message matches the OpenCode row that names its exact
request UUID, queued, delivered or in paged history; a row naming another request never matches,
and rows that name none keep the text/time rule. Inbox rows are not history: with
paged history they follow the newest loaded message while the inspection reports
them and never become a page, cursor or unread reply. Phones do not remove
undelivered messages.

Attach files through the system document picker. The app copies and encrypts
selected bytes privately before adding them to the original draft, rather than
sending document URIs or trusting mutable files. A restored picker
callback waits for private drafts to load and stays bound to its exact launch
identity. A fresh app launch clears an interrupted selection without discarding
the draft or its existing files. Pending selection can also be canceled from
Updates, Saved drafts, or any open conversation; a late canceled result cannot
attach to a newer selection. Active imports continue to block installation.
The supported gateway limits are eight files, 10 MiB per file, 20 MiB total,
and 64 KiB of message text. The picker remains bound to its original composition
if navigation or typing changes. Files remain recoverable with unsent and
uncertain messages.

An unopened conversation immediately shows “Loading conversation…” while its
messages load. A resolved empty conversation keeps “No messages yet.” during
background reads instead of returning to the initial placeholder. Opening cached
messages immediately animates the existing **Refresh** action until the initial
inspection and history reads settle. Cached messages remain readable, and the
indicator never inserts a row or moves the transcript. Later background reads
show progress only after five seconds; manual refresh shows it immediately and
reuses any matching in-flight read. Catalogs retain their thin progress bar.
Pulling a conversation up past its newest
message, as far as a list's downward pull, is the toolbar's explicit refresh; its
indicator rises from the bottom. Short pulls and flings do not refresh. Navigation cancels
ownership of earlier reads so stale results cannot replace a new conversation.
Matching history reads continue in a bounded shared queue and are reused on
reopening. Exact-target encrypted history is cached for seven days, up to 20
conversations and 20 MiB. Cached messages appear immediately when already
prepared in memory; cached history permits no action until a fresh inspection
verifies the conversation. History cache failures do not prevent live reads.
Catalog parsing, local read-state decoration and session-list filtering, counters
and grouping run on background workers. The list retains its last projection
and keyed scroll position while a replacement is prepared; canceled projections
cannot replace newer search or filter results.

Agent messages and ordinary questions render Markdown with headings, emphasis,
lists, quotes, links, tables, strikethrough, static tasks and code blocks. Code and
wide tables scroll horizontally inside the message. Approval commands and native
answers stay literal. Tapped HTTP(S) links open a browser; file links in agent
messages show the original computer/path and explicit Open or Save actions.
Documents are downloaded through the selected gateway, verified before use and
opened with an Android read grant or saved through the system document picker.
The limit is 10 MiB per nonempty regular file. A changed file or unavailable
computer requires an explicit retry. HTML stays literal and images do not download.
Parsing and render preparation use bounded background work and an exact-content memory cache;
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
Explicit child requests and blocking native requests also count as attention;
routine child failures and legacy hook-profile observations do not. As on the
desktop, optional Codex questions, which the agent keeps working without, are
not attention: a session whose only pending items are optional questions stays
out of Needs attention and its counters, while a required question, approval,
error or child request beside them still counts. Saved sessions are paused or
stopped; Archive contains immutable archived histories.

The computer filter supports several machines across workspaces. Counts and
cards use the same computer, project, and search scope. Results form collapsible
project groups in the swarm's shared project order, the order desktops show;
empty groups are hidden in Sessions. Filter, query, collapsed
groups, and list position survive opening a conversation and background polling.
Provider badges match the desktop identity motifs.

The computer chooser shows a count beside every machine, including zero, and
an All machines total. These chooser counts use the current session state filter
and project before machine selection or text search, so choosing one computer
keeps the other computers' counts visible. Counts update while the chooser is
open, retain separate workspace/computer identities and count each parent session
once. The chooser names its current state filter and states that counts precede
search; offline inventory and local drafts follow the normal filter rules.

An open conversation shows desktop-style labels 4 dp below its name: its owning
computer first, then status, so status caption changes keep the machine anchored.
Machine names and colours match session cards, including phone-local names,
offline computers, archives and subagents.
Tap the name/status header to open Session details. The provider label precedes
the model in the tools row, inset from the rounded model button to avoid clipping,
followed by compact Read all, Terminal and menu icons with 48 dp touch targets.
The title bar is 56 dp at ordinary font sizes and grows
when enlarged text needs it. Search loaded messages stays in the actions menu.
The chat shows a compact status strip only for loading, unread replies or pending
questions.

Session cards follow the desktop cards. A checkout line shows a small Git branch
mark and `folder / branch` (or detached HEAD); a linked worktree names its main
checkout folder, and ordinary folders show only their name with a folder mark. A subagent counter uses
the desktop's compact active count, such as `3`, `2+` or `?`. Tapping it lists
the children that are working, waiting for input, paused or unknown,
in stable ID order. Finished and failed turns stay hidden, as with the desktop default,
unless the child has an unsent draft on this phone. Profile groups are counted
but not listed. Tapping a child opens its session and then the child's activity
once that conversation is verified; Back returns to the session. Open subagent
lists persist with the other Sessions state. A running session with pending
optional Codex questions, which the agent keeps working without, shows an
amber question-mark chip with their count (`99+` above 99) right after its
status badge, as on the desktop; its spoken description says how many are
waiting. It shares the amber of attention but changes no status, filter or count.
The listing's `optional_questions` count wins. Without it (an older computer, or
while hgs cannot read its question index), the chip counts the connector's
optional attention items once per question. It is hidden for saved, archived,
exited and offline sessions; open the session to answer or skip the questions.

Visible live-parent cards also show read-only Git publication marks when both
the gateway and that exact computer advertise `git_status`: changed/new files,
outgoing/incoming commits, unknown publication, or verified clean/synced. Counts
above 99 display `99+`; accessibility keeps the full counts and verification
description. Ordinary non-Git folders and archives have no Git marks. Reading
status never initializes a repository, fetches, pushes or changes the checkout.

Git reads run only for cards intersecting the Sessions list viewport while the
list is resumed and focused, with no action or machine dialog. They stop on
navigation, backgrounding and loss of focus. Each exact computer/original folder
gets at most one read per 15 seconds, with at most two calls outstanding, 64
visible registrations and a 128-entry process-local cache. Connection, native
computer, route, checkout or capability changes invalidate current proof. Local
status expires 30 seconds after the original request starts; remote verification
also needs the computer's reported remote age plus that elapsed time below 60
seconds. Offline, expired or unverified results stay muted, never synced. No Git
status is restored from disk, and Windows gateways do not advertise this read.

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

Copy session reference stays in the conversation's actions menu, including for
child agents, without a duplicate toolbar button. Copying remains available
while the agent works and when a known computer is offline. References use the
computer's reported native machine UUID and snapshot host, not its relay ID or
display label. An unambiguous advertised relay alias may locate that computer
within the same workspace. Missing native identity is explained in the menu;
it never falls back to an unrelated computer or a display name.

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
When a reply read from recent inspection later gains a canonical history ID,
unambiguous native-provided aliases carry its existing read position forward.
A later unseen reply remains unread; equal text never establishes this identity.
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
elsewhere can be adopted here for the exact verified selected reply. Automatic
adoption for a nonselected conversation is deferred: fleet hints do not prove
which canonical incoming prefix belongs to that shared reply. Canonical history
counters can remain phone-local until a verified selection or explicit Mark read
/ Read all. Sessions reply badges follow the owning computer's current reply
identity and shared mark. When its bounded journal no longer lists a current
reply ID but retains a conversation-scoped read mark, old phone history does not
recreate a New reply badge. This changes no history watermark; a newly listed
reply and an independent Review later reminder remain visible.
Review later stays on this phone. This interim guard does not add native prefix
proof or change explicit reading and Read all.
Catalog refresh publishes rows and their local read decoration together, retaining
unchanged card objects and the list viewport while background preparation runs.
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

When both routes advertise `clear_context_dismiss` and the selected fresh native
inspection explicitly permits it, context details offer **Dismiss and clear…**
with a separate named confirmation. Its mode stays fixed while the dialog is
open and is checked again after the durable request is saved. Ordinary clear
continues omitting `dismiss_prompt`; the explicit mode saves and sends `true`
with the original request identity. Check result and recovery never repeat it.
Missing capabilities, cached evidence or a changed target cannot authorize it;
unsent composer text, files and question drafts stay intact.

Native controls are enabled only when both gateway and computer advertise them
and a fresh inspection permits the action. These include session lifecycle,
queue Send now, model and effort changes, process output and Stop requested,
scoped recovery controls, and exact child history. Scheduled model settings have
one durable authorized apply attempt when native readiness is observed, including
when another session is open. If that attempt is confirmed as scheduled again,
a later fresh NotReady → Ready transition in the same app process can authorize
one new attempt, pinned to the exact pending ID, model and effort. Repeated Ready,
restoration and checking an old receipt never grant that authority; unknown
results are checked using their original request. Uncertain actions expose their original receipt;
they are never retried automatically.

Model and effort opens a sheet with a scrolling native model list and a pinned
Effort row showing the current level. That row opens a separate effort page;
Back returns to models and preserves the staged selections. Selected rows have
a checkmark and accessible radio state, with at least 48 dp touch targets.
Default retains the native default choice. Selection and navigation only edit
the form; Apply keeps the exact-session, pending-request and readiness checks.
An OpenCode session's effort page lists the selected model's own variants with
the computer's labels; choosing a model without that variant returns to Default.
Variant names outside the shared effort names (Off through Ultra) are offered and
sent only when both the relay and the computer advertise `opencode_launch_effort`.

Model labels and order come from the shared `hgs` catalog, without Android
sorting or invented recommendation/cost badges. Codex uses its native cached
catalog's `display_name` (falling back to the ID) and stable ascending `priority`
order; missing or invalid priorities follow ranked entries in source order.
Claude uses the account catalog's `name` and retains its array order. Kimi
retains configuration order, and DeepSeek retains native group/model order.
OpenCode retains the service's model order. Desktop consumes
the same catalog, deduplicates model IDs and prepends the current model when absent.

OpenCode launch and session model pickers group the computer's short list by
provider and offer search when more than eight models are reported. Default,
Favorite and Free marks are read-only. A shortened list points to the desktop
for other models; opening the picker never starts the service or fetches a full
catalog. Accounts shows bounded provider details and the last known catalog
state, including offline or stopped-service reports. Provider credentials and
favorite changes remain managed on the computer.

Terminal uses a separate saved input buffer and a fresh exact native binding.
Snapshots poll adaptively while visible; input sends literal text or fixed keys,
with confirmation for Ctrl+C and Ctrl+D. Submitted means native input handoff,
not command completion. New session uses the selected computer's native provider,
account catalog and an explicitly chosen folder. Ordinary folders remain usable
without creating a Git repository. A verified repository defaults to a generated
worktree, created only after Create session; None and existing or manually
created worktrees remain available. A launch is confirmed only by its own native
request identity. For OpenCode, an **Effort** choice (Model default or the chosen
model's variants) appears only when that model reports variants and both the
relay and the computer's connector advertise `opencode_launch_effort`; otherwise
the launch request carries no effort.

## Voice and video messages

Settings includes **Voice messages**, with an independently selected transcription
computer and host-configured profile. Global settings lists verified online computers
and reads the selected computer's `speech_profiles` directly, without opening a
conversation. **Refresh voice profiles** reloads those profiles and reports progress
or a read failure. Computer choices are scoped to their workspace; their searchable
picker shows at most 100 matches. Choosing a default affects new recordings only.
Existing recordings retain their computer, profile and reviewed revision.

Inside a conversation, discovery still comes from its owner and lists the computers
it can reach. An unavailable saved selection never silently switches providers.
Both the relay and conversation computer must advertise `voice_profiles`,
`voice_submit`, `voice_status` and `voice_cancel`. When supported, `speech_stats`
displays the selected processing computer's aggregate local counts. Audio and
transcription are excluded from Firebase events. See [mobile architecture](../../docs/mobile-architecture.md#voice-input)
and [analytics](../../docs/analytics.md).

`DefaultVoiceProvidersSmokeInstrumentation` exercises the real settings controls
without a selected conversation: initial discovery, Refresh, computer/profile
selection, explicit revision changes, failed reads and retry, and stale response
rejection. A retained recording keeps its original route throughout. It uses only
computer-scoped reads intercepted at a reserved synthetic endpoint.

**Manage speech providers** works without an open conversation. Choose the processing
computer, then add or edit **Local GigaAM** or an **OpenAI-compatible server**. Local
model/device choices come from that computer; remote profiles use a base URL, model,
optional language and optional API key. HTTP requires an explicit local/private
network opt-in. Keys are encrypted for the selected computer before crossing the
relay and never returned to the phone. Saving does not install packages or download
weights. **Test** sends synthetic audio to the saved profile, without using the
microphone or retained recordings. Removal requires confirmation.

The editor names the processing computer and reports its last service check separately
from provider readiness. Brief help and an explicit browser link follow the selected
type: **Local GigaAM setup** or **OpenAI-compatible server setup**. The manager offers
**Speech provider documentation** for the general [setup document](../../docs/speech-setup.md).
Opening a link preserves unsaved fields and the entered key. Links never include profile settings,
server addresses or credentials. Browser failures are shown beside the link.

Configuration edits use the computer's revision and a durable operation UUID.
Conflicts retain the form for explicit review; uncertain saves recover through
**Check operation status** and are never repeated. Editing a provider does not
change the selected default or saved recording. **Use for new recordings** explicitly
sets the default while preserving a recording that is already saved. A retained recording pins the
provider revision: if it changes, explicitly select the provider again before
sending. See [provider configuration](../../docs/speech-providers.md).

With an empty message and no attachments, the composer shows a microphone in the
Send position. Tap to switch microphone/camera, hold to record and release to send. Slide left
to cancel or up to lock. **Preview before sending** in Voice settings keeps both
audio and video after release for playback and a separate Send. It defaults off
on this device; an active capture keeps the preference it started with.
Stop a locked recording to retain a preview. Typing text or adding an attachment restores the Send
button. While holding, a compact recording row replaces the empty editor: a red
indicator, elapsed time with tenths of a second, and **Slide to cancel** with a
left chevron. When the full instruction cannot fit at the current font size, the
visual cue shortens to **Cancel**; accessibility retains the full swipe instruction.
Sliding moves and fades the hint. The text editor and its state stay
mounted underneath and reject input; stopping restores them. The microphone keeps
its 48 dp interaction target and inset 40 dp visual face. Explicit Stop and Cancel
actions remain available through accessibility and keyboard controls.

A short system haptic confirms microphone capture has started and accompanies
explicit stop or cancellation. Feedback belongs to that capture: permission
prompts, rejected starts and late gesture events do not produce extra pulses.
Android's vibration settings are respected. Haptic availability never blocks
recording or saving its preview.
Local capture starts without waiting for provider discovery; discovery refreshes
in the background. Audio capture lasts at most two minutes. Releasing an owned hold requests sending;
stopping a locked recording or leaving the conversation keeps a preview without sending. The audio preview is an
encrypted durable WAV file on this phone. A compact **Pending** card contains
playback and seeking, with Delete and Send icon buttons on the right. All three
buttons keep 48 dp touch targets and accessible **Play**, **Discard** and **Send
voice message** names. Discard still asks for confirmation. A process killed before
capture finishes can lose the unfinished recording. Completed previews survive
restart and remain accessible in Drafts. Capture and bounded 48 kHz fallback
conversion run off the main thread.

A preview recorded before choosing a provider remains saved without a fabricated
selection. A contextual **Transcription provider** action appears when provider
recovery is needed; the normal ready preview has no settings button or saved
recording notice. The dialog always addresses the recording's original
conversation. A preview viewed elsewhere identifies that destination.
Send requires fresh confirmation of that exact available provider; discovery
failure or an unavailable selection keeps the audio and never chooses a fallback
or sends automatically. The conversation must have a confirmed run and conversation
identity before recording.
The composer preview scrolls within a bounded area while the software keyboard is
open, keeping the typed draft visible and editable after capture stops.
The retained preview and expanded editor share that available height; when two
stacked controls cannot fit, Collapse moves beside Attach with its 48 dp target.
The 280 dp / 180% system-text native fixture reaches playback, seeking, Discard
and Send with the IME open, preserving the editor object, text and selection.

Video mode records up to 60 seconds and 10 MiB with CameraX, with an owned circular
preview and a camera switch. The phone extracts mono 16 kHz speech audio on a
worker; one message UUID retains the original MP4 and its derived WAV. The agent
receives the recognized text. Video needs the additive `voice_video` capability
on the relay and conversation computer. Older clients keep audio playback.


While recording, the typed draft and selection remain saved beneath the recording
row, and ordinary message sending and attachment changes are disabled. With a hardware keyboard, **Space** or **Esc** stops capture
into the saved preview; holding or releasing that key never sends, discards or
starts playback. Editing resumes when capture ends, subject to existing send locks.

Denied microphone access opens a focused explanation with **Open app settings**.
This opens Zerus's Android settings; choose **Permissions → Microphone** and allow
access while using the app. After granting permission or returning from settings,
press the microphone again to record. Permission results never start recording
automatically after the original press has ended. If the settings page is unavailable, the explanation retains the manual navigation path.

**Send voice message** saves an immutable audio reference and request UUID before
uploading mono 16 kHz signed little-endian PCM to the conversation computer through
the trusted relay. That explicit action authorizes transcription and delivery of
the recognized text to the agent. The phone never sends a second native message
after recognition. Existing typed text and attachments remain unchanged. Cold-cache
sending requires the normal full-context warning; voice does not implicitly clear,
compact or resume a session. Child and archived conversations cannot send voice;
archived voice messages remain playable.

A single history card shows the original audio, processing or delivery status,
errors, and expandable recognized text. Another device can open the same card and
fetch audio using authenticated `attachment_read` ranges, verifying identity,
length and SHA-256 before playback. Status updates use monotonic revisions; exact
native bindings suppress duplicate user rows without matching on transcript text.
The circular play/pause control sits beside the recording's measured waveform.
Tap or drag the waveform to seek; Android accessibility controls expose the same
position and elapsed/total duration. Seeking a paused recording keeps it paused.
Older messages show a plain seek track until their computer supplies waveform
metadata; the phone never invents amplitude bars or downloads audio just to draw them.
The phone retains encrypted voice receipts and local audio copies in Drafts; removing
a local copy does not remove the computer's history or cancel delivery. New capture
is bounded by a 100 MiB local audio budget and the outbox holds at most 100 receipts.

Interrupted uploads retain their original UUID and recover with status reads only;
recordings are never automatically uploaded again. The computer owns the durable
transcription/delivery pipeline even if the phone disconnects. Failed or uncertain
results remain visible. Playback stops when the app leaves the foreground. The
relay can read relayed audio; this transport has no end-to-end encryption.

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

The clear command removes app data on that emulator. The smoke checks durable
preview without implicit upload, real PCM capture during delayed, failed and
unavailable provider discovery, explicit preview-scoped provider selection without
fallback, explicit Send, local and authenticated remote
playback, processing/history cards, recognized text, concurrent typed-draft
preservation, lost-submit status-only recovery, cancellation and background capture
stop. Hardware Space/Escape coverage verifies the read-only draft, blocked ordinary
send, held-key/release consumption, retained preview and restored editing. It also
denies microphone access twice, opens Android's app permission
settings, grants access there and verifies recording requires a new explicit tap
after returning.
It does not measure recognition accuracy or replace a physical microphone test.

The gesture smoke exercises the actual composer with injected pointer events on
that same disposable emulator. Start with fresh app data and microphone permission
unset; the runner opens Android's permission prompt itself:

```sh
./gradlew -PzerusInstrumentationRunner=app.zerus.mobile.VoiceGestureSmokeInstrumentation assembleDebug assembleDebugAndroidTest
adb -s emulator-5580 install -r app/build/outputs/apk/debug/app-debug.apk
adb -s emulator-5580 install -r app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
adb -s emulator-5580 shell pm clear app.zerus.mobile
adb -s emulator-5580 shell am instrument -w app.zerus.mobile.test/app.zerus.mobile.VoiceGestureSmokeInstrumentation
```

It checks short-tap mode switching, retained preview when provider discovery is
unavailable, left-swipe cancellation, interrupted pointers,
background/navigation stop, permission results after release, preserved drafts
and zero voice uploads before a separate Send. The compact Pending preview checks
cover action order and touch targets, local playback and seeking, discard
confirmation and a final explicit Send to the synthetic gateway with the exact
durable audio identity. The healthy preview has no provider settings action;
the speech smoke covers contextual recovery after a provider revision changes.
It also checks inline feedback
bounds, keyboard retention and input rejection under the overlay, restored editor
selection/expansion, and capture-scoped calls to the Android haptic API. The emulator
verifies dispatch and timing; physical vibration still depends on the device and
its settings.

`TerminalFollowSmokeInstrumentation` uses the same build, install and fresh-data
steps. It renders synthetic 60-row Terminal snapshots and checks, through the
scrollers' accessibility actions, that opening follows the bottom cursor row, a
higher cursor and back, that a user scroll pauses following across snapshots,
that **Follow cursor** resumes it and that a selection never moves.

`TerminalLinkSmokeInstrumentation` uses the same steps. It parses tmux
`capture-pane -e` OSC 8 output and checks the link spans, a confirmation that
shows the whole target and host, **Copy link**, the hand-off of only the parsed
path to computer files, and inert links while selecting. Nothing opens.

`QuestionLayoutSmokeInstrumentation` uses the same build, install and fresh-data
steps. It opens synthetic history with a pending question, measures the inline
card from a screenshot and checks the card limit, the whole answer field, the
footer gap, scrolling to the last option and a compact short question. It sends
no answers.

Multi-part questions show **Question 1/2** and an explicit **Next** action after
the current answer is valid. Only the final **Send all** submits the complete
request. The Other field's keyboard action follows the same steps. Earlier
selections and custom text remain editable while navigating; an incomplete earlier
answer is offered for review before submission. Existing exact-request, approval
and cold-context consent checks still apply to the final send.

`QuestionDeliverySmokeInstrumentation` exercises physical Next/Enter/Send all controls,
retained per-question answers, one complete synthetic answer, confirmed and missing
receipt recovery, and native-to-canonical read progress with a later unseen reply.
Use fresh private emulator data and repeat at a narrow width with font scale 1.8.
The narrow run exercises recovery inline and opens the full-height question editor
through its physical **Expand** action before answering.
Its reserved endpoint is intercepted in-process; no agent receives a message.

Private previews may override only their build identity using
`-PzerusVersionName=0.1.31-voice.245 -PzerusVersionCode=10245`. Choose a code greater
than the installed build; the next regular update must exceed the preview code.
These properties do not change the signing key or checked-in release version.

The checked-in Gradle wrapper uses Gradle 8.13 with an official distribution
checksum. Dependencies come from Google Maven and Maven Central. The debug APK
uses the local Android debug signing key; public distribution requires an
owner-controlled release key. Generated APKs and local configuration are ignored.

Third-party notices list every library of the published Firebase release build.
After a dependency change, regenerate them from that configuration (a synthetic
`google-services.json` like CI's is enough) and commit the changed assets:

```sh
./gradlew -q --init-script ../../scripts/android-resolved-artifacts.gradle -PzerusFirebase=true :app:zerusResolvedNotices
python3 ../../scripts/android-notices.py
```

The init script only records the resolved `releaseRuntimeClasspath`, with the exact
artifact files Gradle resolved, in `app/build/zerus-notices/resolved.json`; it
changes no build setting. `--check` compares instead of writing; CI runs it after
the unit tests and release lint. The script
writes nothing when a shipped library has no artifact, when a library declares no
recognized license or needs a license text that has not been reviewed into
`app/src/main/assets/licenses`.
`ThirdPartyNoticesTest` also fails locally when a declared library or platform
version is missing from the generated list.

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
Offline bundled Third-party notices include selectable license text, every
shipped library by license and, in Firebase builds, the open source notices
that Google Play services and Firebase embed; long texts open on request.
Back returns to the same app content; notification and push controls stay in Machines.
Anonymous checks follow the selected channel: Dev uses
`https://zerus.dev/updates/v2/android/dev.json` with a fifteen-minute interval;
Stable uses `https://zerus.dev/updates/v1/android/stable.json` with a six-hour
interval. Checks run at startup and through WorkManager when a network is
available; Android may defer background work. An available update or pending installation badges
the Settings button. The indicator remains available without notification
permission. Check, Download and Install are separate actions; no update silently
repoints or removes a paired workspace. The suggested relay for new pairing is
`https://relay.zerus.dev`, and custom HTTPS relays remain editable.

Attachment receipts match delayed queued native acknowledgements by their complete
per-request expansion; canonical aliases preserve one visible message while
independent sends stay distinct.

Downloads are bounded, cancellable and verified by exact size and SHA-256. Before
installation, the APK must match the advertised package, version, Android minimum
and the current installed signer. Saved verified downloads work offline.
Installation saves private drafts first and waits for active deliveries or
commands. Android's install-source permission and confirmation remain required;
the updater explicitly requests user action through
[PackageInstaller](https://developer.android.com/reference/android/content/pm/PackageInstaller.SessionParams#setRequireUserAction(int)).
Install is the consent to that prompt: it opens directly for the session the tap
committed while Updates stays open in the foreground. Continue Android
confirmation remains only when the prompt could not open then. The exact prompt
is consumed and its waiting-for-result state is saved before opening Android;
while Android finishes, the primary action is disabled and duplicate callbacks
cannot reopen it. Foreground recovery observes the existing session and installed
version without resubmitting. Status, progress and action slots keep their geometry
through preparation, confirmation and installation, including larger text sizes.
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
optional questions, errors, and turn completion. Input and errors are on by
default; optional questions and completed turns are off. The master switch
disables all session alerts. Disabling a type also removes its existing cards.

A session whose only pending items are optional Codex questions, which the agent
keeps working without, notifies as *Optional question on* its computer, and only
with that switch on. Each question ID announces at most once per session slot:
answering or skipping one keeps the card, a dismissed card stays dismissed, and
a resume, connector restart or emptied and refilled list announces nothing
again. Questions pending while the switch is off, or while the session needs
input or has an error, are not announced later. Without optional questions the
card is withdrawn. A blocking request or error in the same session takes
precedence and its card does not change for new optional questions. When the
switch is off, a finished turn can still notify. Optional cards never join the
summary of sessions that need attention, and a new need may replace one when the
shade is full. As on the desktop, optional-only sessions are not in Needs
attention or its counters.

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
for input, approvals, optional questions or errors (the relay and the phone rank a session that
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

Settings > Computer analytics reads private, content-free agent summaries through
the capability-gated `analytics_read` operation and the existing authorized
computer route. It requests 7, 30 or 90 inclusive UTC days; each reply is limited
to 1 MiB and 1,080 daily buckets. The page retains at most 32 computers and tries
at most three configured routes per computer, with two reads in flight. Extra
computers are explicitly excluded. Reads and parsing run away from the UI thread;
daily rows use a lazy list. Leaving the page cancels pending reads. The bounded
cache is memory-only and disappears with the application process.

Stable computer UUIDs deduplicate multiple gateway routes. Newer source snapshots
replace earlier revisions, including an epoch reset after purge; repeated reads
never add the same totals. Each computer shows freshness, coverage gaps,
unsupported measures and collection state. Offline or rejected reads retain
explicitly marked last-known data for the same range. Unknown measures stay
unknown. Main and child turns have separate filters. Known work/wait durations
are distinct from elapsed session age, and token counts do not imply costs.
Collection enablement, retention and purge remain local computer controls.

Voice statistics use the existing `speech_stats` read separately, filtered to the
same UTC interval and replaced once per processing computer. Profile identifiers
are discarded. Accepted audio minutes, job outcomes and recorded queue/processing
totals stay separate from agent turns; the phone does not count a recording a
second time. The existing speech contract provides neither full coverage nor
latency percentiles, so the page does not claim either.

Review diagnostics builds a separate allowlisted report of the Android build
number, date range, fixed read states and coverage codes. It excludes machine
names/IDs, connection addresses, account details, source epochs, aggregate usage
values, prompts, code, paths, recordings and transcripts. The user must review
the exact report and tap Share reviewed report before Android's share chooser
opens. Nothing is automatically written or uploaded, and these features do not
change the independent Firebase Usage analytics or Crash reports controls.

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

Accounts is a read-only viewer grouped by workspace and provider-reported account identity, matching desktop accounts. Each card contains its machine labels, using the same names and colors as Sessions. Provider account IDs and organization scope own identity; email-only reports join an ID only when unambiguous. Signed-out and unidentified profiles stay machine-local. Compact cards show account type and aligned period, remaining reset time and usage columns, with recurring-period and hourglass glyphs; tap a card for the full identity, limits, per-machine profiles and update times. Authentication problems remain visible in the list; optional usage availability, refresh progress and probe failures are neutral diagnostics in details. Saved usage is labelled once rather than repeating each machine's timestamps. Usage selects one best-quality report and never adds quotas across machines. It shows reported account identity, plan, usage windows and wallet balances when the connector provides them. Unknown limits stay unknown; ended windows need a provider refresh. Account snapshots use a separate encrypted private cache, so offline values remain available with their original provider update time and stale styling. A connector advertising `accounts_snapshot` is required; Refresh reads its latest snapshot and does not force a provider refresh.

## Project and account selection

New session is a full-screen form with Computer, Source, Project, Working folder,
Worktree, Account and agent, and Session name sections. Computer is selected
inside the form, initially preferring the current machine scope; there is no
separate machine-selection step. Changing it resets native folder/account
choices, retains the URL and session name, and keeps an explicit project/Inbox
only within the same workspace and canonical swarm.

Source offers **Working folder** and **Git repository**. A repository URL suggests
the destination leaf; **Clone to** is editable and Browse chooses its parent.
A manually entered destination is retained when the URL changes. **Clone and start**
checks the selected computer, URL, destination and launch choices before creating
anything. It clones into a new folder and starts there only after a definite
native result. Existing folders are never replaced and Git is never initialized
for an ordinary folder. This requires a current Rust connector and the guarded
native mobile acquisition ABI; older/Python gateways show an update reason.
A checkout interrupted by transport loss is kept as one durable original request.
**Check original result** reads that clone's native receipt; a confirmed recovery
offers **Use checkout** and never starts a session automatically. A created checkout
also remains available after a later launch failure or closing the form. An
unconfirmed attempt reserves its original destination; Inspect folder opens it
for review, while a different destination can be used for a new explicit attempt.

Repository recovery selects the attempt for the current Clone to destination and
pages other unresolved attempts in bounded groups. Each Check original result
reads that attempt's original native clone UUID; choosing another destination
keeps it recoverable. A read-only check may resolve a parent alias (for example,
`/var/tmp` to `/private/var/tmp` on macOS). The form shows that canonical path and
requires a fresh explicit Clone and start before creating it; mutation and status
receipts must keep the confirmed exact destination.

 Account and agent pairs share one list: a
large colored provider logo spans the email and one line of cached limits, or `limits n/a`.
Small usage/elapsed rails and pace colors match the desktop status footer.
Default labels, profile IDs and quota error details stay out of these rows;
the list matches Accounts grouping using only the selected computer's bindings.
Aliases of the same provider account and organization share a row; conflicting
account IDs, organizations and unidentified profiles stay separate. Launch uses
the exact selected or default local profile. The chooser reads only
existing machine/profile snapshots and cached catalog data, without refreshing
provider quotas. Tapping outside never closes it; Close or
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
form shows the sibling `<root>-<branch>` path before launch; routine worktree
explanations are omitted from the form.
Create session then locks the form, creates that worktree from `HEAD` with a
durable receipt, checks its folder and catalog, and launches there. A worktree
of a project folder is placed as a related worktree; other explicit project
placements retain their existing assignment checks. Opening the form and checking folders never
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

### Synthetic video-message qualification

Use a fresh, isolated Android emulator with emulated front/back cameras (never a
personal device or webcam). Build with
`-PzerusInstrumentationRunner=app.zerus.mobile.VideoMessageSmokeInstrumentation`,
install both debug APKs, then run
`adb -s <isolated-emulator> shell am instrument -w app.zerus.mobile.test/app.zerus.mobile.VideoMessageSmokeInstrumentation`.
The runner checks short tap, real CameraX MP4 capture, upward lock, retained
private media, audio extraction, exact one-time upload and left cancellation
against an in-process gateway fixture. Its screenshot is saved to the app's
external files as `video-message-preview.png`.
