# Local and fleet analytics

Status: speech aggregates are implemented with the voice preview. The general
agent event ledger, fleet aggregation API and analytics views below are the
design for subsequent implementation, tracked by Beads epic `zerus-qc6w`.

Each computer owns its statistics. Desktop and Android request bounded summaries
through existing authorized connections and combine them by stable computer
identity. The relay transports requests and results; it does not collect an
independent copy of product analytics. No analytics endpoint receives audio,
messages, answers, command arguments, filesystem paths or account email addresses.

## Speech statistics available now

The optional speech service maintains a separate daily aggregate table in its
private SQLite database. `hgs speech --json` with `{"action":"stats"}` returns
schema 1, `enabled`, `retention_days: 90`, daily rows and totals. Rows identify a
UTC day and configured profile ID and contain:

| Field | Meaning |
| --- | --- |
| submitted | Accepted unique inference jobs |
| completed, failed, cancelled, uncertain | Jobs reaching each terminal state |
| audio_ms | Audio duration accepted for processing |
| queue_ms | Recorded wait before processing |
| processing_ms | Recorded processing time |
| error_codes | Counts by fixed error category |

Counters change transactionally with job state. Repeated submissions with the
same identity and repeated status reads do not increment them. Audio duration
belongs to the processing host; a phone recording and its processing on a
computer are one submitted job, not two voice uses. Recordings cancelled before
submission are not represented in these host counters. Time totals alone do not
provide latency percentiles or user activity duration. Incomplete jobs can make
submitted exceed the sum of terminal outcomes.

Host configuration `analytics_enabled: false` stops new aggregate writes.
Existing rows age out within the retention period; disabling collection does
not erase previous rows. Aggregates survive the shorter audio and transcript
lifetimes. The service keeps transcript results for 24 hours by default;
the relay removes completed speech status reads after 120 seconds and removes
submitted audio when the connector acknowledges acceptance. Durable mutation
identity receipts retain no audio. See [speech host](../services/speech/README.md).

## Common event model

The future collector belongs in `src/analytics/`, behind native lifecycle,
delivery and provider adapters. It consumes authoritative events as they happen
and updates private local storage away from UI and polling paths. It does not
reparse complete transcripts on each dashboard refresh. Speech aggregates can
be imported by source bucket revision or read alongside this store; they must
not be re-added to totals on every poll.

An event has a schema version, stable local event ID, computer UUID, producer,
event type, UTC observation time and a versioned capability/coverage descriptor.
Local deduplication may use exact session/run/conversation/turn/request identity;
those identifiers stay in the private ledger and need not appear in fleet
aggregate responses. Provider callbacks use their native event ID when present;
otherwise a reviewed composite identity prevents reconnects or replayed hooks
from counting twice. Native subagents have distinct producer identities and
remain distinguishable from user turns.

The initial event families are turn started/completed/interrupted, input or
approval requested/resolved, typed provider failure, input delivery outcome,
recovery attempt/outcome and usage observation. Session creation and resume are
separate from turns. A process existing or consuming CPU does not establish a
working agent. Queued input and background shell jobs remain separate activity
categories. Unknown native state remains unknown.

Record durations using monotonic time within a process lifetime and UTC only for
bucket placement. A restart or observation gap closes known coverage and starts
a new interval; it must not turn an offline night into measured work or waiting.
Intervals crossing midnight are divided between UTC daily buckets. Views may
present local dates only when the requested time range preserves those semantics.

## Measures and accuracy

| Area | Measures | Evidence required |
| --- | --- | --- |
| Agent work | Started/completed/interrupted turns, confirmed work duration | Reviewed native lifecycle events |
| Waiting | Approval/input wait duration and unresolved count | Matching request and resolution identities |
| Delivery | Confirmed, failed and uncertain submissions | Durable local delivery receipts |
| Recovery | Attempts, successes, exhausted budgets and outcomes | Recovery worker events |
| Reliability | Failures by provider and stable category | Typed errors, never text keyword guesses |
| Usage | Input/output/cache/reasoning token deltas where supported | Provider-reported values and their scope |
| Voice | Accepted audio duration, outcomes, queue/inference latency | Speech job state transitions |

Store usage counter scope and reset epoch. Convert cumulative observations to
deltas only within the same exact source identity and counter epoch. Repeated
snapshots, resumes, archives and native child reports must not double-count a
conversation. Account quota percentages are gauges, not token totals. Unknown
token components and unknown charges remain absent. Monetary costs require an
authoritative bill or an explicitly versioned pricing estimate clearly labeled
as such; token totals alone are insufficient.

Daily latency distributions use fixed versioned histogram boundaries, count and
sum. Fleet histograms can then be merged; per-machine percentiles cannot be
averaged. Every summary includes coverage start/end, gaps, unsupported measures,
source revision and freshness. A partial day is marked partial.

## Storage and collection controls

Use private local SQLite with bounded writes and indexed metadata. Proposed
defaults are 14 days for content-free dedup/event metadata and 90 days for daily
aggregates, matching speech. Bounds also apply to row counts and bytes. Record
an explicit history boundary after purge or quota exhaustion rather than silently
claiming uninterrupted collection. Settings should expose collection enablement,
retention and an explicit purge action with its scope. Disabling collection and
purging history are distinct operations. Durable native input no-replay receipts
remain outside analytics and cannot be removed by analytics cleanup.

Diagnostics are separate: bounded local component logs and optional crash
artifacts, with fixed error codes and build/provider versions. No recording,
transcript, key, raw provider response or private configuration enters aggregate
errors. Export diagnostics only after an explicit user action and a review of the
exported fields. Existing Android Firebase Analytics and crash-report controls
remain independent and must not be used as an implicit upload path for this data.

## Fleet reads and presentation

The proposed read API is `hgs analytics --json` with a bounded UTC date range,
optional grouping and schema version. Local execution reads one computer;
desktop fanout uses verified configured SSH peers and Android uses new
capability-gated relay operations. Reads never enable collection or mutate agent
state. The mobile route retains the existing authorized one-hop boundary.

Replies contain stable `machine_id`, source epoch/revision, generation time,
requested range, coverage and aggregate buckets. A bucket key is machine UUID,
source, schema/bucket version, date and dimensions. Clients select one response
per machine and replace a cached source revision; they do not sum repeated
replies or different routes to the same machine. A reset cannot revive cached
pre-purge totals. Unreachable computers display last known values with age and
are excluded from claims of a complete current summary.

Desktop and Android should offer a common overview with per-computer/provider
drilldown, completed work, known waiting, typed failures and supported usage.
Voice views show accepted minutes, outcomes and latency with units. Counts of
sessions, turns, background jobs and speech jobs must remain distinct. All totals
identify the selected computers, date interval and missing coverage. Local data
can be useful with every remote computer offline.
