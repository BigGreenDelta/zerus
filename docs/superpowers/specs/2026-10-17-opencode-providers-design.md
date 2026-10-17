# OpenCode providers and models design

Date: 2026-10-17. Beads feature: `zerus-dg1g`, blocked by the
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
  available model; favorites, configured, free and the current model form the
  bounded short list that New session and phones show first.
- Build on the OpenCode 2 service API; Zerus never restarts the service for a
  change.

## Verified OpenCode 2 behavior

From isolated OpenCode 2.0.32 probes on 2026-10-17:

- `/api/provider` lists connected providers; `/api/model` lists models with
  provider, name, status, cost, limits, capabilities and reasoning variants;
  `/api/model/default` reports the default model.
- `/api/integration` lists each provider's connection methods (`key`, `oauth`
  with optional prompts, `env`, `command`, `external`) and its connections.
- `POST /api/integration/{id}/connect/key` (`key`, optional `label`) takes
  effect immediately; OAuth and command connections return attempts that are
  polled, completed with a code or cancelled.
- `/api/credential` lists credentials (several per integration, one active) and
  supports activate, rename (`PATCH`) and remove (`DELETE`).
- Configuration file edits are picked up live; `/api/location/reload` exists.
- `opencode auth export <integration>` prints that integration's credentials,
  including secrets, as JSON; `opencode auth import` reads them from stdin.
- Keyless OpenCode free models are listed with zero cost.

## Catalog

- `hgs` reads the catalog from the local OpenCode service through the adapter's
  service connection. Inspection never starts the service; when it is not
  running the account row shows `OpenCode is not running` with the last known
  catalog and its age, and `Start OpenCode` is an explicit action. The New
  session form may still list models once with `opencode models
  --standalone`, as the adapter design describes.
- The account row for `native-opencode` gains additive fields:
  - `providers`: up to 64 entries with `id`, `label`, connection kind
    (`free`, `key`, `oauth`, `env`, `config`, `command`), credential count,
    active credential label, model count;
  - `model_options`: the short list, at most 256 entries, ordered current
    session model, default model, favorites, models from configuration, free
    models; each entry has `id`, `label`, `provider`, `provider_label`, and
    flags `free`, `favorite`, `default`; variants become `effort_options`;
  - `catalog_at` and `catalog_reason`.
- The full catalog is fetched only by the desktop on demand (`hgs account
  models native-opencode --json`), limited to connected providers, at most
  10,000 models and 4 MiB.
- Launch and model change validate against the service's full list, so any
  available model works from the CLI as well.
- Free means zero input and output cost in the service's catalog.
- Favorites are stored on the agent computer in the `hgs` account catalog
  (at most 64 model IDs), so every desktop and phone connected to that
  computer sees them.

## Desktop

### Accounts

The Native OpenCode configuration card shows, per computer:

- providers with connection kind, active account and model count; actions
  Replace key, Add account, Switch account, Remove, Copy to another computer;
- Connect provider, Add endpoint and Refresh;
- `OpenCode is not installed on <computer>` or `OpenCode is not running` when
  that is the case.

### Connect provider

- A searchable list of the service's integrations, then the methods it offers.
- Key: a password field plus any prompts the method declares. The key goes to
  `hgs` on the target computer over SSH stdin, as DeepSeek keys do today, and
  is submitted to `connect/key`.
- Browser sign-in: start the OAuth attempt, show its URL and instructions with
  Open and Copy, accept a code when the attempt mode is `code`, poll until
  done, expired or cancelled (at most 10 minutes), and cancel on close. Methods
  that need a browser on the agent computer are labelled when it is remote.
- Command connections are shown with their instructions and polled the same
  way.
- Environment-variable connections are shown read-only.

### Custom endpoints

- Add endpoint asks for ID, name, base URL, an optional key and models
  (entered, or fetched from the endpoint's `/v1/models` by `hgs` on the agent
  computer).
- `hgs` edits the user's global OpenCode configuration file with a JSONC-aware
  change limited to that provider entry, keeps comments and formatting, writes
  atomically, keeps the three newest backups and validates the result through
  the service. A failed validation restores the backup.
- The key is stored as a credential, never in the configuration file.
- Zerus edits and removes only endpoints it created (recorded in the `hgs`
  catalog); other configured providers are read-only.

### Copy to another computer

- API-key credentials and Zerus-created endpoints can be copied. The source
  `hgs` runs `opencode auth export <integration>` and keeps only the selected
  key credential; the desktop pipes it to `opencode auth import` through `hgs`
  on the target computer, never to a terminal or file, with a size limit.
- An endpoint copy also writes its configuration entry. A loopback base URL is
  flagged because it points to the target computer's own service.
- OAuth credentials are not copied (refresh tokens can rotate and break both
  computers); the dialog offers Sign in on the target computer instead.
  Environment-variable connections are not copied.

### Model picker

New session and model change group the short list by provider with Free,
favorite and default marks, a star to toggle favorites, and All models, which
searches the full catalog of that computer.

## Phones

Android and iOS show providers per computer in Accounts and use the short list
in New session and model change, grouped by provider with Free and favorite
marks and search within the list. They cannot connect or copy providers.
The connector forwards only IDs, labels, kinds, counts and flags.

## Security and privacy

- Keys travel only over SSH stdin to `hgs` on the agent computer and through
  OpenCode's own API or `auth import`; they never enter command lines, logs,
  journals, receipts, relay payloads or error messages.
- Exported credential JSON exists only in process memory and pipes.
- Base URLs of providers are shown on the desktop only.

## Testing

- Rust: catalog projection and bounds, short-list order, favorites storage,
  JSONC edit with comments, backup rotation, credential filtering for copy.
- Native qualification with the real minimum OpenCode and private homes:
  catalog with keyless free models, key connect and remove with a fixture
  provider, endpoint add and remove, export/import between two private homes,
  service not running.
- Qt tests for the Accounts card, dialogs and picker; Android and iOS tests
  for providers and the grouped picker from shared fixtures; connector bounds.
- Browser sign-in with real accounts is a manual, opt-in check.
