# OpenCode providers and models design

Date: 2026-10-17, revised 2026-10-18 after independent review. Beads feature:
`zerus-dg1g`, blocked by the
[OpenCode 2 adapter](2026-10-17-opencode-2-adapter-design.md) (`zerus-pl2u`).

## Goal

Set up OpenCode providers and models from Zerus instead of OpenCode's own
interface, and show what is connected and available on each computer. For
example, start a session with the free `opencode/glm-5.3-flash-free` or
`opencode/deepseek-v4.1-flash-free` right after installing OpenCode, or connect
a provider key on one computer and copy it to another.

## Owner decisions (2026-10-17)

- One design covers both the catalog (what is connected and available per
  computer) and connecting providers from Zerus.
- Connecting on the desktop includes API keys, browser sign-in, custom
  endpoints and copying a connection to another computer.
- Phones (Android and iOS) show provider status per computer and the new model
  picker, read-only. Credentials never pass through the relay.
- Large catalogs use favorites plus search: the desktop can search every
  available model; favorites, configured, free and default models form the
  bounded short list that New session and phones show first.
- Build on the OpenCode 2 service API; Zerus never restarts the service for a
  change.

## Verified OpenCode 2 behavior

From isolated OpenCode 2.0.32 probes and upstream source (`v2.0.32`):

- `/api/provider` lists connected providers and `/api/model` lists models with
  provider, name, status, cost, limits, capabilities and reasoning variants;
  `/api/model/default` reports the default model. All three take a
  `location`: a project `opencode.json` adds providers for that folder only.
  Right after the service starts `/api/provider` can be empty, and the model
  list changes when its catalog refreshes.
- Their responses also carry provider `settings`, `headers` and `body`, which
  can contain API keys and authorization headers.
- `/api/integration` lists each provider's connection methods (`key`,
  `oauth` with prompts, `env`, `command`, `external`) and connections.
  Auto-detected local providers (for example Ollama) can appear without an
  integration.
- `POST /api/integration/{id}/connect/key` (`key`, optional `label`,
  `answer` for prompts) takes effect immediately. OAuth and command
  connections return attempts that are polled, completed with a code or
  cancelled.
- `/api/credential` lists credentials with raw secret values (several per
  integration, one active) and supports create, activate, rename and remove.
  `opencode auth export/import` are wrappers over it and also run upstream's
  service ensure logic, which the adapter design forbids; `import` skips IDs
  that already exist.
- Configuration file edits are picked up live. OpenCode 2 reads the same
  `provider` configuration entries as OpenCode 1 (the arch `llamacpp-*`
  providers appeared unchanged).
- Keyless OpenCode free models are listed with zero cost.

## Phases

- Phase 1: read-only catalog per computer, favorites, grouped picker with
  search on the desktop, providers and the short list on phones. This is
  enough to find and start free models from any client.
- Phase 2: connect, replace and remove API keys; switch and rename accounts.
- Phase 3: browser and command sign-in, custom endpoints, copying a connection
  to another computer.

Each phase ships as its own pull request.

## Catalog

- `hgs` reads the catalog through the adapter's service connection and catalog
  allowlist: IDs, names, labels, provider and integration IDs and names,
  status, `enabled`, cost numbers, limits, `capabilities`, variant IDs,
  method types and labels, and connection IDs, labels, methods and status from
  `/api/integration`. Secret fields are dropped in memory and never cached,
  logged or returned. Polling never calls `/api/credential`; only the phase 3
  copy path reads one credential (below).
- Machine-level views use the home directory as `location`; New session uses
  the selected project folder and labels folder-specific providers.
- Inspection never starts the service. When it is not running the account row
  reports `OpenCode is not running` with the last known catalog and its age;
  `Start OpenCode` is the explicit action from the adapter design.
- The `native-opencode` account row gains additive fields:
  - `providers`: up to 64 entries with `id`, `label`, connection kind
    (`free`, `key`, `oauth`, `env`, `config`, `command`, `external`,
    `local`), credential count, active credential label and model count;
  - `model_options`: the short list, ordered default model, favorites, models
    from configuration, free models; each entry has `id` and `value` (equal,
    because the connector and its launch check keep only entries with
    `value`), `label`, `provider`, `provider_label`, flags `free`,
    `favorite`, `default`, and `effort_options`;
  - `catalog_at`, `catalog_state` (`ready`, `loading`, `not_running`,
    `incompatible`) and `catalog_reason`.
- The OpenCode row has a 32 KiB budget, measured as the connector measures
  projections (ASCII-escaped JSON), so the connector's 128 KiB
  account projection limit cannot empty the phones' account list. When the
  budget is reached, entries are dropped in this order: free models, then
  configured models, then favorites, then providers beyond the first 16; the
  default model is always kept. The row then reports
  `model_options_truncated` and `providers_truncated`. At most 256 model
  entries are listed.
- The session's current model belongs to session settings, not this row;
  pickers add it when absent, as today.
