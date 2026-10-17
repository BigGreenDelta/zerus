# OpenCode 2 native adapter design

Date: 2026-10-17. Beads epic: `zerus-pl2u`. Follow-up feature: `zerus-dg1g`
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
local OpenCode service offers; Activity shows its history and live state; Stop,
messages, voice transcripts, attachments, model and effort changes, forms
(questions), permissions, child sessions, compaction, fork, rename and Forget
work; and the conversation stays usable from the OpenCode TUI at the same time.

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

Probed on 2026-10-17 with isolated homes and fixture credentials only.

OpenCode 1.18.35:

- A running host keeps the models.dev catalog and configuration for the life of
  the process. `opencode models --refresh`, configuration edits,
  `POST /instance/dispose` and `POST /global/dispose` do not expose new models
  or providers; only a process restart does.
- Credentials written through `PUT /auth/{id}` reach `auth.json` and appear
  after an instance dispose. The v2-style `/api/integration/{id}/connect/key`
  stores credentials in `opencode.db`, which providers ignore.

OpenCode 2.0.32 (`opencode serve`, OpenAPI at `/openapi.json`, 119 paths):

- Removed: `/global/health`, `/api/session/{id}/history`,
  `/api/session/{id}/question`, `/permission`, `/question` and every legacy
  `/session/...` route, including children and messages.
- Present: `/api/info` (health and version), `/api/event` (server-sent
  events), `/api/session` (list with `parentID`, `directory`, `cursor`,
  `limit`, `order`), `POST /api/session` with optional client `id`
  (pattern `^ses`), `parentID`, `title`, `agent`, `model`, `location`,
  `metadata` and `permissions`; `/api/session/active`; `/api/session/{id}`
  (`GET`, `PATCH` title/metadata/permissions, `DELETE`); `/prompt` with
  optional client message `id` (pattern `^msg_`), `text`, `files`
  (`uri`, `name`, `description`), `delivery` (`steer` or `queue`) and
  `resume`; `/message` (cursor pages) and `/message/{messageID}`;
  `/interrupt`; `/model` (`Model.Ref` with `providerID`, `id`, optional
  `variant`); `/compact`; `/fork`; `/form` and `/form/{id}/reply`;
  `/permission` and `/permission/{id}/reply` (`once`, `always`, `reject`);
  `/inbox`; `/api/model`, `/api/model/default`, `/api/provider`,
  `/api/integration`, `/api/credential`, `/api/location/reload`.
- Forms replace questions. Field types: `string` (format, length, pattern),
  `number`, `integer` (minimum, maximum), `boolean` (default), `multiselect`
  (options with value/label/description, min and max items) and `external`
  (URL). Fields can be `required`, `hidden` and conditional (`when`: key,
  `eq`/`neq`, value).
- Connecting a key through `/api/integration/{id}/connect/key` exposed the
  provider's models immediately. Configuration file edits appeared without a
  restart. Keyless OpenCode free models include `glm-5.3-flash-free`.
- The background service registers itself in
  `$XDG_STATE_HOME/opencode/service.json` (default
  `~/.local/state/opencode/service.json`, also on macOS) with `url`, `pid`,
  `version` and a private `password`, mode 0600. Upstream's client treats that
  file as the complete discovery contract and authenticates with HTTP basic
  user `opencode`. A client of another version replaces a mismatched service.
- On the real arch and mac upgrades, the existing Z.AI Coding Plan key from
  OpenCode 1 remained available to OpenCode 2.

## Non-goals

- OpenCode 1 compatibility. Saved OpenCode 1 bindings stay listed only so they
  can be forgotten.
- Starting, stopping, restarting or upgrading the OpenCode service on the
  owner's behalf, except starting it for an explicit user action.
- Provider connection, model favorites and catalog search
  ([providers design](2026-10-17-opencode-providers-design.md)).
- Embedding OpenCode's web interface as Native UI, Terminal, Pause/Resume,
  archive and the `Background` state for OpenCode.
- Windows hosts. OpenCode remains unavailable on Windows computers as today.

## Architecture

The adapter stays a Rust module of `hgs` (`src/state/opencode.rs`, split into
focused files where it grows) with a small blocking HTTP client, as today.
Alternatives were rejected: calling `opencode api` for each request costs a
process per call and is too slow for polling; a Node sidecar using
`@opencode/client` adds a runtime dependency on every host.

### Service connection

1. Read the registration file. `XDG_STATE_HOME` follows the same environment
   as the agent home; `OPENCODE_*` selectors that change the data or state
   directory are honored the way OpenCode does.
