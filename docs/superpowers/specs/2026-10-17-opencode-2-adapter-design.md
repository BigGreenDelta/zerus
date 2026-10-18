# OpenCode 2 native adapter design

Date: 2026-10-17, revised 2026-10-18 after independent review. Beads epic:
`zerus-pl2u`. Follow-up feature: `zerus-dg1g`
([providers and models](2026-10-17-opencode-providers-design.md)).

## Goal

The owner wants to pick OpenCode providers and models from Zerus, see what is
connected on each computer, and start sessions with models such as
`opencode/glm-5.3-flash-free` or `opencode/deepseek-v4.1-flash-free` without
opening OpenCode's own interface. The current adapter is pinned to OpenCode
1.18.35, whose API cannot connect providers reliably (verified below), while
upstream has moved to OpenCode 2. This design moves the native adapter to
OpenCode 2 and drops OpenCode 1 support. The provider and model experience is a
separate design that builds on this adapter.

Success means: on a computer with OpenCode 2, New session (desktop, Android,
iOS) and `hgs opencode` start a native OpenCode conversation with any model the
local OpenCode service offers for that project; Activity shows its history and
live state; Stop, messages, voice transcripts, model and effort changes and
OpenCode questions work in phase 1; attachments, live child sessions,
compaction, fork and permission modes follow in phase 2; and the conversation
stays usable from the OpenCode TUI at the same time.

## Owner decisions (2026-10-17)

- Stop supporting OpenCode 1.18.35 and move to OpenCode 2. OpenCode 2.0.32 was
  installed on arch (AUR `opencode-beta`) and mac (verified npm binary in the
  install-script location) on the same day, before this adapter exists.
- Zerus connects to OpenCode 2's shared per-user background service, the one
  the OpenCode TUI, CLI and desktop use. OpenCode owns the service lifecycle.
- Version policy exception for OpenCode only: accept any OpenCode 2.x at or
  above the reviewed minimum when the running service declares every operation
  and field Zerus uses; otherwise report an explicit incompatibility. A daily
  canary qualifies the newest release. Codex and Claude Code keep exact pins.
- Scope beyond parity: attachments and effort, live child sessions, compaction
  and fork, and Zerus permission modes.

## Verified facts

Probed on 2026-10-17 and 2026-10-18 with isolated homes and fixture
credentials only, and checked against upstream source at tag `v2.0.32`.

OpenCode 1.18.35:

- A running host keeps the models.dev catalog and configuration for the life of
  the process; only a restart exposes new models or providers.
- Credentials written through `PUT /auth/{id}` reach `auth.json`; the v2-style
  `/api/integration/{id}/connect/key` stores credentials that providers ignore.

OpenCode 2.0.32 (`/openapi.json`, 119 paths):

- Removed: `/global/health`, `/api/session/{id}/history`,
  `/api/session/{id}/question`, `/permission`, `/question` and every legacy
  `/session/...` route.
- Sessions: `GET /api/session` (`parentID`, `directory`, `cursor`, `limit`,
  `order`); `POST /api/session` with optional client `id` (`^ses`; posting
  the same ID again returns the existing session), `parentID`, `title`,
  `agent`, `model`, `location`, `metadata`, `permissions`;
  `GET|PATCH|DELETE /api/session/{id}` (`PATCH` replaces the whole
  `metadata` object and the whole `permissions` list);
  `/api/session/active`.
- Messages: `POST /prompt` with optional client message `id` (`^msg_`;
  posting the same ID again returns the original admission while it is still
  in `/inbox` or history; once `DELETE /inbox/{id}` removed it, the same ID is
  admitted again as a new message), `text`, `files`
  (`uri`, `name`; at most 20 MiB each), `delivery` (`steer` or `queue`;
  `steer` when omitted) and `resume`. A queued prompt is listed in `/inbox`
  and `GET /message/{id}` returns 404 until OpenCode runs it; queued items
  stay pending after `/interrupt`. `/message` pages by cursor.
- `/interrupt`; `/model` (`Model.Ref`: `providerID`, `id`, optional
  `variant`); `/compact` (client ID); `/fork` (only `before`; no client ID;
  400 for an empty session); `/form`, `/form/{id}/reply`
  (`{"answer": {<key>: value}}`); `/permission`, `/permission/{id}/reply`
  (`once`, `always`, `reject`; a reject rejects every pending request of that
  session); `/inbox` and `DELETE /inbox/{id}` (it deletes the pending row and
  keeps no tombstone; 204 even when the item had already left the inbox, 404
  only for an unknown session).
