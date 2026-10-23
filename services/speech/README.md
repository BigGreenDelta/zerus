# Optional speech host

For an independently deployed server with an optional OpenAI-compatible HTTP
API, see [Standalone speech HTTP server](../../docs/speech-server.md). It needs
no desktop application or `hgs`. HTTP listening is opt-in; the existing private
Unix-socket service and the lightweight HTTP client remain available separately.

Zerus records on the client and recognizes speech on an explicitly selected
computer. This optional Python service owns model configuration, model memory,
the inference queue and private job receipts. `hgs speech --json` is its bounded
JSON interface; remote desktop clients use the existing configured SSH route and
Android uses the mobile connector/relay. Selecting a profile never downloads or
loads a model. Nothing falls back to a cloud provider automatically.

Client **Send voice** uses the conversation host's durable voice-message worker
to orchestrate this service and native delivery. That worker retains original
audio and transcript for synchronized playback; this ASR service keeps its
existing short-lived jobs. See [persistent voice messages](../../docs/voice-messages.md).

The local adapter is GigaAM, pinned to upstream commit
`7447938d791c4f3e643386ee22c33777004293a5`. Its reviewed short-audio API supports
`v3_e2e_rnnt`, `v3_e2e_ctc`, `v3_rnnt`, `v3_ctc`, `multilingual_ctc` and
`multilingual_large_ctc`. Russian dictation starts with `v3_e2e_rnnt`, including
punctuation. The multilingual alternatives can be advertised as additional host
profiles after explicitly preparing their weights. Adapters are first-party
Python modules implementing `providers/base.py`; profile data cannot name an
executable, an import path or a client-supplied model file.

The `openai_http` adapter sends a WAV file to an explicitly configured
OpenAI-compatible transcription server. The base URL includes its API prefix,
for example `https://asr.example/v1`; the adapter appends
`/audio/transcriptions`. It sends multipart `file`, `model`, optional ISO 639-1
`language`, and `response_format=json`, and expects a bounded JSON `text` string.
An optional API key becomes a Bearer header on the processing computer. This
does not require an OpenAI account or SDK, and does not imply that an arbitrary
GigaAM server implements this protocol. The existing local service remains a
private Unix-socket service; it is not automatically exposed over HTTP.

HTTPS verifies the server certificate. Explicit `allow_insecure_http: true`
permits plain HTTP only when the actual connected address is private or loopback.
The adapter resolves once, connects to that checked address, ignores ambient
proxy configuration and never follows redirects. A recognition attempt has a
60-second absolute deadline, bounded DNS/connect work and a 64 KiB response
limit. A cancelled or ambiguous POST is never retried automatically. Endpoint
response bodies, credentials and raw network exceptions do not enter public
errors or analytics.

## Managing profiles in Zerus

Desktop and Android Voice settings can add, edit, remove and explicitly test
profiles on a selected computer, without opening a conversation. Configuration
management works even if that computer's optional inference service is stopped
or its GigaAM packages are absent. Saving a profile does not install packages,
download weights, start a service or run recognition. The processing computer
must run this service before its profiles can accept audio.

On first management access, the CLI imports existing `speech.json` profiles
into `$HGS_CONFIG_DIR/speech-profiles/profiles.sqlite3` (default
`~/.config/hgs/speech-profiles/profiles.sqlite3`). This private child directory
is mode 0700 and the database is mode 0600; the surrounding configuration
directory's permissions stay unchanged. Existing profile IDs and private model
cache paths survive import. This database then owns profiles and credentials;
later edits to the old JSON `profiles` array do not overwrite managed settings.
Keep `speech.json` for service socket/state paths, limits and analytics settings.
Hosts without a managed store continue reading legacy JSON profiles.