- The full catalog is fetched only by the desktop on demand
  (`hgs account models native-opencode --location <folder> --json`), limited
  to connected providers, at most 10,000 models and 4 MiB.
- Launch and model change validate against the full list for the folder, so
  any available model works from the CLI too.
- Free means zero input and output cost in the service catalog.
- Favorites are stored on the agent computer in the `hgs` account catalog (at
  most 64 model IDs), so every desktop and phone connected to that computer
  sees them. A favorite that the catalog no longer lists stays in the list as
  unavailable until removed.

## Desktop

### Accounts

The Native OpenCode configuration card shows, per computer:

- providers with connection kind, active account and model count;
- Refresh, and `Start OpenCode` when it is not running;
- `OpenCode is not installed on <computer>` or the incompatibility reason;
- phase 2: Connect provider, and per provider Replace key, Add account,
  Switch account, Rename account and Remove;
- phase 3: browser sign-in in Connect provider, Add endpoint, and Copy to
  another computer.

Remove, Replace key, Switch account and endpoint removal affect every session on
that computer and require an explicit confirmation that says so.

### Connect provider (phase 2 keys, phase 3 sign-in)

- A searchable list of the service's integrations, then the methods it offers.
- Key: a password field plus any prompts the method declares. The key goes to
  `hgs` on the target computer over SSH stdin, as DeepSeek keys do today (on a
  Windows desktop through `hgs.exe`, never a piped `ssh.exe`), and is
  submitted to `connect/key`.
- Browser sign-in: start the OAuth attempt, show its URL and instructions with
  Open and Copy, accept a code when the attempt mode is `code`, poll until
  done, expired or cancelled (at most 10 minutes), and cancel on close. Methods
  that need a browser on the agent computer are labelled when it is remote.
- Command connections show their instructions and are polled the same way.
- Environment-variable and auto-detected local connections are read-only.

### Custom endpoints (phase 3)

- Add endpoint asks for ID, name, base URL, an optional key and models
  (entered, or fetched from the endpoint's `/v1/models` by `hgs` on the agent
  computer).
- `hgs` adds an OpenCode `provider` entry (`npm`
  `@ai-sdk/openai-compatible`, `name`, `options.baseURL`, `models`) to the
  user's global OpenCode configuration file with a JSONC-aware change limited
  to that entry. It keeps comments and formatting, writes atomically, keeps the
  three newest backups and checks that the service lists the provider; a
  failed check restores the backup.
- The key is stored as a credential through `connect/key`, never in the
  configuration file.
- Zerus edits and removes only endpoints it created (recorded in the `hgs`
  catalog); other configured providers are read-only.

### Copy to another computer (phase 3)

- API-key credentials and Zerus-created endpoints can be copied. OAuth
  credentials are not copied, because refresh tokens can rotate and break both
  computers; the dialog offers Sign in on the target computer instead.
  Environment-variable and local connections are not copied.
- The desktop runs `hgs` on the source computer, which reads that one key
  credential through `/api/credential` and writes its record (integration ID,
  label, and `value` with type, key, `configuration` and `metadata`, so
  prompted fields such as an Azure resource or Cloudflare account survive) to
  a private pipe, never to a terminal or file, with a size
  limit, as account copies do today. The desktop pipes it to `hgs` on the
  target computer, which creates it with `POST /api/credential`, passing `activate`
  explicitly.
- If the target already has a credential for that integration, the dialog asks
  whether to add it as another account or to replace the active one; the new
  credential becomes active only when the user chooses so.
- An endpoint copy also writes its configuration entry on the target. A
  loopback base URL is flagged because it points to the target computer itself.

### Model picker

New session and model change group the short list by provider with Free,
favorite and default marks, a star to toggle favorites, and All models, which
searches the full catalog of that computer and folder.

## Phones

Android and iOS show providers per computer in Accounts and use the short list
in New session and model change, grouped by provider with Free and favorite
marks and search within the list. They show truncation and `catalog_state`
messages. They cannot connect or copy providers. The connector forwards only
the allowlisted fields within its existing bounds.

## Security and privacy

- Keys travel only over SSH stdin to `hgs` on the agent computer and through
  OpenCode's API; they never enter command lines, logs, journals, receipts,
  relay payloads or error messages.
- A copied key exists only in process memory and pipes.
- Provider base URLs are shown on the desktop only.

## Testing

- Rust: allowlist projection against fixtures with keys and headers, short-list
  order, the 32 KiB budget and truncation order, favorites storage, location
  handling, JSONC edit with comments, backup rotation, credential filtering
  for copy.
- Connector: an OpenCode row at the budget and the 128 KiB account projection
  limit.
- Native qualification with the minimum OpenCode and private homes: catalog
  with keyless free models, a folder-specific provider, service not running,
  key connect and remove with a fixture provider, endpoint add and remove, copy
  between two private homes including an existing credential.
- Qt tests for the Accounts card, dialogs, confirmations and picker; Android
  and iOS tests for providers, truncation and the grouped picker from shared
  fixtures.
- Browser sign-in with real accounts is a manual, opt-in check.
