# Shared session notification preferences

Mute is owned by the computer running the conversation and applies to every
connected desktop and phone. It suppresses notification delivery and neutralizes
attention/unread presentation; it does not acknowledge a reply, answer a
question, change attention or Active membership, stop work, or change an agent's
permissions. Working and operational errors remain available. Child notifications
follow their parent's preference; child and archived controls are unavailable.

## ABI v1

The installed CLI advertises the exact help marker `session-notifications-v1`.
An old CLI must not receive this unknown command: remote forwarding probes that
marker before forwarding stdin. The connector operation is
`session_notifications`. A missing capability or missing/null preference is
unknown, never permission to fall back to a device-local mutation.

```
hgs [@host] notifications SESSION --json
```

JSON stdin contains exactly these four fields:

```json
{"run_id":"run-1","conversation_id":"conversation-1","muted":true,"expected_revision":0}
```

The run ID must be nonempty and at most 128 UTF-8 bytes; the conversation ID
is nonempty and at most 256 bytes. Neither permits control characters. `muted` is a JSON boolean. The revision is a JSON integer from 0
through 9223372036854775807; strings, floating point numbers, nulls, missing
fields and extra fields are refused. Input is limited to 8 KiB. Session names
are bounded to 2048 bytes. No dry run or child/archive selector is accepted.

`hgs ls --json` and parent `hgs inspect` add `notification_preferences`:

```json
{
  "conversation_id":"conversation-1",
  "muted":false,
  "revision":0,
  "suppressed":{
    "reply_id":null,
    "attention_signature":null,
    "optional_question_ids":[],
    "main_attention":null,
    "child_attention":{}
  }
}
```

A known new conversation has revision 0 and `muted:false`. Corrupt/unreadable
preference storage yields null in a listing and refuses writes; it never resets
to an unmuted preference. Untracked rows yield null.

A successful response exits 0 and has exactly the current identities and
preference:

```json
{"ok":true,"applied":true,"run_id":"run-1","conversation_id":"conversation-1","preferences":{"conversation_id":"conversation-1","muted":true,"revision":1,"suppressed":{"reply_id":null,"attention_signature":null,"optional_question_ids":[],"main_attention":null,"child_attention":{}}}}
```

A matching revision with the same requested value returns `applied:false` and
does not write, increment the revision, or capture a baseline. An old revision
always returns `ok:false,error:"conflict"`, even if the requested value already
matches. Changed run/conversation IDs return `error:"stale_identity"`. Both
refusals exit 1 and return the **current** `run_id`, `conversation_id` and
`preferences`, so a client must not attach that preference to its stale target.
An absent, archived, untracked or reused binding returns
`{"ok":false,"error":"not_found"}` with exit 1. Invalid input/storage may instead
produce the normal CLI diagnostic and no JSON receipt.

Every actual transition increments the revision. Exhaustion returns
`revision_exhausted`. Clients must apply the returned current preference; an
unknown delivery must refresh it before issuing a new CAS. A delayed offline
unmute cannot silently overwrite a newer mute.

## Suppression baseline and offline devices

Muting works even when a question index/provider is unavailable. It retains the
previous suppression baseline, initially empty. An actual unmute captures a
complete, bounded set of **current** notification edge identities. An unavailable
or over-limit baseline returns `baseline_unavailable` with the current preference
and no preference write. There is no trimming and no no-replay claim for an
incomplete snapshot. This permits an offline device which missed both transitions
to suppress the old current edges after reconnecting without marking them read.
Failed recovery states (`uncertain`, `blocked`, `exhausted`) have no independent
identity in this native-edge baseline. They therefore refuse an actual unmute
with `baseline_unavailable`, keeping the existing mute until recovery is resolved.

`suppressed` contains:

| Field | Type and normalization |
| --- | --- |
| `reply_id` | Current nonempty reply ID, or null. |
| `attention_signature` | Current canonical shared read-attention SHA-256, lowercase, or null. |
| `optional_question_ids` | Sorted unique IDs of all pending optional Codex questions whose answer is not submitted, at most 64. This is not the listing's newest-32 preview. |
| `main_attention` | Null, or `{run_id,phase,request_id,started_at}` for approval/input/error. Phase is the current raw phase; request ID is `attention_id`, or `""`. |
| `child_attention` | Object keyed by child ID, at most 256 actionable children. Values are `{phase,request_id,started_at}`. Hook profile aggregates are excluded. |

Phases are limited to 256 UTF-8 bytes without control characters. Request IDs
are limited to 65536 UTF-8 bytes, allowing LF separators and no other control
characters (DeepSeek joins native question IDs with LF). Other edge IDs are
limited to 256 UTF-8 bytes without controls; main run IDs use the 128-byte limit.

For children, phase is nonempty `display_state`, otherwise `state`; only
approval/input/attention is included. The request ID is nonempty `attention_id`,
otherwise `question_id`, otherwise `""`. `started_at` is a **JSON number or null**:
finite positive `attention_started`, with a child's finite positive
`last_event_at` as fallback. Main timestamps have no fallback. No timestamp is a
string or a client-clock cutoff. Match host identities exactly; when a request ID
is present it identifies the request, and timestamps are the fallback for an
edge without one. An actionable main or child edge with neither a request ID
nor a positive finite timestamp refuses unmute; unknown identities cannot
suppress every future unidentified request. A new reply/request after unmute
remains eligible. The real
read marks, optional-question pending/delivery state and attention fields remain
unchanged.

## Host storage and observation

Only the host writes `notifications.json` in the existing session state directory.
Version 1 stores `sessions[stable_owner][conversation_id]` preferences. A tracked
owner is its stable journal name. Native provider renames anchor their original
name before changing the label. Resume retains the same conversation preference;
a new conversation or fork defaults independently. At most eight conversations
per owner are retained. Capacity returns `capacity` rather than
discarding a mute or a revision that a delayed client may still hold. Each serialized preference is limited to 384 KiB when ASCII-escaped (ASCII
characters cost one byte, non-ASCII BMP characters six and non-BMP characters
twelve). This bound is checked before store mutation/persistence and on load;
a too-large unmute is `baseline_unavailable` with no change. The file
is limited to 16 MiB, atomically replaced under a separate notification writer
lock. It is never stored in or written through `read-marks.json`.

Admission reads the exact raw current binding while holding its existing writer
lock. It does not run rename recovery, archive reconciliation, native legacy
adoption, approval fitting, input, answer, read-mark, lifecycle or permission
actions. An interrupted rename or stale archive index is refused until an
ordinary refresh settles it. Archive admission uses only the existing read-only
metadata index, never an unbounded archive scan.

Baseline observation may read native panels, bounded provider/transcript status,
and maintain existing derived journal/question/history caches. OpenCode uses an
already-running service connection and GET-only observation, bypassing legacy
adoption. DeepSeek uses only resident `inspect` and `list` with matching host
generation; it never starts a host. Unavailable provider observations are
transient refusals, not permission to fabricate an empty baseline. An old
unadopted OpenCode binding must be adopted through its normal product path first.

The backend source tests cover strict input, CAS/no-op/stale identities,
unknown/over-limit baselines, exact suppression identities, bounds/exhaustion,
rename/resume/fork separation and preference-file persistence. They are private
synthetic fixtures; they do not qualify a real native agent or deployment.