- Catalog routes (`/api/model`, `/api/provider`, `/api/integration`) take a
  `location`; a project `opencode.json` adds providers for that folder only.
  Their responses include provider `settings`, `headers` and `body`, which can
  contain API keys and authorization headers; `/api/credential` returns raw
  keys and OAuth tokens.
- Forms replace questions. Field types: `string` (format, length, pattern,
  and `options` with `custom` for OpenCode's question tool), `number`,
  `integer`, `boolean`, `multiselect` (options, `custom`, min and max
  items) and `external` (URL). Fields can be `required`, `hidden` and
  conditional (`when`: a list of key/`eq`|`neq`/value conditions).
- Permission rules merge agent rules and then session rules; the last matching
  rule wins, so a session `*` allow overrides configured denies.
- Subagents run in foreground or background child sessions of the same
  service.
- The background service registers in
  `$XDG_STATE_HOME/opencode/service.json` (default `~/.local/state`, also on
  macOS; `service-<channel>.json` for channels other than latest, dev, beta
  and next). The file holds `id`, `version`, `url`, `pid` and an optional
  `password` (basic user `opencode`), mode 0600. Readiness is HTTP 200 from
  `GET /api/info`. A service configuration can be `disabled`.
- Every OpenCode CLI command that uses the service (including
  `opencode service start`, `opencode models` and `opencode auth export`
  without `--standalone`) runs upstream `Service.ensure`, which terminates a
  registered service after three 2-second `/api/info` timeouts and starts the
  new service with the caller's environment, in the caller's process group
  and cgroup.
- OpenCode 2 migrates OpenCode 1 sessions, including the 1.x native-API store
  the current adapter used, into its own store with the same session IDs;
  `/api/experimental/migration/v1` reports progress.
- Connecting a key through `/api/integration/{id}/connect/key` exposed the
  provider's models immediately; configuration file edits appeared without a
  restart. Keyless OpenCode free models include `glm-5.3-flash-free`.
- On the real arch and mac upgrades, the existing Z.AI Coding Plan key from
  OpenCode 1 remained available to OpenCode 2.

## Non-goals

- OpenCode 1 compatibility beyond adopting migrated sessions.
- Stopping, restarting, replacing or upgrading the OpenCode service.
- Provider connection, model favorites and catalog search
  ([providers design](2026-10-17-opencode-providers-design.md)).
- Embedding OpenCode's web interface as Native UI, Terminal, Pause/Resume,
  archive, and sending messages to child sessions.
- Windows hosts. OpenCode remains unavailable on Windows computers as today.

## Phases

- Phase 1: service connection and compatibility gate, OpenCode 1 removal and
  migration, launch, send (text and voice), history, status, Stop, model and
  effort (desktop, Android, iOS), OpenCode questions and permissions.
  This alone lets the owner start `opencode/glm-5.3-flash-free` from Zerus.
- Phase 2: live child sessions and `Background`, attachments, compaction,
  fork, permission modes, forms beyond question-tool fields.

Each phase ships as its own pull request with native qualification.

## Architecture

The adapter stays a Rust module of `hgs` (`src/state/opencode.rs`, split into
focused files as it grows) with a small blocking HTTP client, as today.
Calling `opencode api` per request was rejected (a process per call and it
ensures the service, see Service lifecycle); a Node sidecar using
`@opencode/client` adds a runtime dependency on every host.

### Service connection

1. Locate the registration file as upstream does: `XDG_STATE_HOME` (default
   `~/.local/state`) plus `opencode/service.json`, or
   `service-<channel>.json` for the channel the installed CLI reports.
2. Parse `id`, `version`, `url`, `pid` and optional `password`. Require a
   loopback HTTP `url` and a live `pid`. Never log or forward the password,
   URL or file contents.
3. `GET /api/info` with basic authentication within 2 seconds. Readiness is
   HTTP 200 with a version at or above `OPENCODE_MINIMUM` (2.0.32).
4. Fetch `/openapi.json` once per service identity (`id`, `pid`, `version`,
   `url`) and check the required operation table: method, path, the request
   fields Zerus sends and the response fields it reads. Cache the verdict in
   memory and in private state keyed by that identity. A missing entry reports
   `OpenCode <version> lacks <method> <path>; update OpenCode or Zerus`.
5. Reuse one verdict per `hgs` invocation.
6. Report a disabled service configuration as `The OpenCode background
   service is disabled on <computer>`.

### Service lifecycle

- Zerus never runs an OpenCode CLI command that ensures the service, except
  `opencode service start` under the start rule below: `opencode models`,
  `opencode auth`, `opencode run` and the other `opencode service`
  subcommands are not used, and `--standalone` commands are not used against
  the user's data either, because they open a second server on the same
  database.
- Inspection, catalog polling, snapshots, connector refreshes and opening a
  session's details never start the service. When it is not running they
  report `OpenCode is not running on <computer>` and keep native state
  unknown, not stopped.
- `Start OpenCode` is an explicit action (Accounts, New session, a session's
  unavailable banner) and launch implicitly starts it. Start runs only when no
  registration file exists or its `pid` is not alive; it never runs while a
  registered process is alive, however slow. It starts
  `opencode service start` with the environment `hgs` uses for new tmux
  servers, additionally without `HGS_*`, `SSH_*`, `OPENCODE_SERVER_*` and
  the connector's own variables, detached from `hgs`, and on Linux in its own
  transient user scope (`systemd-run --user --scope`) as tmux servers are, so restarting the GUI or connector unit never takes the service with
  it. It then waits up to 20 seconds for a compatible registration.
- If OpenCode replaces the service during a turn (for example after an
  upgrade), Zerus treats the result like a native stop: it reads state again
  and never resubmits.
- Any OpenCode 1.18.35 hosts that Zerus started earlier are reported in the
  account status and never killed automatically.

### Locations

Catalog and session requests carry `location.directory`. Session operations use
the binding's directory. Machine-level summaries use the home directory.
New session and launch validation use the selected project folder.

### Identity, launch and delivery

- Launch generates a new session ID as `ses_` plus 26 random ASCII letters
  and digits, records it in the launch journal, and creates the session with that
  `id`, `location.directory`, the chosen `model` (with `variant` for effort),
  `title` from the Zerus name, `permissions` from the permission mode (phase
  2), and `metadata` `{"zerus": {"run_id", "launch_id"}}`. Metadata never
  contains secrets.
- A lost launch response is reconciled by `GET /api/session/{id}`, or by
  posting the same ID again, which returns the existing session. Zerus never
  creates two sessions for one launch.
- That pattern applies only to generating IDs. Stored, adopted, child and
  fork session IDs and message IDs keep the existing check: the expected
  prefix, at most 128 characters of ASCII letters, digits, `_` and `-`.
- Bindings store run ID, session ID, directory and name. The service URL and
  password come only from the registration file.
- Each message gets a pre-generated `msg_` ID recorded in the durable request
  journal, with a private index from that ID to its request, before
  `POST /prompt` with `delivery: queue` always. A lost response is reconciled
  with `GET /message/{id}`, then `GET /inbox`. Zerus never posts the same ID
  again: OpenCode forgets an ID once its queued message is removed, so a
  repost could deliver a message the user removed. A message found in
  neither place, like an unreachable service, leaves the result uncertain for
  explicit inspection, and the request is never replayed.
- Pending inbox items appear in Activity as queued. After Stop they stay
  visible as not delivered, with Remove (`DELETE /inbox/{id}`) behind a
  confirmation. Remove requires the item in this session's inbox, the session
  not running and the ID absent from history. Before the DELETE, Zerus records
  a removal intent for the session and message ID in private state (the
  newest 5000 records): it forbids any repost but proves nothing. Because the
  DELETE answers 204 even for an item that already left the inbox, Zerus then
  reads the inbox and, after it, history; only a message gone from both is a
  confirmed removal. Its request then reports the definite non-delivery
  "Removed from the OpenCode queue before delivery" (a voice transcript
  becomes failed), and every later reconciliation answers the same without
  contacting OpenCode. A lost, failed or 404 DELETE keeps the intent: the
  message counts as delivered if history shows it, otherwise it stays
  uncertain.
- Voice transcripts use the same guarded text path.
- Every `PATCH` reads the session first and writes back the complete
  `metadata` object or `permissions` list, so the `zerus` metadata key and
  other clients' fields survive.

### OpenCode 1 removal and migration

- Delete the OpenCode 1 host management, legacy routes, JSONC catalog parser
  and classic child roster.
- An existing OpenCode 1 binding is adopted when
  `/api/experimental/migration/v1` reports completed and
  `GET /api/session/{id}` returns the binding's session in the binding's
  directory. It then behaves like any OpenCode 2 binding.
- Otherwise it is listed as stopped with `This conversation belongs to
  OpenCode 1, which Zerus no longer supports`, and only Forget is allowed.
- A paused flag from OpenCode 1 bindings is ignored.
- Forget removes only the Zerus binding and never calls
  `DELETE /api/session`. Native data is never modified.
- `docs/opencode.md`, `docs/reference.md` and `docs/local-models.md` describe
  OpenCode 2. The account row reports `installed` only for OpenCode 2 and the
  minimum version in its reason otherwise.

### History and status

- History reads `/message` in cursor pages, newest window first, with the
  existing event and byte budgets; message IDs are stable Activity IDs.
  Reaching a budget reports truncated history.
- Status:
  - `Working`: the session is in `/api/session/active`;
  - `Needs attention`: it has pending forms or permission requests;
  - `Error`: the newest assistant message ended with a provider error;
  - `Background` (phase 2): idle, with a confirmed live child session: a
    current `/api/session/active` entry for an exact child listed with this
    `parentID`. It follows the existing status contract, including deferred
    reply-only notifications; `Working` and `Needs attention` keep priority,
    and an unreachable service means unknown, never `Background`. The same
    phase updates `docs/opencode.md`, the shared status fixtures, Android and
    iOS;
  - `Ready` otherwise.
  An unreachable or incompatible service makes state unknown with its reason.
- The adapter polls, as today. `/api/event` is a later optimization.

### Forms and permissions

Forms map onto the existing question item model with additive fields, so
desktop, Android, iOS and the connector keep one card type:

| Form field | Question item |
| --- | --- |
| `string` with `options` | options from `value`/`label`/`description`; `allow_other` = `custom` |
| `multiselect` | options; `multi_select` unless `maxItems` is 1; `allow_other` = `custom`; `min_items`, `max_items` |
| `boolean` | two options, Yes and No; default preselected |
| `string` without options | `input_kind: "text"`, `format`, `min_length`, `max_length`, `pattern` |
| `number`, `integer` | `input_kind: "number"` or `"integer"`, `minimum`, `maximum` |
| `external` | `input_kind: "external"` with its URL |
| `hidden` | omitted; its default, if any, is submitted |
| `when` | `visible_when`: the list of conditions |

- Phase 1 covers option fields, booleans and plain text, which is everything
  OpenCode's question tool produces. Other fields arrive in phase 2. Until a
  client renders a form, the card is `can_answer: false` with the reason
  `Answer this form in OpenCode`, the mechanism clients already honor.
- `hgs` validates every answer against the original form (types, bounds,
  required fields, conditions), drops answers for fields whose conditions are
  false, and submits `{"answer": {<key>: value}}`.
- Bounds: at most 32 fields, 64 options per field and 16 KiB of text per form;
  a larger form is `can_answer: false` with the same reason.
- Permission requests keep the existing permission card: Allow once, Always
  allow, Reject. Reject warns that it rejects every pending request of that
  session.
- Answers keep the existing answer journal, question hash and exact
  run/conversation identity. A settled or changed form returns an explicit
  error and never retries.

### Child sessions (phase 2)

- Children are `GET /api/session?parentID=<id>` (the 64 newest).
- Each child reports live state from the same service; unknown remains only
  for an unreachable service.
- Child forms and permission requests are answerable from Zerus with the
  child's exact session identity. Sending messages to children stays in the
  OpenCode client.
- A child never takes its parent's state, and the parent's state comes from
  its children only as `Background` from confirmed live children.

### Models and effort

- New session and model change list the models the service reports through
  `/api/model` for the selected folder, projected through the catalog
  allowlist (below), bounded to 256 entries in service order, with the
  selected model always present. The providers design replaces this order with
  its short list.
- An empty list right after the service starts is shown as loading.
- When the service is not running, New session offers `Start OpenCode` and
  shows the last known list.
- Effort options are the selected model's variant IDs. Changing the model drops
  an unsupported effort.
- Model change uses `POST /model` and verifies the session's model afterwards.
- Phones launch with effort through an additive `effort` field on the
  connector's launch request, accepted only when the connector advertises
  `opencode_launch_effort` and validated against that model's
  `effort_options` instead of the fixed effort name list.

### Catalog allowlist

Every projection of `/api/model`, `/api/provider` and `/api/integration`
copies only: IDs, names, labels, provider ID and name, integration ID, status,
`enabled`, cost numbers, limits, `capabilities`, variant IDs and method
types and labels. `settings`, `headers`, `body`, `options`, URLs and
credential values are dropped in memory and never cached, logged, journaled
or returned. Zerus never calls `/api/credential` in this adapter.

### Attachments (phase 2)

Attachments are files already staged on the agent computer by the existing
attachment path. The prompt passes them as `files` with a `file://` URI and
the original name (no description), within 20 MiB per file and the existing
count limit. Unsupported media types are rejected before submission, keeping
the draft.

### Compaction and fork (phase 2)

- Compact context calls `POST /compact` with a client ID, the existing
  confirmation and journal.
- Fork is journaled before `POST /fork`. A lost response is reconciled by
  listing sessions whose fork source is the original session, created after the
  journal entry. Fork is unavailable for an empty session. The new session gets
  a new Zerus binding with a derived name; the original stays unchanged.

### Permission modes (phase 2)

- `Default` adds no session rules; OpenCode's configuration applies.
- `Bypass` sets a session ruleset that allows every action and then re-appends
  the deny rules of the session's agent, read from `GET /api/agent/{id}` with
  the session's directory as `location`, so configured denies still win. An
  error, 404 or empty answer fails closed: Bypass is not applied and the
  reason is shown. The denies are a snapshot taken when Bypass is applied;
  Zerus derives them again whenever it applies the mode (launch, mode change
  or agent change), and the confirmation says that later configuration edits
  apply after selecting Bypass again. Selecting it requires the existing
  confirmation.
- Other Zerus modes are offered only where an equivalent ruleset exists;
  otherwise they stay unavailable with a reason.
- Changing the mode uses a read-modify-write `PATCH`.
- Automatic tool approvals remain a separate, manual opt-in and are not part
  of this adapter.

### Actions

The adapter reports `allowed_actions` and `action_reasons` explicitly. Phase 1
allows send, stop, answer, rename, forget, model and effort when the service is
compatible and the state permits them; phase 2 adds compact, fork and
permission mode. Pause, resume, terminate, archive, restore and Terminal are
unavailable with reasons.

### Connector and mobile

- The connector forwards the additive question item fields unchanged and keeps
  API v1 compatible; bounds match `hgs`.
- Android and iOS render effort for OpenCode models and the allowed actions the
  session reports in phase 1, and the new item kinds and conditions in phase 2.

### Security and privacy

- The service password, URL and registration file never leave the agent
  computer and never enter logs, journals, receipts, relay payloads or errors.
- Catalog projections follow the allowlist; credential routes are not used.
- Metadata written to sessions contains only Zerus identifiers.

## Testing and qualification

- Rust unit tests: registration parsing (no `state` field, channel file
  names, dead `pid`), version comparison, OpenAPI requirement checks, ID
  formats, the catalog allowlist against fixtures carrying keys and headers,
  form mapping and answer validation, permission rulesets, inbox
  reconciliation, bounds.
- Native qualification (`tests/test_opencode_native.py`) against the actual
  minimum OpenCode with private `XDG_*` directories, a private service port
  and an offline catalog, its own service registration and a keyless local
  model fixture. Phase 1: launch with own IDs, lost launch and message
  responses (including a queued message), Stop with a pending queue item,
  history pages, model and variant, question-tool forms and permissions,
  OpenCode 1 binding adoption and refusal, service not running, start only
  with a dead registration, incompatible version and missing operation.
  Phase 2: children and `Background`, compaction, fork reconciliation,
  attachments, Bypass with a configured deny.
- Qt tests for effort, forms and allowed actions; Android and iOS tests from
  shared fixtures (`opencode-form-contract.json`); connector tests for the
  effort launch flag and bounds.
- CI installs the minimum `@opencode/cli` pinned by integrity hash, like
  `dsh`, and runs the native suite in the full Linux and macOS matrix; a
  daily canary runs it with the newest release and reports incompatibilities
  without changing the minimum.

## Rollout

Merge each phase after CI and native qualification pass, then install Zerus on
arch and mac through the normal installers. OpenCode itself is already 2.0.32
on both computers.
