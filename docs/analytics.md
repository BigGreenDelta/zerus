# Local and fleet analytics

The local event ledger, bounded read API, desktop settings and Android views are
implemented in source. Independent acceptance and maintained rollout remain
outstanding. A source build does not establish that an installed computer or
gateway supports analytics; clients check capabilities before sending requests.

Each computer owns its statistics. **Agent analytics collection is off by
default**, including on first read. Desktop and Android request summaries over
existing authorized connections. The relay transports bounded requests and
results through its request journal; it has no independent analytics collector.
Speech statistics use their existing, separate store and collection setting.

## Privacy and collection

`src/analytics/` accepts fixed event types, provider/producer enums, numeric
measurements and exact native metadata identities. The private ledger stores
hashed deduplication/correlation identities; it never stores prompts, replies,
code, command arguments, paths, account labels, email addresses, raw errors or
transcript text. Identities are not derived from content. Aggregate replies expose
the stable computer UUID but no session, run, conversation, request or account
identity. Diagnostics additionally remove computer UUIDs and configured names.

Collection runs from native event, terminal delivery and recovery producers,
never from dashboard reads or UI polling. The Codex/Claude adapter incrementally
extracts reviewed metadata from bounded native JSON records. Such files can
contain text, but the adapter neither retains that text nor uses it for identity,
classification or diagnostics. It validates conversation identity and file
identity, refuses a final-path symlink, and retains a hashed file identity and
byte cursor instead of a path. Missing or conflicting evidence reduces coverage.

When collection is off, producers do not read native files for analytics or add
analytics events. A summary does not create storage, initialize a computer
catalog, enable collection, perform housekeeping or alter native state. A
computer without an existing valid stable UUID reports identity unavailable.
Analytics failures do not change the outcome of input delivery or native work.

## Local controls and retention

The private ledger is `$HGS_STATE_DIR/analytics/analytics.sqlite3`, under the
existing HGS state directory. Desktop **Settings > Private analytics** exposes
local collection enablement, retention from 1 through 90 UTC days, and explicit
purge. Shortening retention requires confirmation. Purge performs a fresh local
read, asks for confirmation and requires the same source epoch. Desktop controls
apply only to the local computer; remote CLI reads, remote views and Android
are read-only.

Disabling preserves retained aggregates. Toggling collection closes pending
intervals and usage baselines, establishes a history boundary and changes the
source epoch, so an offline/disabled interval is never counted as work. Purge
removes local agent aggregates, deduplication records, cursors, baselines and
pending intervals, changes the source epoch and reports `history_purged`. It
preserves the collection setting. It does not purge speech, native input receipts,
conversations or any other computer's history. Native no-replay receipts remain
outside analytics.

| Bound | Implemented limit |
| --- | --- |
| Event/dedup metadata admission and retention | 14 days; at most 200,000 events |
| Daily aggregates | Default 90 UTC days; configurable 1–90 |
| Pending intervals, usage epochs and native cursors | At most 10,000 rows in each table |
| Pending interval lifetime | At most 24 hours before expiry |
| SQLite database pages | 32,768 pages of 4 KiB, or 128 MiB; rollback journal space is additional and bounded by the transaction |
| Native extraction per invocation | At most 68 KiB plus one boundary byte, 64 records, and a 50 ms processing deadline |
| Summary request | 4 KiB JSON, inclusive range of 1–90 UTC days |
| Summary response | Fewer than 1 MiB, at most 1,080 daily buckets |

Native observers use nonblocking SQLite writes. The extraction deadline does
not bound OS file I/O or guarantee end-to-end hook latency. A first observation
starts near the tail rather than backfilling an entire transcript; bounded skips,
replacement files and interrupted coverage are explicit gaps.

Expiry uses indexed timestamps and capped background deletion batches, including
while collection is disabled. Expired daily buckets are excluded from reads
immediately even if physical cleanup awaits the worker. Event, usage and cursor
cleanup removes at most 1,000 expired rows per table per pass. The event admission
window is no longer than the deduplication lifetime, so old native replay cannot
become a new event after its tombstone expires. Events before a collection/purge
boundary or more than five minutes in the future are rejected. Quota exhaustion
does not silently evict recent deduplication identities to admit more events.