2. Require `state` ready, a loopback `url`, a live `pid`, and a password when
   the file has one. Never log or forward the password, URL or file.
3. Call `GET /api/info` with basic authentication. Its version must be at or
   above `OPENCODE_MINIMUM` (2.0.32 initially).
4. Fetch `/openapi.json` once per service identity (`id`, `pid`, `version`,
   `url`) and check the required operation table (method, path, required
   request fields, response fields Zerus reads). Cache the verdict in memory
   and in private state keyed by that identity. A missing entry produces
   `OpenCode <version> lacks <method> <path>; update OpenCode or Zerus`.
5. Reuse one connection verdict per `hgs` invocation.

Service lifecycle:

- Background inspection, catalog polling and snapshots never start the
  service. When it is not running they report `OpenCode service is not
  running` and keep the last known state as unknown, not stopped.
- Explicit actions (launch, send, answer, model change, compact, fork, rename,
  opening a session's details) start it with `opencode service start` when no
  compatible registration exists, unless the user's service configuration is
  disabled, then wait for a compatible registration within a bounded time.
- Zerus never stops, restarts or replaces the service. If OpenCode replaces it
  (for example after an upgrade) during a turn, Zerus treats the result like a
  native stop: it reads the session state again and never resubmits.

### Identity, launch and delivery

- Launch generates a session ID with the `ses_` prefix and a random suffix and
  creates the session with that `id`, the project `location.directory`, the
  chosen `model` (with `variant` for effort), `title` from the Zerus name,
  `permissions` from the permission mode, and `metadata`
  `{"zerus": {"run_id", "launch_id"}}`. Metadata never contains secrets.
- The binding stores run ID, session ID, directory and name. It does not store
  the service URL or password; those come from the registration file.
- A lost launch response is reconciled with `GET /api/session/{id}` for the
  pre-generated ID. Zerus never creates a second session for one launch.
- Each message gets a pre-generated `msg_` ID, recorded in the durable request
  journal before `POST /prompt`. A lost response is reconciled with
  `GET /message/{id}`; when neither confirmation nor rejection is available,
  the result stays uncertain for explicit inspection, as today.
- Messages sent while the session works use `delivery: queue`. Zerus does not
  use `steer`.
- Voice transcripts use the same guarded text path.

### History and status

- History reads `/message` in cursor pages, newest window first, with the
  existing event and byte budgets; message IDs are the stable Activity IDs.
  Reaching a budget reports truncated history.
- Status: `Working` when the session is in `/api/session/active`; `Needs
  attention` when it has pending forms or permission requests; `Error` when
  the newest assistant message ended with a provider error; otherwise
  `Ready`. An unreachable or incompatible service makes state unknown with
  its reason.
- The adapter polls, as today. `/api/event` is a later optimization.

### Forms and permissions

Forms map onto the existing question item model with additive fields, so
desktop, Android, iOS and the connector keep one card type:

| Form field | Question item |
| --- | --- |
| `multiselect` | options from `value`/`label`/`description`; `multi_select` when `maxItems` is not 1 |
| `boolean` | two options, Yes and No; default preselected |
| `string` | free text (`input_kind: "text"`, `format`, `min_length`, `max_length`, `pattern`) |
| `number`, `integer` | free text (`input_kind: "number"` or `"integer"`, `minimum`, `maximum`) |
| `external` | `input_kind: "external"` with its URL; answered by opening it |
| `hidden` | omitted; its default (when any) is submitted |
| `when` | `visible_when` conditions on the item |

- `hgs` validates every answer against the original form (types, bounds,
  required fields, conditions), drops answers for fields whose conditions are
  false, and submits `Form.Reply` with values keyed by field key.
- Bounds: at most 32 fields, 64 options per field and 16 KiB of text per form;
  a larger form is shown read-only with `Answer this form in OpenCode`.
- Clients render `input_kind`, `visible_when` and bounds. An older client that
  does not know `input_kind` shows a text field, and `hgs` still validates. A
  form containing `external` fields is marked `requires_client_feature:
  "opencode_forms"`; older Android and iOS builds show `Answer on the desktop`.
- Permission requests keep the existing permission card: Allow once, Always
  allow, Reject.
- Answers keep the existing answer journal, question hash and exact
  run/conversation identity. A settled or changed form returns an explicit
  error and never retries.

### Child sessions

- Children are `GET /api/session?parentID=<id>` (bounded to the 64 newest).
- Each child reports live state from the same service (`Working`, `Needs
  attention`, `Ready`, `Error`); unknown remains only for an unreachable
  service.
- Child forms and permission requests are answerable from Zerus with the
  child's exact session identity. Sending messages to children stays in the
  OpenCode client.
- Parent and child states stay independent, as in the existing contract.

### Models and effort

- New session and model change offer the models the service reports through
  `/api/model` for connected providers, with labels from the service, bounded
  to 256 entries in service order; the selected model is always present. The
  providers design later replaces this order with its short list.
- Effort options are the selected model's variants; changing model drops an
  unsupported effort.
- When the service is not running, the New session form runs
  `opencode models --standalone` once to list models and otherwise shows the
  last known list. Background account polling uses the last known list.
- Model change uses `POST /model` and verifies the session's model afterwards.

### Attachments

Attachments are files already staged on the agent computer by the existing
attachment path. The prompt passes them as `files` with a `file://` URI and
the original name, within the existing size and count limits. Unsupported
media types are rejected before submission, keeping the draft.

### Compaction and fork

- Compact context calls `POST /compact` with the existing confirmation and
  journal.
- Fork calls `POST /fork` and creates a new Zerus binding for the returned
  session with a derived name; the original stays unchanged.

### Permission modes

- `Default` (provider settings) adds no session rules; OpenCode's own
  configuration applies.
- `Bypass` sets a session ruleset that allows every action not explicitly
  denied by configuration, equivalent to OpenCode's `--auto`. Qualification
  pins the exact ruleset against the running service.
- Other Zerus modes are offered only where an equivalent ruleset exists;
  otherwise they stay unavailable with a reason.
- Changing the mode on an existing session uses `PATCH /api/session/{id}`.
- Automatic tool approvals remain a separate, manual opt-in and are not part
  of this adapter.

### Actions

The adapter reports `allowed_actions` and `action_reasons` explicitly:
send, stop, answer, rename, forget, model, effort, compact and fork are
allowed when the service is compatible and the state permits them; pause,
resume, terminate, archive, restore and Terminal are unavailable with reasons.

### OpenCode 1 removal

- Delete the OpenCode 1 host management, legacy routes, JSONC catalog parser
  and classic child roster.
- Existing OpenCode 1 bindings are listed as stopped with `This conversation
  belongs to OpenCode 1, which Zerus no longer supports`; only Forget is
  allowed. Native OpenCode 1 data is never modified.
- `docs/opencode.md`, `docs/reference.md` and the local model guide describe
  OpenCode 2. The account row reports `installed` only for OpenCode 2.

### Connector and mobile

- The connector forwards the additive question item fields unchanged and
  keeps API v1 compatible; bounds match `hgs`.
- Android and iOS render the new item kinds and conditions, the effort row for
  OpenCode models, and the allowed actions the session reports.

### Security and privacy

- The service password, URL and registration file never leave the agent
  computer and never enter logs, journals, receipts, relay payloads or errors.
- Provider credentials are never read by this adapter.
- Metadata written to sessions contains only Zerus identifiers.

## Testing and qualification

- Rust unit tests: registration parsing and rejection, version comparison,
  OpenAPI requirement checks, ID generation, form mapping and answer
  validation, permission rulesets, bounds.
- Native qualification (`tests/test_opencode_native.py`) against the actual
  minimum OpenCode with private `XDG_*` directories, its own service
  registration and a keyless local model fixture: launch with own IDs, lost
  launch and message responses, queued delivery, history pages, Stop, model
  and variant, forms and permissions (fixture tools), children, compaction,
  fork, attachments, Bypass rules, incompatible version and missing operation.
- Qt tests for form items, effort and allowed actions; Android and iOS tests
  from shared fixtures (`opencode-form-contract.json`).
- CI: the full matrix runs the pinned minimum on Linux and macOS; a daily
  canary runs the native suite with the newest `@opencode/cli` and reports
  incompatibilities without changing the minimum.

## Verification items for the plan's first task

Each is checked against the real service before code depends on it:
client `ses_`/`msg_` ID formats accepted; Bypass ruleset shape; `file://`
attachments; `Form.Reply` value shape; how subagent children appear;
`/compact` and `/fork` responses; `opencode service start` behavior without a
terminal on Linux and macOS; registration file location with custom
`XDG_STATE_HOME`.

## Rollout

Merge after CI and native qualification pass, then install Zerus on arch and
mac through the normal installers. OpenCode itself is already 2.0.32 on both
computers.