Changes use a durable request UUID and an expected configuration revision.
Concurrent edits conflict instead of overwriting one another. Removed IDs are
retired, so an old selection cannot silently identify a different endpoint.
API keys are write-only: Keep, Replace or Remove are explicit choices and reads
return only whether a key is configured. Android seals replacement keys to the
selected host before relay transport; desktop sends them through its configured
SSH channel. See the [management contract](../../docs/speech-providers.md).

Discovery reads only configuration and local runtime metadata. An explicit Test
submits a short synthetic sound to the saved exact profile revision and can load
an already installed local model or contact the configured HTTP server. It never
records the microphone, sends a person's saved audio, returns the synthetic
transcript or increments voice-usage counters. Test status remains recoverable
by its original UUID; a lost acknowledgement does not repeat recognition.

The service observes committed profile changes on subsequent discovery and
submission without restart. New clients pin the selected profile revision in
their recorded preview. A changed revision is refused before inference or HTTP
upload; the original recording remains available for explicit review. Already
accepted ASR jobs use their immutable configuration snapshot even if the profile
is edited or removed. Legacy unversioned requests retain their previous behavior.

## Host installation

Use the repository setup guide for your selected provider:

- [Local GigaAM](../../docs/speech-setup.md#local-gigaam): FFmpeg, the reviewed
  PyTorch/torchaudio pair, pinned GigaAM dependencies and explicit model/device
  preparation on the processing computer.
- [OpenAI-compatible server](../../docs/speech-setup.md#openai-compatible-server):
  an existing transcription API and only the lightweight base `services/speech`
  package on the processing computer; no GigaAM, PyTorch, FFmpeg or weights.

The [shared host setup](../../docs/speech-setup.md#prepare-the-processing-computer)
requires Python 3.11+ on a supported Unix host, including Linux and macOS. Both
adapters use the private service under the same user/configuration as the Zerus
CLI or connector. The guide includes source checkout, foreground startup and the
optional Linux user-service installer. Opening help or saving a profile performs
none of these actions.

## Advanced host configuration

Profiles saved through Zerus need no `speech.json` file. The service uses its
defaults and reads the managed profile store. As an optional bootstrap or for
custom service paths/limits, create `$HGS_CONFIG_DIR/speech.json` (default
`~/.config/hgs/speech.json`). Preserve an existing file. Bootstrap profiles remain
authoritative only until first management access imports them:

```json
{
  "schema": 1,
  "analytics_enabled": true,
  "profiles": [
    {
      "id": "gigaam-ru",
      "name": "Russian GigaAM v3",
      "adapter": "gigaam",
      "model": "v3_e2e_rnnt",
      "language": "ru",
      "device": "cpu"
    }
  ]
}
```

`download_root` may select a private model cache (default `~/.cache/gigaam`).
`state_dir` defaults to `$XDG_STATE_HOME/hgs/speech`, or
`~/.local/state/hgs/speech`. `socket_path` defaults to `state_dir/service.sock`.
Custom state and socket paths must be absolute; their directories must belong
to this user with mode 0700. The socket is mode 0600. Configuration remains on
the processing computer and is not part of Swarm configuration synchronization.

For startup and persistent-service commands, see
[Start the speech service](../../docs/speech-setup.md#start-the-speech-service).
Managed profile edits take effect without restarting the service. Changes to
socket/state paths, limits or analytics settings require a service restart.
Native agents, tmux and DeepSeek processes are independent of this service.

## JSON interface

Every response has `schema: 1`. Exactly one JSON request is read from stdin:

```sh
printf '%s\n' '{"action":"providers"}' | hgs speech --json
printf '%s\n' '{"action":"stats"}' | hgs speech --json
```

- `providers` returns `profiles`, each with `id`, `name`, `adapter`, `model`,
  `language`, `available`, optional `reason` and `max_duration_ms: 120000`.
  An uninstalled/stopped service returns an empty list and an availability reason.
- `submit` requires `request_id` (canonical lowercase UUID), `profile_id`, and
  `audio: {format: "pcm_s16le", sample_rate: 16000, channels: 1, data_base64: ...}`.
  Audio is at most 120 seconds / 3,840,000 bytes. Its accepted `job_id` always
  equals `request_id`.
  Managed discovery adds `profile_revision`; new submissions may supply that
  positive integer to require the exact selected revision.
- Submit replies, including duplicates, omit transcripts. If the returned state
  is already `completed`, fetch the text with `status`; only status carries text.
- `status` and `cancel` require `job_id`. Job replies include `state`
  (`queued`, `running`, `completed`, `failed`, `cancelled`, `uncertain`), optional
  `text` and `error`. Digital silence completes with empty text. A lost submit
  reply can be reconciled with `status(request_id)` without resending audio.
  Cancel replies omit transcripts even when the job already completed, keeping
  durable mutation receipts free of dictated content.
- `stats` returns `enabled`, `retention_days: 90`, `days` and `totals`. Each daily
  entry has UTC `day`, `profile_id`, `submitted`, `completed`, `failed`,
  `cancelled`, `uncertain`, `audio_ms`, `processing_ms`, `queue_ms` and typed
  `error_codes` counts. Totals contain the same counters without day/profile.

The request limit is 5,130,000 bytes; response limit is 1 MiB. Four concurrent
IPC connections and four accepted jobs (running plus queued) are allowed by
default. `max_queue` can be set from 1 to 16. Only one model and one inference
run reside in memory at a time. Cancellation prevents publication of the result;
an already running model call finishes before the next job starts. A slow model
call never blocks status, cancel, discovery or native agent operations.

Accepted UUIDs bind to an audio/profile fingerprint. Duplicate input returns
the same receipt and conflicting input is rejected. Restarted queued/running
jobs become uncertain and never run again automatically. Raw PCM is deleted when
the worker takes it into memory or the queued job is cancelled; temporary WAV
segments are deleted after each inference call. Startup removes abandoned PCM
and WAV files. Transcript results expire after 24 hours by default
(`result_ttl_seconds`, up to seven days), leaving content-free receipts that
prevent replay. Receipt storage defaults to 100,000 entries (`max_receipts`, up
to 1,000,000) and refuses new jobs when full rather than forgetting accepted IDs.
Increasing the limit preserves old receipts; deleting the database loses this
protection and is an explicit owner maintenance action.

Local aggregate counters are committed in the same SQLite transaction as each
job transition, so retries and polling do not count twice. They retain 90 UTC
days, capped at 2,880 daily/profile rows, independently of audio/transcript expiry.
There are no audio, transcripts, raw exception strings, machine/account IDs or
external telemetry in these aggregates. `analytics_enabled: false` stops new
increments; existing counters continue their normal retention. Terminal counters
are grouped under the submission day. `audio_ms` counts accepted recordings,
including later failures/cancellations, and processing time includes model load.

Long recordings are divided below GigaAM's 25-second limit. The built-in
segmenter removes digital silence, prefers quiet boundaries between 18 and 24
seconds and overlaps continuous speech by 400 ms, stitching exact matching
boundary words. It needs no Hugging Face token or pyannote downloads. This is a
conservative amplitude gate, not a neural voice detector; noisy audio and words
crossing forced boundaries still require real recording quality assessment.

The local socket is private to its Unix user. Mobile traffic also passes through
the trusted relay; API v1 has TLS transport and is not end-to-end encrypted.

## Verification

```sh
cargo test --locked speech
cargo build --locked
python3 -m unittest discover -s tests -p test_speech.py -v
python3 -m unittest discover -s tests -p test_speech_profiles.py -v
```

The synthetic fixture adapter is only registered with the hidden `--test-mode`
flag, never by configuration alone. Contracts exercise persistence, duplicate
and conflicting identities, restart uncertainty, cancellation, limits, cleanup,
analytics, PCM segmentation and the real CLI/socket boundary without a model,
GPU, network or native agent. Actual recognition must additionally be checked
with the installed GigaAM model on the chosen host.