Daily queries use the `(day, provider, producer)` primary key and a hard row
limit. A single read transaction pins metadata and buckets to one source
revision. UI pagination is over that bounded snapshot, so incoming events do
not reorder an already displayed page; refreshing replaces the snapshot.

## CLI and mobile read contracts

First probe the selected executable's help for `hgs [@host] analytics --json`.
Older executables may interpret an unknown command as a session launch. The
command reads one JSON object from stdin and returns one schema-1 summary:

```sh
hgs analytics --json <<'JSON'
{"schema":1,"action":"summary","from":"2026-10-01","through":"2026-10-07"}
JSON
```

`hgs @configured-peer analytics --json` uses the same summary request over the
existing authorized SSH route. Remote forwarding enforces summary-only access;
configure/purge requests are rejected before accessing analytics state. Dates
must be canonical `YYYY-MM-DD`, from year 0001 onward. `through` is inclusive.
Reversed or over-90-day ranges, unknown fields, unknown actions and unsupported
schemas are rejected. There is no raw event-list endpoint or arbitrary grouping
expression.

Explicit local CLI configuration requires the epoch from a fresh summary:

```json
{"schema":1,"action":"configure","expected_epoch":"00000000-0000-0000-0000-000000000000","enabled":true,"retention_days":90}
```

The nil epoch in this example is valid only for the initial empty store; use the
actual returned epoch for an existing store. Both settings are required.
Configuration changes rotate the epoch. A local purge request has exactly these
fields, with `expected_epoch` replaced by the current summary's value:

```json
{"schema":1,"action":"purge","expected_epoch":"11111111-1111-4111-8111-111111111111","confirm":true}
```

A stale epoch rejects the mutation. Unknown delivery of a change requires a new
read and review, not automatic replay. Configure/purge replies contain a summary
for the latest 30 UTC days; summary requests always use their explicit range.

Mobile uses the existing authorized `/v1/requests` envelope with a computer
target, `session: ""`, `operation: "analytics_read"`, and exactly this payload:

```json
{"schema":1,"from":"2026-10-01","through":"2026-10-07"}
```

Both the gateway and selected computer must advertise `analytics_read`. The
connector supplies `action: "summary"`, retains the existing one-hop routing
boundary, bounds output and verifies machine/range identity. The mobile operation
does not accept configure/purge or a session target. Android additionally checks
request/receipt identity and parses the response off the UI thread. Neither
analytics transport nor views alter notifications or read marks.

## Summary schema and measures

| Field | Contract |
| --- | --- |
| `schema`, `source` | `1`, `"agent"` |
| `machine_id` | Canonical non-nil computer UUID |
| `source_epoch`, `revision` | Canonical UUID and nonnegative revision; replace a source snapshot on reset |
| `generated_at` | Numeric Unix seconds, including fractional seconds |
| `range` | Exact requested `from` and `through` dates |
| `collection` | Boolean `enabled`, `event_retention_days: 14`, `retention_days: 1..90` |
| `coverage` | Nullable Unix-second `start`/`end`, fixed gap/unsupported codes, `partial`, and optional per-provider coverage |
| `unresolved_waits` | Optional `{observed: integer, complete: false}` for retained waits observed within the latest 24 hours; not a complete live-session count or a total for the selected historical range |
| `buckets` | Daily rows ordered by `date`, `provider`, `producer` |

An absent store returns nil `source_epoch`, revision zero, collection disabled,
null coverage bounds, no buckets and all measures unsupported. An empty bucket
set or absent counter does not prove zero activity. Coverage bounds record first
and last accepted observations, not a guarantee of continuous coverage within the
requested range. The current adapters always report partial coverage. A
per-provider `supported` entry means at least one qualifying observation exists;
it does not promise every event, producer or token component is available.

Bucket dimensions are UTC date; provider `codex`, `claude`, `kimi`, `dsh`,
`opencode` or `other`; and producer `main` or `child`. Counters are optional
nonnegative exact JSON integers, bounded by 9,007,199,254,740,991:

