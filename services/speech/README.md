# Optional speech host

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

The first adapter is GigaAM, pinned to upstream commit
`7447938d791c4f3e643386ee22c33777004293a5`. Its reviewed short-audio API supports
`v3_e2e_rnnt`, `v3_e2e_ctc`, `v3_rnnt`, `v3_ctc`, `multilingual_ctc` and
`multilingual_large_ctc`. Russian dictation starts with `v3_e2e_rnnt`, including
punctuation. The multilingual alternatives can be advertised as additional host
profiles after explicitly preparing their weights. Adapters are first-party
Python modules implementing `providers/base.py`; profile data cannot name an
executable, an import path or a client-supplied model file.

## Host installation

Keep the optional environment and weights outside the checkout. Python 3.11+
and a matching PyTorch/torchaudio installation are required. On a CUDA host,
install the appropriate reviewed CUDA PyTorch wheels first. The initial GPU
qualification uses PyTorch and torchaudio 2.10.0. The service itself has no
third-party dependencies.

```sh
python3 -m venv "$HOME/.local/share/zerus-speech/venv"
"$HOME/.local/share/zerus-speech/venv/bin/pip" install torch==2.10.0 torchaudio==2.10.0
"$HOME/.local/share/zerus-speech/venv/bin/pip" install './services/speech[gigaam]'
```

Explicitly download and verify the selected model using that environment:

```sh
"$HOME/.local/share/zerus-speech/venv/bin/python" -c \
  'import gigaam; gigaam.load_model("v3_e2e_rnnt", device="cuda")'
```

Upstream verifies the checkpoint checksum on load. The service requires both the
checkpoint and the E2E tokenizer to exist before accepting a recording. Model
files are trusted code/data installed by the computer's owner; clients cannot
upload or select arbitrary checkpoint paths. The normal inference path uses the
prepared cache and does not install packages.

Create `$HGS_CONFIG_DIR/speech.json` (default `~/.config/hgs/speech.json`):

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
      "device": "cuda"
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

Run the service directly, or explicitly install the Linux user service:

```sh
"$HOME/.local/share/zerus-speech/venv/bin/python" -m zerus_speech
python3 services/speech/install-user-service.py \
  --python "$HOME/.local/share/zerus-speech/venv/bin/python" --start
```

The first command runs in the foreground; use one launch method at a time. The
installer only enables `zerus-speech.service` when `--start` is supplied. After
editing profiles, restart that service to reload the configuration. Package
installation itself never starts or restarts a service. Native agents, tmux and
DeepSeek processes are independent of this service. Other Unix hosts may run it
with their existing supervisor or use a configured remote GPU computer.

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
```

The synthetic fixture adapter is only registered with the hidden `--test-mode`
flag, never by configuration alone. Contracts exercise persistence, duplicate
and conflicting identities, restart uncertainty, cancellation, limits, cleanup,
analytics, PCM segmentation and the real CLI/socket boundary without a model,
GPU, network or native agent. Actual recognition must additionally be checked
with the installed GigaAM model on the chosen host.
