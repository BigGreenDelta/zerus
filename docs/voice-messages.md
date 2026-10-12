# Persistent voice messages

Voice records a separate audio draft. Stopping recording makes it available for
playback, discard or **Send voice**; it does not change the typed draft. Sending
authorizes recognition and delivery of the recognized text to the selected
conversation. The original recording and expandable transcript remain together
in conversation history on desktop and Android.

## Authority and processing

The conversation computer owns each voice message. It retains its original WAV,
target identity, selected speech machine/profile and workflow state before
acknowledging acceptance. Processing continues independently of the client.
The speech machine may be another configured computer, resolved and verified by
its stable machine UUID from the conversation computer. Client SSH aliases are
never interpreted as server-side routes.

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
  `profiles`, `available` and an optional `reason`.
* `submit`: also contains `speech: {machine_id, profile_id}` and
  `audio: {format: "pcm_s16le", sample_rate: 16000, channels: 1, data_base64}`.
  Recording duration is bounded to 120 seconds. The request UUID is the permanent
  message UUID.
* `status` and `cancel`: also contain `message_id`. Each operation has a new
  request UUID while retaining the original message UUID.

Android uses `voice_profiles`, `voice_submit`, `voice_status` and `voice_cancel`
inside the existing relay envelope. `computer_id` identifies the conversation
computer; `session` is its exact session name. The envelope's `payload` is the
native request without `action`. Its `request_id` matches the outer request UUID.

Message responses have `schema: 1` and `message`, containing `id`, `run_id`,
`conversation_id`, `created_at`, monotonic `revision`, `state`, and `audio`.
Optional fields are `transcript`, `error_code` and `error`. Audio metadata uses
`request_id` equal to the message UUID, `index: 0`, `name: "voice.wav"`,
`mime: "audio/wav"`, `bytes` and `duration_ms`.

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
