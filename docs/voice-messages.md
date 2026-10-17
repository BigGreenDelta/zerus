# Persistent voice messages

## Video messages and recording controls

Owner direction (2026-10-16, zerus-eo7j): video messages retain the recording
in conversation history and deliver its recognized speech to the agent.

Desktop controls (owner revision 2026-10-17, zerus-0dwp) start recording with a
normal click on **Voice** or **Video**. The adjacent icon immediately to its right
switches microphone/camera mode without opening a device and remembers the mode
on this computer. It stays disabled while opening, recording or retaining a
preview. Recording continues after the starting click; the next **Send** click
requests delivery. **Stop and preview**, Space or Escape retain a reviewable draft.
**Cancel recording** discards an active capture.

Android controls follow Telegram Android
[`f32ffeec763f`](https://github.com/DrKLO/Telegram/blob/f32ffeec763f0bdbf4bec25b52d654f77bff733d/TMessagesProj/src/main/java/org/telegram/ui/Components/ChatActivityEnterView.java#L3059-L3220).
A short press switches microphone/camera mode without opening a device. Holding
starts the selected capture after a short threshold. Releasing an owned,
unlocked recording requests sending by default. A left drag cancels and an upward
drag locks. A locked recording continues after release, with separate
Stop/preview, Send and Cancel controls.

**Preview before sending** in Voice settings retains both audio and video for
review and a separate **Send**. Desktop shows **Stop** as the recording button;
Android retains the preview on release. The device-local preference defaults off
and is captured at recording start; changing it cannot send an existing recording.
On both platforms,
system cancellation, lost focus, navigation, permission callbacks and capture
limits cannot submit. Completed, explicitly stopped recordings retain a preview. Provider discovery cannot
silently resume a deferred send, and cold-context confirmation remains explicit.

Video recordings use ordinary MP4 files displayed through a circular viewport.
Their audio is extracted from the same recording on a background worker and
converted to the existing mono 16 kHz PCM16 speech input. One durable message UUID
owns original video, derived audio, transcription, provider pin and delivery
state. Acceptance saves both media before acknowledging; recognition and native
delivery continue without the originating client. Only recognized text enters
native agent input. Original media remain available in archived history.

Video is limited to 60 seconds and 10 MiB; ordinary audio keeps its 120-second
limit. The additive `voice_video` capability permits an optional `video` on
`voice_submit`. Clients require that capability from both relay and conversation
computer before uploading video. Older audio-only payloads stay unchanged, and
older clients can play the retained audio of a video message. Video metadata
uses attachment index 1 beside the unchanged audio index 0. Pending and uncertain
delivery identities are never replayed. Playback starts only after a tap; lists
use bounded posters and retain at most one active media player per client.

Voice records a separate audio draft. Stopping recording makes it available for
playback, discard or **Send voice**; it does not change the typed draft. Sending
authorizes recognition and delivery of the recognized text to the selected
conversation. The original recording and expandable transcript remain together
in conversation history on desktop and Android.

Capture depends on local microphone permission and storage, not on provider
discovery. Clients open the microphone first and refresh available speech
computers asynchronously. An unsuccessful or delayed refresh must not stop
capture, discard its preview or send anything. The capture limit remains
120 seconds even when no provider has responded.

The preview keeps its intended conversation and selected speech machine/profile.
Versioned profiles additionally pin `profile_revision`. Refreshing discovery or
editing settings cannot silently change that pin: a changed profile requires
explicit reselection. The queued voice worker passes the pin through ASR
acceptance, where a mismatch fails before audio is sent to the provider.
Discovery must not replace an unavailable explicit selection with another
provider. A preview recorded before choosing a provider can be assigned one
before sending. Only an explicit send gesture or **Send**, with a confirmed available
provider for that conversation, authorizes submission. Refreshing providers
alone never submits the recording.

## Android recording gesture

When the editor has no text or attachments, its trailing Send action becomes a
microphone or camera. Tap to switch modes, hold to record, release to send,
slide left to cancel, or slide up to lock. Stop a locked capture to retain a
preview without sending. A compact **Pending**
card identifies the unsent preview, with playback and seeking followed by Delete
and Send icon buttons. The buttons retain accessible **Discard** and **Send voice
message** / **Send video message** names and 48 dp touch targets; deletion still requires confirmation.
Provider settings appear only when the preview needs provider recovery, and a
recording for a different conversation identifies that original destination.
Text or attachments restore the ordinary Send action.

Recording keeps the text draft read-only and disables attachment changes. During
a hold, the empty editor displays a single recording row: a red indicator, a
captured-audio timer with tenths, and a left chevron with **Slide to cancel**. Narrow
layouts use **Cancel** when the full phrase cannot fit at the current font size,
while accessibility retains the complete swipe instruction. The
hint follows the left drag and fades; the underlying editor and the trailing
48 dp pointer/focus/semantics node remain mounted. Explicit Record, Stop and
Cancel actions support accessibility, and Space or Escape on a hardware keyboard
stops into the preview. A permission result requires a fresh recording action. Losing
the pointer or leaving the composer must end capture without submission; a late
release or permission callback must never start another recording or affect a
newer one. Capture stays pinned to its original conversation and provider.
Canceled or claimed Android pointer input cannot send or switch modes; canceling
a second click on a locked recording leaves that capture running.

Round-video capture opens a large live circular preview over the conversation,
with a progress ring and remaining-time countdown. The timer follows captured
media statistics and the camera's actual 59-second recording limit; a file-size
limit can finish earlier. The preview adapts to the available viewport above
the retained composer and keyboard. Locked recording exposes camera switching,
Cancel, Stop and, when preview-before-send is off, Send. Stop always retains the
preview. The same-window layer preserves the held pointer and window focus;
release or cancellation starts a short fade and shrink without delaying camera
Finalize or cleanup. Covered history stays out of accessibility and does not
accrue visible-read progress until the closing animation has finished.

The recording-screen behavior was inspected in Telegram Android's
[InstantCameraView](https://github.com/DrKLO/Telegram/blob/f32ffeec763f0bdbf4bec25b52d654f77bff733d/TMessagesProj/src/main/java/org/telegram/ui/Components/InstantCameraView.java#L620-L635)
and its [opening/closing animation](https://github.com/DrKLO/Telegram/blob/f32ffeec763f0bdbf4bec25b52d654f77bff733d/TMessagesProj/src/main/java/org/telegram/ui/Components/InstantCameraView.java#L881-L943).
The Compose implementation is original Zerus code; no Telegram source is copied.

With a narrow screen, large system text and an expanded editor, the open keyboard
can leave no space for the retained preview. Close the keyboard to reach its
controls; the editor's text, selection and expanded state remain saved.

Haptic feedback is scoped to one capture. A confirmed microphone start and an
explicit stop or cancellation each request a short `KEYBOARD_TAP`; duplicate and
late callbacks cannot replay it. The terminal pulse waits until the microphone
has stopped and released. Permission prompts and a failed start are
silent. Feedback uses the Android view API without flags that override user
settings and cannot prevent audio capture or persistence if unavailable.

The interaction was checked against Telegram Android's
[recording row](https://github.com/DrKLO/Telegram/blob/f2908b14133bbffbf7ab04f641ecb5bfaf533242/TMessagesProj/src/main/java/org/telegram/ui/Components/ChatActivityEnterView.java#L9742-L9766)
and [recording feedback](https://github.com/DrKLO/Telegram/blob/f2908b14133bbffbf7ab04f641ecb5bfaf533242/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L4706-L5005).
Zerus implements these interactions in its existing Compose components with the explicit Stop/preview path described above.

## Message presentation

Audio and video use one compact media block with aligned metadata and icon
controls. Recognized text is collapsed behind its transcript icon. Android's
recording menu contains status checks, cancellation and unsent recovery actions;
errors remain visible. A clock marks uploading or processing, one accent-colored
check confirms native submission, and an exclamation marks failure or unconfirmed
delivery. Cancelled delivery explicitly says **Not sent**, without a close/delete
cross beside the timestamp. Tap the time/status capsule for its plain-language
explanation; it has a 48 dp touch target in both media presentations. There are no
read/completion ticks. An unfamiliar state remains unconfirmed.

Saved drafts contains retained previews and recordings that still need delivery
or recovery review. Confirmed submitted recordings stay in conversation history,
including local playback and transcript, and are excluded from Saved drafts even
after restart. This changes presentation only: no receipt or media is deleted,
and no request is replayed. Terminal local copies can still be removed explicitly
from the message-status dialog, with confirmation. The existing 100-receipt bound
is unchanged.

Android round videos sit directly in the conversation with compact duration,
time and delivery indicators. Their transcript icon opens a text card with a
small circular poster and the existing seekable derived-audio player; the large
video is replaced while the exact recognized text is visible. The up-arrow
returns to the circle. Each message remembers its own disclosure state. Changing
this view never opens media or submits a message; it stops only that recording's
current player, preserving playback of other messages.

The visual reference is Telegram Android's
[ChatMessageCell](https://github.com/DrKLO/Telegram/blob/f32ffeec763f0bdbf4bec25b52d654f77bff733d/TMessagesProj/src/main/java/org/telegram/ui/Cells/ChatMessageCell.java#L29109-L29143),
which groups media time with clock, check and error glyphs. Zerus uses its own UI components
and state model; no Telegram source is copied.

## Authority and processing

The conversation computer owns each voice message. It retains its original WAV and optional MP4 atomically,
target identity, selected speech machine/profile and workflow state before
acknowledging acceptance. Processing continues independently of the client.
The speech machine may be another configured computer, resolved and verified by
its stable machine UUID from the conversation computer. Client SSH aliases are
never interpreted as server-side routes.

An installed per-user service runs `hgs voice worker`: `hgs-voice.service` on
Linux, `com.hgdev.hgs-voice` on macOS. On Linux a submission starts that
service; otherwise it starts a detached worker, unless a worker already runs or
the service's worker waits to take over. The service's own worker (the process
launchd or systemd reports for the job or unit; only when systemd cannot
answer does the unit's control group decide) never exits because another
worker holds the worker lock, which would make the service manager start it
again every 10 seconds: it waits for the lock, and a detached worker hands over
to it between rounds.

The workflow states are `queued`, `transcribing`, `ready`, `waiting_agent`,
`delivering`, `submitted`, `failed`, `cancelled` and `uncertain`. The same message
UUID survives every transition. Only recognized text is delivered to the native
agent; the original audio is a voice-history attachment, not an instruction for
the agent to read a file. Native input receipts prevent replay after ambiguous
delivery. Voice messages are processed in order for each conversation; an
ordinary typed message can be sent while recognition is pending.

Original audio is retained with voice-message history rather than the short-lived
ASR job. Relay payloads and attachment download chunks keep their existing short
retention. Cancellation stops pending delivery where possible and preserves the
original recording and any available transcript for review. It cannot undo a
native message that has already been delivered. Native dialogs, permission
prompts, compaction and changed conversation identities never receive implicit
confirmation from the voice worker.

## Additive v1 transport

Desktop invokes `hgs [@host] voice <session> --json`. The fixed JSON request
contains `action`, `request_id`, `expected_run_id` and
`expected_conversation_id`. Actions:

* `profiles`: returns `computers`, each containing `machine_id`, `name`,
  `profiles`, `available` and an optional `reason`. Computers are queried
  concurrently, but their transport processes start one at a time: on macOS a
  query started in between could inherit another's pipes and hold that request
  open until it ends itself.
* `submit`: also contains `speech: {machine_id, profile_id}` and
  `audio: {format: "pcm_s16le", sample_rate: 16000, channels: 1, data_base64}`.
  Recording duration is bounded to 120 seconds. The request UUID is the permanent
  message UUID. Optional `video` contains `format: "mp4"`, `data_base64`,
  `duration_ms` (100–60000), `width` and `height` (16–1920), and optional
  `poster_base64` (JPEG, at most 8192 decoded bytes). Video requests use at most
  10 MiB MP4 and 60 seconds PCM; audio/video duration may differ by at most
  one second. The full video envelope is bounded to 19 MiB; audio-only limits
  stay unchanged. `profiles.features` advertises `voice_video`; native help
  includes `voice-video-v1`. Video metadata returns attachment index 1 and
  its exact byte count, dimensions, duration and optional poster.
  `speech.profile_revision` is an optional exact configuration revision returned
  by provider discovery. Older clients and unversioned providers may omit it.
* `status` and `cancel`: also contain `message_id`. Each operation has a new
  request UUID while retaining the original message UUID.

Android uses `voice_profiles`, `voice_submit`, `voice_status` and `voice_cancel`
inside the existing relay envelope. `computer_id` identifies the conversation
computer; `session` is its exact session name. The envelope's `payload` is the
native request without `action`. Its `request_id` matches the outer request UUID.

Message responses have `schema: 1` and `message`, containing `id`, `run_id`,
`conversation_id`, `created_at`, monotonic `revision`, `state`, and `audio`.
Optional fields are `transcript`, `error_code`, `error` and `video`. Audio metadata uses
`request_id` equal to the message UUID, `index: 0`, `name: "voice.wav"`,
`mime: "audio/wav"`, `bytes`, `duration_ms` and an additive `waveform` array.
The same array appears in the history audio attachment. It contains exactly
64 integers from 0 through 255, computed from the original mono PCM16 samples.

For `N` samples, bin `i` (0 through 63) starts at `floor(i*N/64)` and ends
exclusively at `max(start+1, floor((i+1)*N/64))`. Very short clips therefore
reuse a sample for an otherwise empty bin. Each bin's amplitude is
`floor(sqrt(floor(sum(sample*sample)/sample_count)))`, using signed samples and
integer arithmetic. Let `peak` be the largest bin amplitude. If it is zero,
all 64 output values are zero; otherwise each output value is
`floor((255*amplitude + floor(peak/2))/peak)`. This normalization keeps quiet
recordings visible while preserving their relative RMS envelope. Empty audio
also produces 64 zeros. No random or synthesized speech pattern is used.

New messages persist waveform metadata with acceptance. The independent voice
worker backfills retained older messages in indexed batches of at most four
recordings per pass, reading at most 15,360,176 bytes of original WAV data per
pass and one recording at a time. Each successful backfill increments the
existing message revision and history change sequence; it never repeats ASR or
native delivery. Invalid legacy audio is marked privately and skipped so it
cannot block later recordings. Inspection and history read stored metadata
without reading audio. Older hosts or records awaiting backfill can omit the
field; clients should use a neutral playback placeholder until it arrives.

History exposes `source: "hgs_voice"`, `type: "UserMessage"`, `message_id` and
`request_id` equal to the voice UUID, `voice` containing the authoritative
message, and `attachments` containing its audio metadata. `detail` is the
transcript when available. Inspect exposes these rows in `voice_messages`.
Existing `attachment_read` retrieves the original, including while recognition
is pending, with exact conversation/archive authorization. Processing revisions
update the same history card. Text equality alone cannot identify a voice
message or associate it with a native user prompt.

The first implementation sends from live primary conversations with verified
native identity. Archived conversations retain playback; child-agent recording
is disabled until it has a separately scoped delivery contract.