| Family | Counter keys |
| --- | --- |
| Turns | `turn_started`, `turn_completed`, `turn_interrupted` |
| Known duration | `work_ms`, `approval_wait_ms`, `input_wait_ms` |
| Matched waits | `approval_requested`, `approval_resolved`, `input_requested`, `input_resolved` |
| Delivery | `delivery_submitted`, `delivery_failed`, `delivery_uncertain` |
| Recovery | `recovery_attempt`, `recovery_succeeded`, `recovery_exhausted` |
| Failures | `failure_authentication`, `failure_rate_limit`, `failure_quota`, `failure_policy`, `failure_network`, `failure_service`, `failure_unknown` |
| Usage | `input_tokens`, `output_tokens`, `cache_tokens`, `reasoning_tokens` |

`histograms.work_ms`, when present, contains `version: 1`, millisecond
`bounds: [100,1000,10000,60000,600000]`, six `counts`, `count`, and `sum`.
The final bin is overflow; counts sum to `count`. Whole interval samples belong
to the completion day while duration totals split across UTC midnight. Clients
may merge matching histograms or show means; they do not average machine
percentiles or invent percentiles from duration totals.

Fixed gap codes are `collection_disabled`, `history_purged`, `retention_boundary`,
`quota_exhausted`, `observation_gap`, `clock_discontinuity`, `missing_identity`
and `unsupported_provider`. Measure codes are `turns`, `work_ms`,
`approval_wait_ms`, `input_wait_ms`, `delivery`, `recovery`, `failures`, `tokens`,
`cost` and `background_jobs`. Clients present unrecognized coverage codes as
generic unknown information without echoing arbitrary server text.

## Accuracy and current adapter limits

Durations require matching correlation identity and monotonic clock generation
within the same native run/process lifetime. Codex live boundaries additionally
require an exact `turn_id` shared by the validated main hook and native record;
matching a hook name and nearby timestamp alone is insufficient. Historical
records discovered together do not establish elapsed work. A clock discontinuity, missing boundary
or expired interval yields unknown duration. A recorded approval/input wait
invalidates the overlapping work interval until work-resumption proof is
available, avoiding counting waiting time as work. Durations exceeding a day or
whose wall and monotonic elapsed times disagree by more than five seconds are
discarded. Monotonic observations are currently available on Unix; other platforms
can retain supported counts without manufacturing duration.

| Source | Implemented coverage and limitation |
| --- | --- |
| Codex native records | Partial main-producer turn lifecycle and conversation cumulative usage. Work duration requires the same exact turn ID in validated main hooks and native records, plus live process generation and matching monotonic boundaries. Current hooks can omit that ID, so work duration is largely unsupported unless this proof is present. Initial usage establishes a baseline; it does not backfill prior consumption. |
| Claude native records | Partial main-producer assistant end-turn counts and per-request usage. No complete native start/interruption or work-duration coverage is claimed. Request identity deduplicates repeated usage observations. |
| Native children | Codex/Claude native extraction does not collect child turns or usage. The schema can represent `child`, but its presence is not evidence of complete child coverage. |
| Kimi and hook waits | Only reviewed hooks with exact event/turn/tool identity and durable source timestamp qualify. Kimi queued `UserPromptSubmit` is not a turn start. Approval/input waits are largely unsupported in current native hooks when IDs or timestamps are absent. `QuestionAnswered` can use its durable numeric receipt time; a matching request is still required. |
| Other provider lifecycle | DeepSeek, OpenCode and unknown-provider hooks do not establish lifecycle/usage support. Independently supported delivery/recovery observations remain distinguishable. |
| Delivery | Current integration records terminal input receipt outcomes. It is not a count of every native or mobile protocol submission; schema support for `delivery_failed` does not establish a wired producer for every failure path. |
| Recovery | Durable recovery-worker transition identity, attempt number and timestamp establish attempts/success/exhaustion. Reason text is ignored. |

Cumulative token observations become deltas only within the exact native scope
and counter epoch. Counter resets, retired epochs and delayed observations cannot
revive already counted totals. Unknown token components stay absent. Cache tokens
are a component of usage, not an additional charge to add to input/output totals.
Account quota percentages are not token counters. No adapter infers monetary
cost, user activity, background jobs or work from process existence, CPU usage,
session status or text. Failures use fixed native error categories, never keyword
matching in prose. `cost` and `background_jobs` remain explicitly unsupported.

