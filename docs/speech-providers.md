# Speech provider management

Speech profiles belong to a processing computer. Desktop and Android manage that
computer's profiles from Voice input settings; selecting a default remains a
separate client preference. Creating, editing or deleting a profile never changes
an already recorded preview, sends audio, installs packages or downloads weights.
Local GigaAM and an OpenAI-compatible transcription server are separate adapters.

For installation and configuration, follow the provider-specific user guide:
[Local GigaAM](speech-setup.md#local-gigaam) or
[OpenAI-compatible server](speech-setup.md#openai-compatible-server). Both use the
selected computer's private speech service; only local GigaAM needs model packages
and weights. The sections below specify the host and transport contracts.

The HTTP adapter uses a base URL including the API prefix (for example
`https://speech.example.net/v1`) and appends `/audio/transcriptions`. It posts a
mono 16 kHz PCM WAV as multipart `file`, with `model`, optional `language`, and
`response_format=json`, and reads a bounded JSON `text`. An optional Bearer key
belongs to the processing computer. A chat-completions endpoint alone is not a
transcription service. See the [official transcription contract](https://developers.openai.com/api/reference/resources/audio/subresources/transcriptions/methods/create).
Redirects and ambient proxies are disabled. HTTPS is the normal transport;
explicit `allow_insecure_http` permits local/private-network HTTP. URLs cannot
contain credentials, query strings or fragments. Recognition POSTs are never
retried automatically.

## Host API

All operations use `hgs speech --json`. The help capability marker is
`Speech profile management v1`, independent of legacy speech support. Requests
and responses use schema 1. Configuration revisions are nonnegative integers;
machine and request identities are canonical nonzero UUIDs.

| Action | Request fields | Result |
| --- | --- | --- |
| `config_list` | none | `machine_id`, `revision`, `service`, `adapters`, `profiles`, `credential_key` |
| `profile_upsert` | `request_id`, `expected_machine_id`, `expected_revision`, `profile`, `credential` | Operation receipt |
| `profile_remove` | `request_id`, `expected_machine_id`, `expected_revision`, `profile_id` | Operation receipt |
| `profile_test` | `request_id`, `expected_machine_id`, `profile_id`, `expected_profile_revision` | Operation receipt |
| `config_status` | `operation_id`, `expected_machine_id` | Original operation receipt |

A profile has immutable `id`, editable `name`, `adapter`, `model`, and `language`.
Language is an optional two-letter lowercase ISO 639-1 code, such as `ru`.
Local `gigaam` profiles additionally have `device`; remote `openai_http` profiles
have `base_url` and `allow_insecure_http`. Lists add `revision`,
`credential: {configured: boolean}`, `available`, optional `reason`, and
`max_duration_ms`. They never return a credential value. Service and adapter
metadata distinguish configuration availability from missing inference runtime
or model weights.

Credentials use an explicit `mode`: `keep`, `replace`, or `remove`. Only replace
has a new value. Direct local/SSH calls may use `value`; mobile calls must use the
sealed envelope below. An unchanged empty editor must not erase a stored key.

Operation receipts have `schema`, `machine_id`, `request_id`, and `state`
(`completed`, `failed`, `running`, or `uncertain`), plus optional `revision`,
`profile_id`, `profile_revision`, `error_code`, and sanitized `error`. Receipts
contain no raw credential, profile configuration, synthetic transcript, remote
response body or exception. The same UUID and request fingerprint returns its
original receipt before revision checking; different input under that UUID is
rejected. A `revision_conflict` leaves the editor intact for explicit refresh.
Unknown outcomes are reconciled with `config_status`, never a fresh mutation ID.

The host privately stores managed profiles, credentials and operation receipts
transactionally. Existing schema-1 `speech.json` profiles are imported once,
preserving IDs and advanced model-cache fields. `speech.json` retains service
paths and limits; the managed store becomes the profile authority. Management
does not require a running inference service. Accepted recognition jobs retain
the exact profile revision and credential snapshot, so later edits or removal
cannot redirect accepted audio. Discovery additionally returns optional
`profile_revision`; new clients pin it in retained previews and the
`voice_submit.speech` route. Delayed recognition carries the same revision into
ASR acceptance, which rejects a changed profile before uploading audio. A retained
preview requires explicit reselection to adopt a changed revision. Older clients
without the optional field remain compatible. Explicit profile Test uses a saved exact revision
and host-generated synthetic audio, never a microphone or retained voice draft;
it cannot deliver input to an agent or increment ordinary voice-volume counters.

## Mobile transport

These operations have an empty session and target an authorized processing
computer, independently of the currently selected conversation:

| Relay operation | Host action |
| --- | --- |
| `speech_config` | `config_list` |
| `speech_profile_upsert` | `profile_upsert` |
| `speech_profile_remove` | `profile_remove` |
| `speech_profile_test` | `profile_test` |
| `speech_config_status` | `config_status` |

Mutation/test payload `request_id` must equal the outer relay request UUID. Reads
and status are short-lived; mutations retain content-free durable receipts while
their completed payloads are erased. Relay and target capabilities must both
advertise the operation. Clients bind replies and dialogs to the exact physical
machine and authorized route. Removing a profile requires explicit confirmation
and leaves existing selections visibly unavailable, without a fallback.

### Sealed credentials

Mobile credentials are encrypted for the processing computer before entering
the durable relay queue. The list response includes:

```json
{
  "credential_key": {
    "key_id": "<lowercase SHA256 hex of host SPKI DER>",
    "algorithm": "P256-HKDF-SHA256-A256GCM",
    "public_key_spki": "<base64 DER>"
  }
}
```

Replacement uses `credential: {mode: "replace", sealed_value: {...}}`, with
exact fields `key_id`, `algorithm`, `ephemeral_public_key` (base64 DER SPKI),
`nonce` (base64 12 bytes), and `ciphertext` (base64 ciphertext followed by the
16-byte GCM tag). Both keys use uncompressed secp256r1 SPKI, at most 128 bytes.
Plaintext is a bounded UTF-8 key of 1–4096 bytes. Base64 is canonical standard
encoding. The host private key stays private on that computer.

ECDH yields the standard 32-byte x-coordinate secret. HKDF-SHA256 uses
`SHA256(host SPKI DER)` as salt, the context below as `info`, and produces 32
bytes. AES-256-GCM uses the same context as additional authenticated data. The
context is UTF-8, LF-joining these strings with **no trailing LF**:

1. `zerus-speech-credential-v1`
2. `expected_machine_id`
3. `request_id`
4. `profile_upsert`
5. Decimal `expected_revision`
6. Profile `id`
7. Profile `name`
8. Profile `adapter`
9. Profile `model`
10. Profile `language`
11. Profile `device`, or empty
12. Profile `base_url`, or empty
13. `true` or `false` for `allow_insecure_http`

Fields reject C0 and DEL controls. Binding the full profile prevents substituting
another endpoint when conveying a sealed replacement. A form keeps its key only
in memory and clears it on success, dismissal or host change. Saved keys never
enter client preferences, clipboard helpers, analytics or diagnostics.