## Fleet views and diagnostics

Desktop **Settings > Private analytics** reads the local computer and configured
SSH peers. It offers UTC dates, computer/provider selection, daily rows in
50-row pages, known durations, typed failures and supported token values. Android
**Settings > Computer analytics**, before About, offers 7/30/90-day UTC ranges,
computer/provider/producer filters and a bounded lazy daily list. Both perform
I/O and parsing off the UI thread and keep bounded in-memory snapshots. Desktop
reads at most 32 configured routes sequentially. Android reads at most 32 unique
computers and three routes per computer, with two computers in flight. Omitted,
unavailable, unsupported and stale coverage is visible.

Both views pool compatible work histograms after computer deduplication. Android
does so for both
the selected overview and daily/provider rows. It adds each fixed bin, sample
count and duration sum, then derives the weighted mean from the combined sum
and count. Reported samples are explicitly partial; absent histograms do not add
zero-duration samples. A sum beyond the safe integer bound makes that projection
unavailable instead of wrapping or displaying a fabricated mean.

Fleet totals select one whole snapshot per stable computer UUID and source.
Repeated reads and multiple routes never add the same buckets twice. Within an
epoch, lower revisions cannot replace newer ones. A different epoch requires a
newer generation time; equal-time ambiguous resets are rejected. Accepted resets
replace all prior buckets, including an empty post-purge summary. Clients retain
up to 64 retired epochs per computer in memory and reject delayed replies from
those epochs even if their generation timestamp is newer. At that bound they
refuse further replacement rather than evict a tombstone. These guards last for
the view/cache lifetime, not across application restarts.

Last-known values retain their age and partial status. Desktop excludes stale
replies from current totals. Android labels its totals as known values including
explicitly marked last-known replies, and shows the current-reply count alongside
each computer's age. Neither presentation claims a complete fleet.
Changing the date range discards incompatible cached buckets. Raw responses,
remote stderr and arbitrary provider fields are not used as display or export
text. No background analytics upload is added.

Diagnostics require an explicit review followed by export/share. Desktop previews
allowlisted aggregate values with computer ordinals before saving a local JSON
file. Android previews a smaller health summary: build code, requested dates,
read states, bucket counts, collection/coverage flags and fixed codes. Android's
diagnostic excludes usage values as well as computer identities. Neither export
contains logs, transcripts, account details, native IDs or private configuration.
Android's existing Firebase Usage analytics and crash-reporting switches remain
independent; this view and export do not upload through Firebase.

## Separate speech statistics

The optional speech service maintains a daily aggregate table in its private
SQLite database. `hgs speech --json` with `{"action":"stats"}` returns schema 1,
`enabled`, `retention_days: 90`, daily rows and totals. The native response groups
by UTC day and configured profile ID. Analytics views discard profile IDs,
combine allowed numeric fields and filter to the selected UTC range.

| Field | Meaning |
| --- | --- |
| `submitted` | Accepted unique inference jobs |
| `completed`, `failed`, `cancelled`, `uncertain` | Jobs reaching each terminal state |
| `audio_ms` | Audio duration accepted for processing |
| `queue_ms` | Recorded wait before processing |
| `processing_ms` | Recorded processing time |
| `error_codes` | Counts by fixed error category |

Counters change transactionally with job state. Repeated submissions with the
same identity and repeated status reads do not increment them. Each processing
computer's selected speech response replaces its previous snapshot. Multiple
routes to that computer are not added together, and voice jobs are never added
to agent turn totals. A phone recording and its processing on a computer are one
submitted job. Recordings cancelled before submission are absent. Incomplete
jobs can make submitted exceed terminal outcomes. Accepted audio duration and
total queue/processing time have units; these totals do not supply percentiles.

Host configuration `analytics_enabled: false` stops new speech aggregate writes;
agent collection controls do not change it. Existing rows age out within the
retention period; disabling does not erase them. Aggregates outlive the shorter
audio/transcript lifetimes. Transcript results default to 24 hours; the relay
removes completed speech status reads after 120 seconds and submitted audio when
the connector acknowledges acceptance. Durable mutation identity receipts retain
no audio. See [speech host](../services/speech/README.md).
