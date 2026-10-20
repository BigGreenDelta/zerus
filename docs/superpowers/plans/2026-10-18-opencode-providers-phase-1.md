# OpenCode Providers Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show which OpenCode providers are connected on each computer, give New session and model change a bounded, grouped short list (default, favorites, configured and free models) on the desktop and phones, let the desktop search every available model and star favorites that are stored on the agent computer.

**Architecture:** `hgs` reads `/api/provider`, `/api/model`, `/api/model/default` and `/api/integration` through the adapter's existing service connection, projects only allowlisted fields, derives a connection kind per provider, and builds the short list. The `native-opencode` account row carries the short list and providers within a 32 KiB budget measured as the connector measures; `hgs account models` returns the full catalog of one folder to the desktop on demand; `hgs account favorite` changes the favorites list beside the account catalog with a request identity. The Rust connector forwards the new allowlisted fields within its 128 KiB bound; desktop, Android and iOS render one grouped picker shape pinned by one shared fixture.

**Tech Stack:** Rust (`hgs`, `serde_json`), Python `unittest` (CLI and native qualification), Rust mobile connector (`services/mobile/connector`, `zerus_relay::json`), Qt 6 desktop (`tray/src`), Kotlin/Compose Android, SwiftUI with `ZerusKit` on iOS.

**Spec:** `docs/superpowers/specs/2026-10-17-opencode-providers-design.md` (phase 1 only: Catalog, Desktop › Accounts read-only parts, Desktop › Model picker, Phones). Read it and the adapter spec `docs/superpowers/specs/2026-10-17-opencode-2-adapter-design.md` before starting any task. Format reference: `docs/superpowers/plans/2026-10-18-opencode-2-adapter-phase-1.md`.

**Beads:** `zerus-dg1g.1` (parent `zerus-dg1g`). Phases 2 and 3 (`zerus-dg1g.2`, `zerus-dg1g.3`) are out of scope except for the seams listed at the end.

**Base:** branch `opencode/providers-1`, started from `opencode/root-repairs` at `ddddd914`: the reviewed lane integration (OpenCode slices S1–S4 and the S5 desktop/hgs parts) plus the reviewed root repairs. The S5 relay and phone slices live on their own branches and are composed by the root coordinator; this plan does not depend on them. Keep the behavior of everything already on the base.

## Global Constraints

From the OpenCode 2 adapter phase 1 constraints (verbatim):

- Minimum OpenCode version: `2.0.32`; accept any `2.x` at or above it that passes the required-operation table.
- Never stop, restart, replace or upgrade the OpenCode service. Start it only when no registration file exists or its `pid` is dead, only via `opencode service start`, only for explicit actions and launch.
- Never run `opencode models`, `opencode auth`, `opencode run`, other `opencode service` subcommands, or any `--standalone` command against the user's data. `opencode --version` is allowed.
- Never log, journal, return or forward the service password, URL, registration file contents, provider `settings`, `headers`, `body`, `options`, URLs or credential values.
- New session IDs: `ses_` + 26 random ASCII letters and digits; new message IDs: `msg_` + 26 random ASCII letters and digits. Stored, adopted, child and fork IDs keep the existing check: expected prefix, ≤128 chars of ASCII letters, digits, `_`, `-`.
- Always send `delivery: "queue"`; never `steer`.
- Forget never calls `DELETE /api/session`.
- Keep API v1, durable request identities and the connector journal compatible. Additive fields only.
- Bounded reads: history ≤10,000 rows and ≤16 MiB per scan; responses ≤32 MiB; forms ≤32 fields, ≤64 options per field, ≤16 KiB text.
- Native tests use private `XDG_*` directories, a private service port and an offline catalog; never the developer's real OpenCode service or the Mac.
- Repository text is English. Every commit message ends with the Co-Authored-By trailer given in the implementer's instructions (omitted from the examples below).
- The repository owner explicitly authorized local commits (and push/merge by the orchestrator) for this work on 2026-10-13.

From the OpenCode remaining lane constraints (verbatim):

- Repository language is English. Match the surrounding code's style, naming and comment density.
- Keep API v1, durable request identities and the connector journal compatible. Additive fields only; old clients keep working.
- Commit authorization: the repository owner authorized local commits for this work (2026-10-13), and the root coordinator's 2026-10-18 assignment says "You may commit isolated source and qualify against private fixtures." Commit locally on your own branch only. Never push, merge, publish, install, or touch main.
- Every commit message ends with exactly the Co-Authored-By trailer line given in the implementer's instructions. (This plan does not spell the trailer out: the source privacy guard rejects its address in repository files.)
- Never use bare `git stash`/`git stash pop` (the stash is shared between worktrees). Use a WIP commit instead.
- Never restart or kill native agents, tmux servers, DeepSeek hosts or the user's OpenCode service. Never install a GUI/APK/app. Never touch the developer's real OpenCode data, accounts, keys or the maintained connector.
- Do not edit `src/state/voice.rs`, `src/state/conversation_history.rs` or generic voice code (owned by another session).
- Dangerous approvals: a choice that reaches beyond the one request (scope "broader", e.g. OpenCode "Always allow") is always a separate, explicit confirmation. Never auto-select or default it.
- The shared contract `tests/fixtures/opencode-form-contract.json` and its Android copy `mobile/android/app/src/test/resources/opencode-form-contract.json` must stay byte-identical. Do not change the fixture unless your brief says so.
- Shared machines: run heavy builds with `nice -n 10`, at most 8 parallel jobs. The Mac (`ssh -o BatchMode=yes mac`) is shared and often loaded; use a fresh `mktemp -d` copy there and delete it afterwards.
- Do not dispatch subagents. Do not run reviews yourself beyond your own self-review; an independent reviewer follows.
- Toolchains: Rust `cargo +1.99.1`; desktop `bash scripts/ci/gui.sh build` / `bash scripts/ci/gui.sh test` or narrower ctest/test binaries under `.ci-build/gui/tests/` with `QT_QPA_PLATFORM=offscreen`; Android `cd mobile/android && nice -n 10 ./gradlew testDebugUnitTest lintDebug`; iOS ZerusKit `swift test` only on the Mac (no swift on Linux).

For this plan:

- API v1 additive only: every new field is optional for readers; old clients and the legacy Python connector keep working by ignoring it.
- Secrets — provider and model `settings`, `headers`, `body`, `options`, base URLs, connection status `message` and `url`, credential IDs and credential values — are dropped in memory and never cached, logged, returned or forwarded. Polling never calls `/api/credential`.
- Inspection never starts the service: `hgs account ls`, `hgs account models`, `hgs account favorite`, `hgs inspect` and every client poll work with the service stopped and report `not_running`.
- Favorites are stored on the agent computer beside the hgs account catalog (`<hgs config dir>/opencode-favorites.json`, under `accounts.lock`), at most 64 model IDs. A favorite the catalog no longer lists is kept until removed.
- Bounded reads and outputs: at most 10,000 models in the full catalog and at most 4 MiB of `hgs account models` output; at most 64 providers and 256 short-list entries on the row.
- The `native-opencode` row has a 32 KiB budget measured as `len(json.dumps(row, ensure_ascii=True))` (the connector's `python_default_len(row, true)`). Over budget hgs drops short-list entries from the end — free, then configured, then favorites — keeping the default model, then providers beyond the first 16, and reports `model_options_truncated` / `providers_truncated`. Every short-list entry keeps `value` (equal to `id`), because the connector and its launch check keep only entries with `value`.
- Every favorites write is an explicit user action carrying a request identity (UUID). A retried identity returns its first receipt; an interrupted change is never replayed over a newer state. Tests never write the owner's data: Rust tests use temporary paths, CLI tests a temporary `HOME`/`HGS_CONFIG_DIR`, native tests private roots, client tests fake `hgs` programs.
- Android work must not touch `ZerusViewModel.observeScheduledSettings`, `ZerusViewModel.settleSessionAction`, `QuestionUi` or `QuestionPolicies` (another lane owns them). This plan needs no change there; if an implementer finds one is needed, stop and report the seam instead of editing.
- Phones are read-only for providers and favorites: they show providers, marks, truncation and catalog state, and choose from the short list; they never star, connect or copy.
- Windows: `hgs` builds for `x86_64-pc-windows-msvc` without warnings. New items used only by `#[cfg(unix)]` accounts code are re-exported under `#[cfg(unix)]`, and the new modules carry `#[cfg_attr(not(unix), allow(dead_code))]`; check with `RUSTUP_TOOLCHAIN=1.99.1 bash scripts/ci/check-windows-target.sh clippy`.
- Privacy guard: plans, fixtures and tests use synthetic paths (`/home/example/...`, `/workspace/...`, `/srv/...`) and reserved example domains only; `python3 scripts/check-source-privacy.py` reports nothing for any file this plan adds or changes.
- Mixed fleets: a computer may run an older `hgs`. Clients call `account models` and `account favorite` only when the OpenCode row (or session details) carries `model_options_truncated`, which every `hgs` with these commands reports; otherwise they show "Update Zerus on %1 to search all models and star favorites."

## Review Focus

- An OpenCode model ID that itself contains `/` (Groq lists `openai/gpt-oss-20b`; a configuration can declare `vendor/slash`): the adapter must list it, launch it and change to it as `provider/openai/gpt-oss-20b`, splitting only at the first `/`. Today's `safe_id` silently drops these models. Tests owned by Task 2 (unit) and Task 12 (native).
- A provider, model or connection that carries `settings.apiKey`, `headers.Authorization`, a base URL or a connection `status.url`/`message`: nothing of it may reach the row, `hgs account models`, the cache, an error or the connector projection. Tests owned by Task 1 (fixture), Task 2 (hgs) and Task 6 (connector).
- A favorite change whose response was lost and that the desktop retries after the user toggled the star again: the retry must return its first receipt and must not re-apply the older state; a change interrupted between its receipt and the file write must refuse instead of replaying. Test owned by Task 3.
- A computer whose catalog would make the OpenCode row larger than 32 KiB (many favorites and free models, many providers, long non-ASCII labels): phones must still receive every account; the default model stays first and the row says what was cut. Tests owned by Task 4a (hgs) and Task 6 (connector).
- OpenCode stopped while New session or the model picker is open: nothing may start the service, the last known short list stays, the reason is shown and a star still works because favorites live in hgs. Tests owned by Task 8b (desktop), Task 4b (cache round trip) and Task 12 (native).

## Frozen contract (additive)

`native-opencode` row of `hgs account ls --json` (existing keys unchanged):

- `model_options[]`: the short list for the home folder, in order default model, favorites (stored order), configured models (service order), free models (service order); each model appears once; at most 256. Entry: `id`, `value` (`== id`, `provider/model`, split at the first `/`), `label`, `provider`, `provider_label`, `free`, `effort_options`, `effort_labels`, plus `favorite: true` and `default: true` only when true.
- `model_options_truncated` (bool), `providers_truncated` (bool): always present.
- `providers[]` (≤64, service order): `id`, `label`, `kind` (one of `free`, `key`, `oauth`, `env`, `config`, `command`, `external`, `local`), `credential_count`, `model_count`, and `active_label` only when exactly one credential connection exists.
- `settings_model`, `catalog_state`, `catalog_reason`, `catalog_at`: unchanged meaning.

`hgs [@host] account models native-opencode [--location DIR] --json` (desktop only): the row keys above for that folder plus `location`, `model_catalog_source`, `models[]` (full catalog, same entry shape with `favorite`/`default` marks, ≤10,000, output ≤4 MiB), `models_truncated`, `favorites[]` (`{"value", "available"}`, `available` only on a live answer), and `folder: true` on providers that the home folder does not list.

`hgs [@host] account favorite native-opencode --model PROVIDER/MODEL --favorite on|off --request-id UUID [--json]`: prints `{"status":"applied","request_id","model","favorite","favorites":[…]}`.

Session details (`hgs inspect` of an OpenCode session, full detail): `model_options` becomes the short list of the session's folder (same entry shape) with the session's current model, and a pending model if any, right after the default — so its efforts stay offered even when it is neither default, favorite, configured nor free — plus `model_options_truncated`.

Support detection: an OpenCode row or session details without `model_options_truncated` come from an older `hgs` without `account models` and `account favorite`. `hgs --help` lists both commands.

Connector (`mobile_accounts` heartbeat): OpenCode rows add `providers` (validated, ≤64), `providers_truncated`, `model_options_truncated`, `catalog_at`, and per model `favorite`/`default` (only `true`). The launch catalog adds the same model marks, `model_options_truncated` and `catalog_at`, but no providers. Over 128 KiB the connector drops `effort_labels`, then `effort_options`, then `providers` (setting `providers_truncated`) before refusing.

Connection kind, from `/api/integration` (looked up by the provider's `integrationID`, else its `id`): a credential connection → its `method` (`key`, `oauth`, `external`, `command`; first listed, which OpenCode lists newest first); else an `env` connection → `env`; else a provider without `integrationID` → `local` when no integration has its ID, otherwise `config`; else every listed model free → `free`; else `config`. Without a readable integration list, kinds use the last three rules only and `credential_count` is 0. Configured models are those of providers without `integrationID` (declared in configuration or detected locally). Free means every `cost` element has zero `input` and `output`.

## File map

hgs:
- Create `src/state/opencode/providers.rs` — provider labels, configured set, allowlisted provider rows and kinds.
- Create `src/state/opencode/shortlist.rs` — short-list builder, `ascii_len`, the 32 KiB `fit_row`.
- Create `src/state/opencode/favorites.rs` — favorites file, receipts, `set_favorite`.
- Modify `src/state/opencode/catalog.rs` — model ID rule, full projection, `Snapshot`, `row_from`, cache v2, `catalog()`, `session_options()`, `models()`.
- Modify `src/state/opencode.rs` — module declarations, `#[cfg(unix)]` re-exports, session details.
- Modify `src/state/opencode/service.rs` — `Service::call_within` (an optional read with its own deadline).
- Modify `src/accounts.rs` — row keys, `fit_row`, `models` and `favorite` operations, usage text.
- Modify `src/cli.rs` — `hgs --help` lines for `account models` and `account favorite`, and their test.

Shared fixture:
- Create `tests/fixtures/opencode-provider-catalog.py` (generator), `tests/fixtures/opencode-provider-catalog.json`, `mobile/android/app/src/test/resources/opencode-provider-catalog.json`, `tests/test_opencode_provider_fixture.py`.

Connector:
- Modify `services/mobile/connector/src/accounts.rs`; create `services/mobile/connector/tests/opencode_providers.rs`.

Desktop:
- Create `tray/src/OpenCodeModels.{h,cpp}` (pure parsing, grouping, search), `tray/src/OpenCodeModelPicker.{h,cpp}` (grouped list with search, marks, stars, All models).
- Modify `tray/CMakeLists.txt`, `tray/src/HgsClient.{h,cpp}`, `tray/src/AccountsPage.{h,cpp}`, `tray/src/NewSessionDialog.{h,cpp}`, `tray/src/MessageComposer.{h,cpp}`, `tray/src/SessionsWindow.{h,cpp}`.
- Tests: create `tray/tests/test_opencodemodelpicker.cpp`; modify `tray/tests/CMakeLists.txt`, `test_accountspage.cpp`, `test_newsessiondialog.cpp`, `test_messagecomposer.cpp`, `test_sessionswindow.cpp`.
- Tasks: 7 (picker), 8a (Accounts), 8b (HgsClient and New session), 9 (composer).

Android (`mobile/android/app/src/main/java/app/zerus/mobile/`):
- Create `OpenCodeCatalog.kt`, `OpenCodeModelListUi.kt`.
- Modify `LaunchPresentation.kt`, `LaunchUi.kt`, `SessionDetailsPresentation.kt`, `NativeSettingsUi.kt`, `AccountSnapshots.kt`, `AccountsUi.kt`.
- Tests: create `OpenCodeCatalogTest.kt`; modify `LaunchPresentationTest.kt`, `AccountSnapshotsTest.kt`, `OpenCodePresentationTest.kt`.

iOS (`mobile/ios/`):
- Create `ZerusKit/Sources/ZerusKit/OpenCodeCatalog.swift`, `Zerus/Common/OpenCodeModelList.swift`.
- Modify `ZerusKit/Sources/ZerusKit/{Launch,SessionDetails,Accounts}.swift`, `Zerus/Launch/LaunchView.swift`, `Zerus/Conversation/SessionSheets.swift`, `Zerus/Accounts/AccountsScreen.swift`.
- Tests: create `ZerusKit/Tests/ZerusKitTests/OpenCodeCatalogTests.swift`; modify `LaunchTests.swift`, `AccountsTests.swift`, `SessionControlsTests.swift`.

Qualification and docs:
- Modify `tests/test_accounts.py`, `tests/test_opencode_native.py`, `docs/opencode.md`, `docs/reference.md`, `docs/mobile-architecture.md`.

Task order: Task 1 first. hgs runs 2 → 3 → 4a → 4b → 5, then Task 12 (native qualification) right after Task 5: it needs only Tasks 2–5 and is the first test of the stopped-service cache and of slash IDs against a real OpenCode, so a contract defect surfaces before the client tasks build on it. Task 6 (connector) and Tasks 10 and 11 (phones) need only Task 1 and can run in parallel lanes. The desktop runs 7 → 8a → 8b → 9 and needs Task 1; its end-to-end behavior needs Tasks 3 and 5 only at qualification. Task 13 last. Tasks 4 and 8 are split into a/b parts so each commit can be reviewed alone; the other task numbers are unchanged.

---

### Task 1: Freeze the shared providers contract fixture

**Files:**
- Create: `tests/fixtures/opencode-provider-catalog.py`
- Create (generated): `tests/fixtures/opencode-provider-catalog.json`, `mobile/android/app/src/test/resources/opencode-provider-catalog.json`
- Test: `tests/test_opencode_provider_fixture.py`

**Interfaces:**
- Produces: the fixture keys every later task reads — `service` (raw OpenCode 2.0.32-shaped `provider`, `model`, `default`, `integration` responses with deliberate secrets), `favorites`, `row` (hgs row catalog keys for home), `hgs_account` (complete hgs row as the connector reads it), `heartbeat_account` and `launch_account` (connector projections), `stopped_account` (truncated row of a stopped service), `ascii_lengths` (`{"value", "length"}` pairs).

- [ ] **Step 1: Write the failing test**

`tests/test_opencode_provider_fixture.py`:

```python
"""The OpenCode providers contract fixture: generated, private and byte-identical across copies."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FIXTURE = REPO / 'tests/fixtures/opencode-provider-catalog.json'
ANDROID = REPO / 'mobile/android/app/src/test/resources/opencode-provider-catalog.json'
GENERATOR = REPO / 'tests/fixtures/opencode-provider-catalog.py'
OUTPUTS = ('row', 'hgs_account', 'heartbeat_account', 'launch_account', 'stopped_account')
SECRETS = ('fixture-secret-key', '127.0.0.1', 'example.invalid', 'Authorization', 'baseURL', '"settings"',
           '"headers"', '"body"', 'cred_fixture', 'needs_auth', 'GROQ_API_KEY')


class ProviderCatalogFixture(unittest.TestCase):
    def setUp(self):
        self.data = json.loads(FIXTURE.read_text())

    def test_the_generator_reproduces_both_committed_copies(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'fixture.json'
            subprocess.run([sys.executable, '-I', str(GENERATOR), str(out)], check=True, cwd=REPO)
            self.assertEqual(out.read_bytes(), FIXTURE.read_bytes())
        self.assertEqual(ANDROID.read_bytes(), FIXTURE.read_bytes())

    def test_outputs_never_carry_service_secrets(self):
        self.assertIn('fixture-secret-key', json.dumps(self.data['service']))
        for key in OUTPUTS:
            text = json.dumps(self.data[key])
            for secret in SECRETS:
                self.assertNotIn(secret, text, key)

    def test_ascii_lengths_are_python_json_lengths(self):
        self.assertEqual(len(self.data['ascii_lengths']), 5)
        for case in self.data['ascii_lengths']:
            self.assertEqual(len(json.dumps(case['value'], ensure_ascii=True)), case['length'])

    def test_short_list_order_marks_and_kinds(self):
        options = self.data['row']['model_options']
        self.assertEqual([o['value'] for o in options], [
            'fixture/local', 'keyed/paid-model', 'groq/openai/gpt-oss-20b', 'fixture/alternate',
            'ollama/qwen3-coder:30b', 'opencode/glm-5.3-flash-free', 'opencode/big-pickle', 'groq/groq/compound'])
        self.assertEqual([o['value'] for o in options if o.get('default')], ['fixture/local'])
        self.assertEqual([o['value'] for o in options if o.get('favorite')],
                         ['keyed/paid-model', 'groq/openai/gpt-oss-20b'])
        self.assertNotIn('gone/missing', json.dumps(options))
        self.assertTrue(all(o['id'] == o['value'] for o in options))
        providers = self.data['row']['providers']
        self.assertEqual({p['id']: p['kind'] for p in providers},
                         {'opencode': 'free', 'groq': 'env', 'fixture': 'config', 'ollama': 'local',
                          'keyed': 'key', 'solo': 'oauth'})
        self.assertEqual([p.get('active_label') for p in providers], [None, None, None, None, None, 'Work'])


if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `PYTHONPATH=tests python3 -m unittest test_opencode_provider_fixture -v`
Expected: ERROR `FileNotFoundError` for `opencode-provider-catalog.json`.

- [ ] **Step 3: Write the generator and generate both copies**

`tests/fixtures/opencode-provider-catalog.py` (the reference model of the phase 1 rules; keep it byte for byte):

```python
#!/usr/bin/env python3
"""Regenerates the OpenCode providers phase 1 contract fixture.

Run from the repository root: python3 -I tests/fixtures/opencode-provider-catalog.py
It writes tests/fixtures/opencode-provider-catalog.json and the byte-identical Android
test copy (which the iOS tests read). With one argument it writes only that file.

The expected values come from this reference model of the phase 1 rules in
docs/superpowers/plans/2026-10-18-opencode-providers-phase-1.md; hgs, the Rust
connector, the desktop, Android and iOS must agree with it. Change a rule here and in
every implementation together."""
import json
import sys
from pathlib import Path

SECRET = 'fixture-secret-key'
HOME = '/home/example'
ZERO = [{'input': 0, 'output': 0, 'cache': {'read': 0, 'write': 0}}]
HIDDEN = {'settings': {'apiKey': SECRET, 'baseURL': 'http://127.0.0.1:9/v1'},
          'headers': {'Authorization': 'Bearer ' + SECRET}, 'body': {'token': SECRET}}


def paid(i, o):
    return [{'input': i, 'output': o, 'cache': {'read': 0, 'write': 0}}]


def model(provider, mid, name, cost, variants=(), enabled=True, hidden=False):
    m = {'id': mid, 'modelID': mid, 'providerID': provider, 'name': name,
         'capabilities': {'tools': True, 'input': ['text'], 'output': ['text']},
         'variants': [{'id': v, 'settings': {'reasoningEffort': v}} for v in variants],
         'time': {'released': 1790000000000}, 'cost': cost, 'status': 'active', 'enabled': enabled,
         'limit': {'context': 131072, 'output': 8192}}
    if hidden:
        m.update(HIDDEN)
    return m


MODELS = [
    model('opencode', 'glm-5.3-flash-free', 'GLM-5.3-Flash Free', ZERO, ['low', 'high', 'max']),
    model('opencode', 'big-pickle', 'Big Pickle', ZERO),
    model('groq', 'openai/gpt-oss-20b', 'GPT OSS 20B', paid(0.075, 0.3), ['low', 'medium', 'high']),
    model('groq', 'groq/compound', 'Compound', ZERO),
    model('fixture', 'local', 'Native fixture', [], ['low', 'medium', 'high'], hidden=True),
    model('fixture', 'alternate', 'Alternate fixture', [], hidden=True),
    model('ollama', 'qwen3-coder:30b', 'qwen3-coder:30b', []),
    model('keyed', 'paid-model', 'Paid model', paid(3, 15)),
    model('keyed', 'off', 'Disabled model', paid(1, 1), enabled=False),
    model('solo', 'solo-model', 'Solo model', paid(1, 2)),
]
PROVIDERS = [
    {'id': 'opencode', 'integrationID': 'opencode', 'name': 'OpenCode', 'activation': 'enabled',
     'package': '@opencode/ai/providers/openai-compatible',
     'settings': {'apiKey': 'public', 'baseURL': 'https://opencode.example.invalid/zen/v1'}},
    {'id': 'groq', 'integrationID': 'groq', 'name': 'Groq', 'activation': 'auto', 'package': '@opencode/ai/providers/groq'},
    {'id': 'fixture', 'name': 'Private keyless fixture', 'activation': 'enabled',
     'package': '@opencode/ai/providers/openai-compatible', **HIDDEN},
    {'id': 'ollama', 'name': 'Ollama', 'activation': 'enabled', 'package': '@opencode/ai/providers/openai-compatible',
     'settings': {'baseURL': 'http://127.0.0.1:11434/v1', 'provider': 'ollama'}},
    {'id': 'keyed', 'integrationID': 'keyed', 'name': 'Keyed fixture', 'activation': 'enabled',
     'package': '@opencode/ai/providers/openai-compatible'},
    {'id': 'solo', 'integrationID': 'solo', 'name': 'Solo fixture', 'activation': 'enabled',
     'package': '@opencode/ai/providers/openai-compatible'},
]
INTEGRATIONS = [
    {'id': 'anthropic', 'name': 'Anthropic', 'methods': [{'type': 'key'}, {'type': 'env', 'names': ['ANTHROPIC_API_KEY']}],
     'connections': []},
    {'id': 'opencode', 'name': 'OpenCode Console', 'methods': [{'type': 'key', 'label': 'API key (service account)'}],
     'connections': []},
    {'id': 'groq', 'name': 'Groq', 'methods': [{'type': 'key'}, {'type': 'env', 'names': ['GROQ_API_KEY']}],
     'connections': [{'type': 'env', 'name': 'GROQ_API_KEY'}]},
    {'id': 'fixture', 'name': 'Private keyless fixture', 'methods': [{'type': 'key', 'label': 'Manually enter API Key'}],
     'connections': []},
    {'id': 'keyed', 'name': 'Keyed fixture', 'methods': [{'type': 'key'}], 'connections': [
        {'type': 'credential', 'id': 'cred_fixture_team', 'label': 'Team', 'method': 'key'},
        {'type': 'credential', 'id': 'cred_fixture_personal', 'label': 'Personal', 'method': 'key'}]},
    {'id': 'solo', 'name': 'Solo fixture', 'methods': [{'id': 'device', 'type': 'oauth', 'label': 'Sign in'}], 'connections': [
        {'type': 'credential', 'id': 'cred_fixture_work', 'label': 'Work', 'method': 'oauth',
         'status': {'status': 'needs_auth', 'message': 'Sign in again with ' + SECRET,
                    'url': 'https://auth.example.invalid/device?code=' + SECRET}}]},
]
DEFAULT = model('fixture', 'local', 'Native fixture', [], ['low', 'medium', 'high'], hidden=True)
FAVORITES = ['keyed/paid-model', 'groq/openai/gpt-oss-20b', 'gone/missing']
CATALOG_AT = 1792267200.5


def free(cost):
    return isinstance(cost, list) and bool(cost) and all(c.get('input') == 0 and c.get('output') == 0 for c in cost)


def build():
    labels = {p['id']: p['name'] for p in PROVIDERS}
    full = []
    for m in MODELS:
        if m['enabled'] is False:
            continue
        value = m['providerID'] + '/' + m['id']
        efforts = [v['id'] for v in m['variants']]
        full.append({'id': value, 'value': value, 'label': m['name'], 'provider': m['providerID'],
                     'provider_label': labels.get(m['providerID'], m['providerID']), 'free': free(m['cost']),
                     'effort_options': efforts, 'effort_labels': efforts})
    by_value = {e['value']: e for e in full}
    configured = {p['id'] for p in PROVIDERS if not p.get('integrationID')}
    default = DEFAULT['providerID'] + '/' + DEFAULT['id']
    short = []
    candidates = ([by_value[default]] + [by_value[f] for f in FAVORITES if f in by_value]
                  + [e for e in full if e['provider'] in configured] + [e for e in full if e['free']])
    for entry in candidates:
        if all(e['value'] != entry['value'] for e in short):
            short.append(dict(entry))
    for entry in short:
        if entry['value'] in FAVORITES:
            entry['favorite'] = True
        if entry['value'] == default:
            entry['default'] = True
    integrations = {i['id']: i for i in INTEGRATIONS}
    providers = []
    for p in PROVIDERS:
        integration = integrations.get(p.get('integrationID') or p['id'])
        connections = (integration or {}).get('connections', [])
        credentials = [c for c in connections if c.get('type') == 'credential']
        own = [e for e in full if e['provider'] == p['id']]
        if credentials:
            kind = {'oauth': 'oauth', 'external': 'external', 'command': 'command'}.get(credentials[0]['method'], 'key')
        elif any(c.get('type') == 'env' for c in connections):
            kind = 'env'
        elif not p.get('integrationID'):
            kind = 'local' if integration is None else 'config'
        elif own and all(e['free'] for e in own):
            kind = 'free'
        else:
            kind = 'config'
        row = {'id': p['id'], 'label': p['name'], 'kind': kind, 'credential_count': len(credentials), 'model_count': len(own)}
        if len(credentials) == 1:
            row['active_label'] = credentials[0]['label']
        providers.append(row)
    row = {'settings_model': default, 'model_options': short, 'providers': providers,
           'model_options_truncated': False, 'providers_truncated': False}

    def option(e):
        o = {'value': e['value'], 'label': e['label'], 'provider': e['provider'], 'provider_label': e['provider_label'],
             'free': e['free']}
        if e.get('favorite'):
            o['favorite'] = True
        if e.get('default'):
            o['default'] = True
        if e['effort_options']:
            o['effort_options'] = e['effort_options'][:8]
            o['effort_labels'] = e['effort_labels'][:8]
        return o

    base = {'id': 'native-opencode', 'provider': 'opencode', 'label': 'Native OpenCode configuration',
            'native': True, 'installed': True, 'is_default': True}
    usage = {'status': 'unavailable', 'identity': {}, 'windows': [], 'refreshing': False, 'refresh_error': False}
    options = [option(e) for e in short]
    hgs_account = {**base, 'catalog_state': 'ready', 'catalog_reason': '', 'catalog_at': CATALOG_AT, **row}
    heartbeat = {**base, 'model_options': options, 'catalog_state': 'ready', 'catalog_at': CATALOG_AT,
                 'model_options_truncated': False, 'providers': providers, 'providers_truncated': False,
                 'usage': {**usage, 'stale': True}}
    launch = {'id': base['id'], 'provider': base['provider'], 'label': base['label'], 'native': True, 'is_default': True,
              'installed': True, 'auth_status': 'unknown', 'model_options': options, 'catalog_state': 'ready',
              'catalog_at': CATALOG_AT, 'model_options_truncated': False}
    stopped = {**base, 'model_options': options[:1], 'catalog_state': 'not_running',
               'catalog_reason': 'OpenCode is not running on fixture', 'catalog_at': CATALOG_AT,
               'model_options_truncated': True, 'providers': providers[:2], 'providers_truncated': True,
               'usage': {**usage, 'stale': True}}
    lengths = [{'value': v} for v in (
        {'a': 'x'},
        ['café', '\U0001F600', '\u0000\u001f', '"\\', '\u007f', '\n\t\r\b\f', 1, 1.5, True, None, [], {}],
        {'label': '界' * 3, 'nested': {'k': [0, -2, CATALOG_AT]}},
        hgs_account,
        heartbeat,
    )]
    for case in lengths:
        case['length'] = len(json.dumps(case['value'], ensure_ascii=True))
    return {
        'version': 1,
        'description': ('OpenCode providers phase 1 contract shared by hgs, the Rust connector, the desktop, Android '
                        'and iOS; regenerate with tests/fixtures/opencode-provider-catalog.py. service holds raw '
                        'OpenCode 2.0.32-shaped responses that deliberately carry secrets; favorites is the stored '
                        'favorites list; row is what hgs adds to the native-opencode account row for the home '
                        'location; hgs_account is that complete row as the connector reads it; heartbeat_account '
                        'and launch_account are the connector projections of hgs_account; stopped_account is a '
                        'truncated row of a stopped service; ascii_lengths pins len(json.dumps(value, '
                        'ensure_ascii=True)), the measure of the 32 KiB row budget and the 128 KiB projection.'),
        'service': {'location': HOME, 'provider': {'location': {'directory': HOME}, 'data': PROVIDERS},
                    'model': {'location': {'directory': HOME}, 'data': MODELS},
                    'default': {'location': {'directory': HOME}, 'data': DEFAULT},
                    'integration': {'location': {'directory': HOME}, 'data': INTEGRATIONS}},
        'favorites': FAVORITES,
        'row': row,
        'hgs_account': hgs_account,
        'heartbeat_account': heartbeat,
        'launch_account': launch,
        'stopped_account': stopped,
        'ascii_lengths': lengths,
    }


def main():
    text = json.dumps(build(), indent=2, ensure_ascii=True) + '\n'
    targets = [Path(sys.argv[1])] if len(sys.argv) > 1 else [
        Path('tests/fixtures/opencode-provider-catalog.json'),
        Path('mobile/android/app/src/test/resources/opencode-provider-catalog.json')]
    for target in targets:
        target.write_text(text, encoding='ascii')


if __name__ == '__main__':
    main()
```

Run: `python3 -I tests/fixtures/opencode-provider-catalog.py`
Expected: both JSON copies written (about 41 KB each). `ascii_lengths` come out as `[10, 105, 71, 2778, 2398]`.

- [ ] **Step 4: Run the test to verify it passes**

Run: `PYTHONPATH=tests python3 -m unittest test_opencode_provider_fixture -v && python3 scripts/check-source-privacy.py`
Expected: 4 tests PASS; the privacy check reports no finding for the files of this task (the fixture uses `/home/example`, reserved example domains, loopback and a synthetic key only). An untracked file of another lane in the same worktree (for example a phase 2 plan draft) can report its own findings; it is not part of this task.

- [ ] **Step 5: Commit**

```bash
git add tests/fixtures/opencode-provider-catalog.py tests/fixtures/opencode-provider-catalog.json \
  mobile/android/app/src/test/resources/opencode-provider-catalog.json tests/test_opencode_provider_fixture.py
git commit -m "Freeze the shared OpenCode providers contract fixture"
```

---

### Task 2: hgs — allowlisted full catalog, provider kinds and model IDs with `/`

**Files:**
- Create: `src/state/opencode/providers.rs`
- Modify: `src/state/opencode/catalog.rs` (ID rule, `project_all`, `Snapshot`, `snapshot_from`, `read_snapshot`, `include_selected`, `model_ref`, `offered_model`, `catalog()` reads through the snapshot), `src/state/opencode/service.rs` (`Service::call_within`), `src/state/opencode.rs` (declare `#[cfg_attr(not(unix), allow(dead_code))] mod providers;` beside `mod catalog;`; in its test module add `use std::collections::HashMap;` and update the tests that call `catalog::project`)
- Test: unit tests in `catalog.rs` and `providers.rs`

**Interfaces:**
- Consumes: `connect`, `Service::call` (adapter phase 1); the Task 1 fixture.
- Produces (catalog.rs):
  - `pub(super) fn safe_id(value: &str) -> bool` (now `pub(super)`, unchanged rule: provider IDs and variant IDs)
  - `pub(super) fn label_of<'a>(value: &'a Value, fallback: &'a str) -> &'a str` (now `pub(super)`)
  - `pub(super) fn model_value(provider: &str, id: &str) -> Option<String>`: provider passes `safe_id`; the model ID is non-empty, ≤200 bytes, without whitespace or control characters and may contain `/`; the value is ≤256 bytes.
  - `pub(super) const MAX_FULL: usize = 10_000;`
  - `pub(super) fn project_all(models: &Value, labels: &HashMap<String, String>) -> (Vec<Value>, bool)`: allowlisted entries `{"id","value","label","provider","provider_label","free","effort_options","effort_labels"}` in service order, distinct, enabled only; `true` when the bound cut the list.
  - `pub(super) struct Snapshot { pub models: Vec<Value>, pub models_truncated: bool, pub default: String, pub configured: HashSet<String>, pub providers: Vec<Value>, pub providers_truncated: bool, pub loaded: bool }`
  - `pub(super) fn snapshot_from(providers: &Value, models: &Value, default: &Value, integrations: Option<&Value>) -> Snapshot` (pure)
  - `pub(super) fn read_snapshot(service: &Service, location: &Path, with_providers: bool) -> Result<Snapshot>` (reads `/api/provider`, `/api/model`, `/api/model/default`, and `/api/integration` only when `with_providers`; an integration failure leaves approximate kinds and never fails the snapshot)
  - `pub(super) fn call_within(&self, method: &str, path: &str, body: Option<&Value>, limit: Duration) -> Result<Value>` on `Service` (service.rs): `call` with its own deadline
  - `model_ref(rows, value, effort)` keeps its signature and accepts `/` in the model ID; `offered_model(service, location, value, effort)` validates against `read_snapshot(service, location, false)?.models` (the full list, ≤10,000 plus the default).
- Produces (providers.rs):
  - `pub(super) const MAX_PROVIDERS: usize = 64;`
  - `pub(super) const KINDS: [&str; 8] = ["free", "key", "oauth", "env", "config", "command", "external", "local"];`
  - `pub(super) fn labels(providers: &Value) -> HashMap<String, String>`
  - `pub(super) fn configured(providers: &Value) -> HashSet<String>`
  - `pub(super) fn project(providers: &Value, integrations: Option<&Value>, models: &[Value]) -> (Vec<Value>, bool)`

- [ ] **Step 1: Write the failing tests**

Add to the `tests` module of `src/state/opencode/catalog.rs` (keep `fixture()`; add `use std::collections::HashMap;` if missing):

```rust
    fn shared() -> Value {
        serde_json::from_slice(include_bytes!(
            "../../../tests/fixtures/opencode-provider-catalog.json"
        ))
        .unwrap()
    }

    #[test]
    fn model_ids_may_contain_a_slash_but_providers_may_not() {
        assert_eq!(model_value("groq", "openai/gpt-oss-20b").as_deref(), Some("groq/openai/gpt-oss-20b"));
        assert_eq!(model_value("ollama", "qwen3-coder:30b").as_deref(), Some("ollama/qwen3-coder:30b"));
        let (max_id, long_id, long_provider) = ("m".repeat(200), "m".repeat(201), "p".repeat(60));
        assert_eq!(model_value("p", &max_id).map(|v| v.len()), Some(202));
        for (provider, id) in [("a/b", "c"), ("p", ""), ("p", "a b"), ("p", "a\u{7}"), ("", "m"),
                               ("p", long_id.as_str()), (long_provider.as_str(), max_id.as_str())] {
            assert!(model_value(provider, id).is_none(), "{provider}/{id}");
        }
        let rows = project_all(&shared()["service"]["model"], &HashMap::new()).0;
        assert_eq!(
            model_ref(&rows, "groq/openai/gpt-oss-20b", "high").unwrap(),
            json!({"providerID": "groq", "id": "openai/gpt-oss-20b", "variant": "high"})
        );
        assert!(model_ref(&rows, "groq/openai/gpt-oss-20b", "max").is_err());
        assert!(model_ref(&rows, "keyed/off", "").is_err());
    }

    #[test]
    fn the_full_catalog_is_allowlisted_and_skips_disabled_models() {
        let service = &shared()["service"];
        let labels = super::super::providers::labels(&service["provider"]);
        let (rows, truncated) = project_all(&service["model"], &labels);
        assert!(!truncated);
        assert_eq!(rows.len(), 9);
        let text = serde_json::to_string(&rows).unwrap();
        for secret in ["fixture-secret-key", "baseURL", "\"settings\"", "\"headers\"", "Authorization",
                       "127.0.0.1", "example.invalid"] {
            assert!(!text.contains(secret), "{secret} leaked: {text}");
        }
        let groq = rows.iter().find(|r| r["value"] == "groq/openai/gpt-oss-20b").unwrap();
        assert_eq!(groq["id"], groq["value"]);
        assert_eq!(groq["provider_label"], "Groq");
        assert_eq!(groq["effort_options"], json!(["low", "medium", "high"]));
        assert_eq!(rows.iter().find(|r| r["value"] == "groq/groq/compound").unwrap()["free"], true);
        assert_eq!(rows.iter().find(|r| r["value"] == "ollama/qwen3-coder:30b").unwrap()["free"], false);
        assert!(rows.iter().all(|r| r["value"] != "keyed/off"));
    }

    #[test]
    fn the_full_catalog_is_bounded_at_ten_thousand_models() {
        let mut many = json!({"data": []});
        for i in 0..10_050 {
            many["data"].as_array_mut().unwrap().push(
                json!({"id": format!("m{i}"), "providerID": "p", "name": "M", "enabled": true}),
            );
        }
        let (rows, truncated) = project_all(&many, &HashMap::new());
        assert_eq!((rows.len(), truncated), (MAX_FULL, true));
        // The default beyond the bound is still offered, replacing the last row.
        let snapshot = snapshot_from(
            &json!({"data": [{"id": "p", "name": "P"}]}),
            &many,
            &json!({"data": {"id": "m10040", "providerID": "p", "settings": {"apiKey": "fixture-secret-key"}}}),
            None,
        );
        assert_eq!(snapshot.models.len(), MAX_FULL);
        assert_eq!(snapshot.default, "p/m10040");
        assert!(snapshot.models.iter().any(|r| r["value"] == "p/m10040"));
        assert!(model_ref(&snapshot.models, "p/m10040", "").is_ok());
        assert!(!serde_json::to_string(&snapshot.models).unwrap().contains("fixture-secret-key"));
    }
```

Delete `projection_is_bounded_and_skips_invalid_ids`, `selected_model_beyond_the_bound_is_still_offered` and `shared_rows_list_and_accept_a_default_beyond_the_bound` (replaced by the test above). In the remaining tests of `catalog.rs` and in `src/state/opencode.rs` (search `catalog::project` / `project(&`) replace `project(&x)` with `project_all(&x, &HashMap::new()).0`.

Add `src/state/opencode/providers.rs` with only its test module first:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    fn shared() -> Value {
        serde_json::from_slice(include_bytes!(
            "../../../tests/fixtures/opencode-provider-catalog.json"
        ))
        .unwrap()
    }

    #[test]
    fn kinds_counts_and_labels_follow_the_shared_contract() {
        let fixture = shared();
        let service = &fixture["service"];
        let models = super::super::catalog::project_all(&service["model"], &labels(&service["provider"])).0;
        let (rows, truncated) = project(&service["provider"], Some(&service["integration"]), &models);
        assert!(!truncated);
        assert_eq!(json!(rows), fixture["row"]["providers"]);
        let text = serde_json::to_string(&rows).unwrap();
        for secret in ["fixture-secret-key", "needs_auth", "cred_fixture", "GROQ_API_KEY", "example.invalid", "127.0.0.1"] {
            assert!(!text.contains(secret), "{secret} leaked: {text}");
        }
    }

    #[test]
    fn without_integrations_kinds_are_approximate_but_never_fail() {
        let fixture = shared();
        let service = &fixture["service"];
        let models = super::super::catalog::project_all(&service["model"], &labels(&service["provider"])).0;
        let (rows, _) = project(&service["provider"], None, &models);
        let kinds: Vec<&str> = rows.iter().map(|r| r["kind"].as_str().unwrap()).collect();
        assert_eq!(kinds, ["free", "config", "config", "config", "config", "config"]);
        assert!(rows.iter().all(|r| r["credential_count"] == 0 && r.get("active_label").is_none()));
    }

    #[test]
    fn providers_are_bounded_and_identities_checked() {
        let mut providers = json!({"data": [{"id": "bad id", "name": "X"}, {"id": "a/b", "name": "Y"}]});
        for i in 0..70 {
            providers["data"].as_array_mut().unwrap().push(
                json!({"id": format!("p{i}"), "name": format!("P{i}"), "integrationID": format!("p{i}")}),
            );
        }
        let (rows, truncated) = project(&providers, Some(&json!({"data": []})), &[]);
        assert_eq!((rows.len(), truncated), (MAX_PROVIDERS, true));
        assert_eq!(rows[0]["id"], "p0");
        // A control character in a name falls back to the ID; an overlong label is omitted.
        let one = json!({"data": [{"id": "q", "name": "bad\u{1b}[1m", "integrationID": "q"}]});
        let integrations = json!({"data": [{"id": "q", "name": "Q", "methods": [], "connections": [
            {"type": "credential", "id": "c", "label": "x".repeat(81), "method": "key"}]}]});
        let (rows, _) = project(&one, Some(&integrations), &[]);
        assert_eq!(rows[0]["label"], "q");
        assert_eq!((rows[0]["kind"].as_str(), rows[0]["credential_count"].as_u64()), (Some("key"), Some(1)));
        assert!(rows[0].get("active_label").is_none());
    }

    #[test]
    fn configured_providers_are_those_without_an_integration() {
        let fixture = shared();
        let mut configured: Vec<String> = configured(&fixture["service"]["provider"]).into_iter().collect();
        configured.sort();
        assert_eq!(configured, ["fixture", "ollama"]);
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cargo +1.99.1 test --locked opencode::`
Expected: compile errors (`model_value`, `project_all`, `snapshot_from`, `MAX_FULL`, `providers::labels`, `project`, `configured` not defined).

- [ ] **Step 3: Implement**

`src/state/opencode/providers.rs` (above the test module):

```rust
//! OpenCode 2 providers for the account row: identity, a connection kind, the
//! number of credential connections and of listed models. Provider settings,
//! headers, request bodies, base URLs, connection status messages and URLs,
//! credential IDs and values never leave this module.
use super::super::*;
use super::catalog::{label_of, safe_id};
use std::collections::{HashMap, HashSet};

pub(super) const MAX_PROVIDERS: usize = 64;
pub(super) const KINDS: [&str; 8] = ["free", "key", "oauth", "env", "config", "command", "external", "local"];
const MAX_INTEGRATIONS: usize = 4096;
const MAX_CONNECTIONS: usize = 64;

fn listed(providers: &Value) -> impl Iterator<Item = (&str, &Value)> {
    providers["data"].as_array().into_iter().flatten().take(MAX_INTEGRATIONS).filter_map(|p| {
        let id = p["id"].as_str().filter(|id| safe_id(id))?;
        Some((id, p))
    })
}

/// Provider ID → display name (`name`, at most 160 characters, else the ID).
pub(super) fn labels(providers: &Value) -> HashMap<String, String> {
    listed(providers).map(|(id, p)| (id.to_owned(), label_of(&p["name"], id).to_owned())).collect()
}

/// Providers declared in configuration or detected on this computer: OpenCode
/// lists them without an `integrationID`.
pub(super) fn configured(providers: &Value) -> HashSet<String> {
    listed(providers).filter(|(_, p)| p["integrationID"].as_str().is_none()).map(|(id, _)| id.to_owned()).collect()
}

fn connection_label(value: &Value) -> Option<&str> {
    value.as_str().filter(|l| !l.trim().is_empty() && l.chars().count() <= 80 && !l.chars().any(char::is_control))
}

fn kind(provider: &Value, integration: Option<&Value>, known: bool, own: &[&Value]) -> &'static str {
    let connections: Vec<&Value> = integration
        .and_then(|i| i["connections"].as_array())
        .into_iter()
        .flatten()
        .take(MAX_CONNECTIONS)
        .collect();
    // OpenCode lists the newest credential first.
    if let Some(first) = connections.iter().find(|c| c["type"] == "credential") {
        return match first["method"].as_str() {
            Some("oauth") => "oauth",
            Some("external") => "external",
            Some("command") => "command",
            _ => "key",
        };
    }
    if connections.iter().any(|c| c["type"] == "env") {
        return "env";
    }
    if provider["integrationID"].as_str().is_none() {
        return if known && integration.is_none() { "local" } else { "config" };
    }
    if !own.is_empty() && own.iter().all(|m| m["free"] == true) { "free" } else { "config" }
}

/// Allowlisted provider rows in service order, at most [`MAX_PROVIDERS`].
/// `integrations` is `/api/integration`; without it kinds are approximate.
pub(super) fn project(providers: &Value, integrations: Option<&Value>, models: &[Value]) -> (Vec<Value>, bool) {
    let by_id: HashMap<&str, &Value> = integrations
        .and_then(|i| i["data"].as_array())
        .into_iter()
        .flatten()
        .take(MAX_INTEGRATIONS)
        .filter_map(|i| Some((i["id"].as_str()?, i)))
        .collect();
    let mut rows = Vec::new();
    let mut seen = HashSet::new();
    for (id, provider) in listed(providers) {
        if !seen.insert(id) {
            continue;
        }
        if rows.len() == MAX_PROVIDERS {
            return (rows, true);
        }
        let integration = by_id.get(provider["integrationID"].as_str().unwrap_or(id)).copied();
        let own: Vec<&Value> = models.iter().filter(|m| m["provider"] == id).collect();
        let credentials: Vec<&Value> = integration
            .and_then(|i| i["connections"].as_array())
            .into_iter()
            .flatten()
            .take(MAX_CONNECTIONS)
            .filter(|c| c["type"] == "credential")
            .collect();
        let kind = kind(provider, integration, integrations.is_some(), &own);
        debug_assert!(KINDS.contains(&kind));
        let mut row = json!({"id": id, "label": label_of(&provider["name"], id), "kind": kind,
            "credential_count": credentials.len(), "model_count": own.len()});
        if let [only] = credentials.as_slice() {
            if let Some(label) = connection_label(&only["label"]) {
                row["active_label"] = json!(label);
            }
        }
        rows.push(row);
    }
    (rows, false)
}
```

Note: `own` is computed from the full projection's `provider` field; `kind` reads `m["free"]`.

`src/state/opencode/catalog.rs` changes (replace `project`, `rows_from`, `offered_rows`, `fetch`, `include_selected`):

```rust
use super::providers;
use std::collections::{HashMap, HashSet};

pub(super) const MAX_FULL: usize = 10_000;
const MAX_VALUE: usize = 256;
/// Until the short list (Task 4a) the row lists the first models, as before.
const MAX_MODELS: usize = 256;

pub(super) fn safe_id(value: &str) -> bool { /* unchanged body */ }
pub(super) fn label_of<'a>(value: &'a Value, fallback: &'a str) -> &'a str { /* unchanged body */ }

/// A model ID may contain `/`: OpenCode lists Groq's `openai/gpt-oss-20b`.
fn model_id(value: &str) -> bool {
    !value.is_empty() && value.len() <= 200 && !value.chars().any(|c| c.is_whitespace() || c.is_control())
}

/// `provider/model`, split by every reader at the first `/`.
pub(super) fn model_value(provider: &str, id: &str) -> Option<String> {
    (safe_id(provider) && model_id(id))
        .then(|| format!("{provider}/{id}"))
        .filter(|value| value.len() <= MAX_VALUE)
}

fn entry(model: &Value, labels: &HashMap<String, String>) -> Option<Value> {
    let (provider, id) = (model["providerID"].as_str()?, model["id"].as_str()?);
    if model["enabled"] == false {
        return None;
    }
    let value = model_value(provider, id)?;
    let efforts: Vec<&str> = model["variants"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|v| v["id"].as_str())
        .filter(|v| safe_id(v) && v.len() <= 40)
        .take(16)
        .collect();
    Some(json!({"id": value, "value": value, "label": label_of(&model["name"], id),
        "provider": provider, "provider_label": labels.get(provider).map(String::as_str).unwrap_or(provider),
        "free": free(&model["cost"]), "effort_options": efforts, "effort_labels": efforts}))
}

pub(super) fn project_all(models: &Value, labels: &HashMap<String, String>) -> (Vec<Value>, bool) {
    let mut rows = Vec::new();
    let mut seen = HashSet::new();
    for model in models["data"].as_array().into_iter().flatten() {
        let Some(row) = entry(model, labels) else { continue };
        if !seen.insert(row["value"].as_str().unwrap_or_default().to_owned()) {
            continue;
        }
        if rows.len() == MAX_FULL {
            return (rows, true);
        }
        rows.push(row);
    }
    (rows, false)
}

pub(super) struct Snapshot {
    pub models: Vec<Value>,
    pub models_truncated: bool,
    pub default: String,
    pub configured: HashSet<String>,
    pub providers: Vec<Value>,
    pub providers_truncated: bool,
    pub loaded: bool,
}

/// The default model is always offered: past the bound its allowlisted row,
/// built from the default response, replaces the last row.
fn include_selected(rows: &mut Vec<Value>, default: &Value, selected: &str, labels: &HashMap<String, String>) {
    if selected.is_empty() || rows.iter().any(|row| row["value"] == selected) {
        return;
    }
    let Some(row) = entry(&default["data"], labels) else { return };
    if rows.len() >= MAX_FULL {
        rows.pop();
    }
    rows.push(row);
}

pub(super) fn snapshot_from(providers: &Value, models: &Value, default: &Value, integrations: Option<&Value>) -> Snapshot {
    let labels = providers::labels(providers);
    let (mut rows, models_truncated) = project_all(models, &labels);
    let selected = match (default["data"]["providerID"].as_str(), default["data"]["id"].as_str()) {
        (Some(p), Some(m)) => model_value(p, m).unwrap_or_default(),
        _ => String::new(),
    };
    include_selected(&mut rows, default, &selected, &labels);
    let (provider_rows, providers_truncated) = providers::project(providers, integrations, &rows);
    Snapshot {
        models: rows,
        models_truncated,
        default: selected,
        configured: providers::configured(providers),
        providers: provider_rows,
        providers_truncated,
        loaded: providers["data"].as_array().is_some_and(|p| !p.is_empty()),
    }
}

pub(super) fn read_snapshot(service: &Service, location: &Path, with_providers: bool) -> Result<Snapshot> {
    let providers = service.call("GET", &query("/api/provider", location), None)?;
    let models = service.call("GET", &query("/api/model", location), None)?;
    let default = service.call("GET", &query("/api/model/default", location), None)?;
    // Integrations only name connection kinds; a failure or a slow answer leaves
    // kinds approximate. One second keeps `account ls` within the connector's 3 s.
    let integrations = with_providers
        .then(|| service.call_within("GET", &query("/api/integration", location), None, Duration::from_secs(1)).ok())
        .flatten();
    let mut snapshot = snapshot_from(&providers, &models, &default, integrations.as_ref());
    if !with_providers {
        snapshot.providers.clear();
        snapshot.providers_truncated = false;
    }
    Ok(snapshot)
}
```

`src/state/opencode/service.rs` — move the status check of `Service::call` into `fn answer_of(response: Response) -> Result<Value>` (same messages) and add:

```rust
    /// `call` with its own deadline, for optional reads that must not hold up a listing.
    pub(super) fn call_within(&self, method: &str, path: &str, body: Option<&Value>, limit: Duration) -> Result<Value> {
        answer_of(request(&self.reg, method, path, body, Instant::now() + limit)?)
    }
```

`call` becomes `answer_of(self.raw(method, path, body)?)`.

`model_ref`: change the split check to `.filter(|(p, m)| safe_id(p) && model_id(m))` and compare `row["value"] == value` (rows always carry `value`). `offered_model`: `let snapshot = read_snapshot(service, location, false)?; model_ref(&snapshot.models, value, effort)`.

`catalog()` in this task keeps its output: replace the `fetch(&service, location)` match with `read_snapshot(&service, location, false)`, and build `model_options` from `snapshot.models` exactly as before — the first `MAX_MODELS` rows, with the default row kept (if the default lies beyond the first 256, put it in place of the 256th). Task 4a replaces this.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo +1.99.1 test --locked opencode:: && cargo +1.99.1 clippy --locked --all-targets -- -D warnings && RUSTUP_TOOLCHAIN=1.99.1 bash scripts/ci/check-windows-target.sh clippy`
Expected: PASS; no warnings on Linux or the Windows target.

- [ ] **Step 5: Commit**

```bash
git add src/state/opencode/providers.rs src/state/opencode/catalog.rs src/state/opencode/service.rs src/state/opencode.rs
git commit -m "Project OpenCode providers and keep model IDs that contain a slash"
```

---

### Task 3: hgs — favorites on the agent computer with a request identity

**Files:**
- Create: `src/state/opencode/favorites.rs`
- Modify: `src/state/opencode.rs` (`#[cfg_attr(not(unix), allow(dead_code))] mod favorites;` and, because only `#[cfg(unix)]` accounts code calls it, `#[cfg(unix)] pub(crate) use favorites::set_favorite;`), `src/accounts.rs` (operation `favorite`; usage text), `src/cli.rs` (`hgs --help` line), `docs/reference.md` (CLI line under the OpenCode section)
- Test: unit tests in `favorites.rs`; `usage_lists_opencode_effort_and_the_explicit_service_start` in `src/cli.rs`; `tests/test_accounts.py`

**Interfaces:**
- Consumes: `catalog::model_value` (Task 2); `storage::atomic`, `root()`.
- Produces:
  - `pub(crate) const MAX_FAVORITES: usize = 64;`
  - `pub(crate) fn favorites_path(config_dir: &Path) -> PathBuf` → `<config_dir>/opencode-favorites.json`
  - `fn read(path: &Path) -> Result<Option<Vec<String>>>` (the stored list; `Ok(None)` without a file; Task 4a adds `load_favorites` on top of it, so this commit leaves nothing unused)
  - `pub(crate) fn set_favorite(config_dir: &Path, model: &str, favorite: bool, request_id: &str) -> Result<Value>`; the caller holds `accounts.lock`
  - `pub(super) fn set_favorite_at(path: &Path, receipts: &Path, model: &str, favorite: bool, request_id: &str) -> Result<Value>` (testable core)
  - CLI: `hgs [@host] account favorite native-opencode --model PROVIDER/MODEL --favorite on|off --request-id UUID [--json] [--dry-run]`

File format: `{"version":1,"models":["provider/model", …]}`, at most 64 distinct valid values, at most 64 KiB. Receipts: `<state root>/opencode/favorite-requests/<uuid>.json` with `{"status":"pending"|"applied","request_id","model","favorite"}`; the newest 256 are kept, and pruning never removes the receipt it has just written.

- [ ] **Step 1: Write the failing tests**

`src/state/opencode/favorites.rs` test module:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    const R1: &str = "6f1c1a0e-4d8b-4b7e-9a51-3c2d1e0f9a01";
    const R2: &str = "6f1c1a0e-4d8b-4b7e-9a51-3c2d1e0f9a02";
    const R3: &str = "6f1c1a0e-4d8b-4b7e-9a51-3c2d1e0f9a03";

    fn paths() -> (tempfile::TempDir, PathBuf, PathBuf) {
        let dir = tempfile::tempdir().unwrap();
        let (file, receipts) = (dir.path().join("opencode-favorites.json"), dir.path().join("receipts"));
        (dir, file, receipts)
    }

    fn stored(path: &Path) -> Vec<String> {
        read(path).unwrap().unwrap_or_default()
    }

    #[test]
    fn favorites_keep_their_order_and_never_repeat() {
        let (_dir, file, receipts) = paths();
        set_favorite_at(&file, &receipts, "groq/openai/gpt-oss-20b", true, R1).unwrap();
        let receipt = set_favorite_at(&file, &receipts, "fixture/local", true, R2).unwrap();
        assert_eq!(receipt["favorites"], json!(["groq/openai/gpt-oss-20b", "fixture/local"]));
        assert_eq!(receipt["status"], "applied");
        let again = set_favorite_at(&file, &receipts, "fixture/local", true, R3).unwrap();
        assert_eq!(again["favorites"], json!(["groq/openai/gpt-oss-20b", "fixture/local"]));
        assert_eq!(stored(&file), ["groq/openai/gpt-oss-20b", "fixture/local"]);
    }

    #[test]
    fn a_retried_request_returns_its_receipt_without_reapplying() {
        let (_dir, file, receipts) = paths();
        set_favorite_at(&file, &receipts, "fixture/local", true, R1).unwrap();
        set_favorite_at(&file, &receipts, "fixture/local", false, R2).unwrap();
        let replay = set_favorite_at(&file, &receipts, "fixture/local", true, R1).unwrap();
        assert_eq!((replay["status"].as_str(), replay["favorite"].as_bool()), (Some("applied"), Some(true)));
        assert_eq!(replay["favorites"], json!([]));
        assert!(stored(&file).is_empty());
    }

    #[test]
    fn an_identity_is_bound_to_one_change() {
        let (_dir, file, receipts) = paths();
        set_favorite_at(&file, &receipts, "fixture/local", true, R1).unwrap();
        let error = set_favorite_at(&file, &receipts, "fixture/other", true, R1).unwrap_err();
        assert_eq!(error, "This request identity belongs to another favorite change");
        assert!(set_favorite_at(&file, &receipts, "fixture/local", false, R1).is_err());
    }

    #[test]
    fn an_interrupted_change_is_never_replayed_over_a_newer_state() {
        let (_dir, file, receipts) = paths();
        std::fs::create_dir_all(&receipts).unwrap();
        let pending = json!({"status": "pending", "request_id": R3, "model": "fixture/local", "favorite": true});
        std::fs::write(receipts.join(format!("{R3}.json")), pending.to_string()).unwrap();
        let error = set_favorite_at(&file, &receipts, "fixture/local", true, R3).unwrap_err();
        assert_eq!(error, "This favorite change was interrupted; refresh and choose again");
        assert!(stored(&file).is_empty());
        // When the file already shows the change, the receipt completes.
        set_favorite_at(&file, &receipts, "fixture/local", true, R1).unwrap();
        assert_eq!(set_favorite_at(&file, &receipts, "fixture/local", true, R3).unwrap()["status"], "applied");
    }

    #[test]
    fn at_most_sixty_four_favorites() {
        let (_dir, file, receipts) = paths();
        for i in 0..MAX_FAVORITES {
            set_favorite_at(&file, &receipts, &format!("p/m{i}"), true, &uuid::Uuid::new_v4().to_string()).unwrap();
        }
        let error = set_favorite_at(&file, &receipts, "p/one-more", true, &uuid::Uuid::new_v4().to_string()).unwrap_err();
        assert_eq!(error, "OpenCode keeps at most 64 favorite models; remove one first");
        // Removing still works at the bound.
        assert!(set_favorite_at(&file, &receipts, "p/m0", false, &uuid::Uuid::new_v4().to_string()).is_ok());
    }

    #[test]
    fn invalid_models_and_identities_are_refused() {
        let (_dir, file, receipts) = paths();
        for model in ["fixture", "a b/c", "p/ x", "/m", "p/"] {
            assert!(set_favorite_at(&file, &receipts, model, true, R1).is_err(), "{model}");
        }
        assert_eq!(set_favorite_at(&file, &receipts, "p/m", true, "not-a-uuid").unwrap_err(), "Invalid favorite request identity");
        assert!(!file.exists() && !receipts.exists());
    }

    #[test]
    fn a_damaged_file_is_refused_and_never_overwritten() {
        let (_dir, file, receipts) = paths();
        std::fs::write(&file, b"{not json").unwrap();
        assert_eq!(read(&file).unwrap_err(), INVALID);
        assert_eq!(set_favorite_at(&file, &receipts, "p/m", true, R1).unwrap_err(), INVALID);
        assert_eq!(std::fs::read(&file).unwrap(), b"{not json");
        std::fs::write(&file, vec![b' '; 70_000]).unwrap();
        assert_eq!(read(&file).unwrap_err(), INVALID);
    }

    #[test]
    fn receipts_are_pruned_to_the_newest_256_never_the_one_just_written() {
        let (_dir, file, receipts) = paths();
        let mut last = String::new();
        for i in 0..260 {
            last = uuid::Uuid::new_v4().to_string();
            set_favorite_at(&file, &receipts, "p/m", i % 2 == 0, &last).unwrap();
            assert!(receipts.join(format!("{last}.json")).is_file(), "receipt {i} was pruned at once");
        }
        assert_eq!(std::fs::read_dir(&receipts).unwrap().count(), 256);
        assert!(receipts.join(format!("{last}.json")).is_file());
    }
}
```

`tests/test_accounts.py` (inside `class Accounts`):

```python
    def test_opencode_favorites_are_stored_beside_the_catalog_with_request_identity(self):
        # Private OpenCode state: an inherited XDG_STATE_HOME must never reach a real service.
        self.env.update(XDG_STATE_HOME=str(self.root/'xdg-state'),XDG_CONFIG_HOME=str(self.root/'xdg-config'))
        binary=self.bin/"opencode";binary.write_text("#!/bin/sh\n[ \"$1\" = --version ] && echo v2.0.32 || exit 9\n");binary.chmod(0o700)
        catalog=self.config/'accounts.json'
        request='6f1c1a0e-4d8b-4b7e-9a51-3c2d1e0f9a10'
        def favorite(*args,ok=True):
            return self.run_hgs('account','favorite','native-opencode',*args,ok=ok)
        receipt=json.loads(favorite('--model','groq/openai/gpt-oss-20b','--favorite','on','--request-id',request,'--json').stdout)
        self.assertEqual((receipt['status'],receipt['favorite'],receipt['favorites']),('applied',True,['groq/openai/gpt-oss-20b']))
        stored=json.loads((self.config/'opencode-favorites.json').read_text())
        self.assertEqual(stored,{'version':1,'models':['groq/openai/gpt-oss-20b']})
        self.assertFalse(catalog.exists(),'favorites never rewrite accounts.json')
        self.assertEqual(json.loads(favorite('--model','groq/openai/gpt-oss-20b','--favorite','on','--request-id',request).stdout),receipt)
        for args,message in [(('--model','groq/x','--favorite','maybe','--request-id',request),b'Choose --favorite on or off'),
                             (('--model','groq/x','--favorite','on'),b'A favorite change needs --request-id UUID'),
                             (('--favorite','on','--request-id',request),b'Choose --model PROVIDER/MODEL')]:
            self.assertIn(message,favorite(*args,ok=False).stderr)
        refused=self.run_hgs('account','favorite','native-codex','--model','a/b','--favorite','on','--request-id',request,ok=False)
        self.assertIn(b'Only the native OpenCode account has favorite models',refused.stderr)
        planned=json.loads(favorite('--model','fixture/local','--favorite','on','--request-id','6f1c1a0e-4d8b-4b7e-9a51-3c2d1e0f9a11','--dry-run').stdout)
        self.assertEqual(planned,{'dry_run':True,'model':'fixture/local','favorite':True})
        self.assertEqual(json.loads((self.config/'opencode-favorites.json').read_text())['models'],['groq/openai/gpt-oss-20b'])
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cargo +1.99.1 test --locked opencode::favorites; cargo +1.99.1 build --locked && PYTHONPATH=tests python3 -m unittest test_accounts.Accounts.test_opencode_favorites_are_stored_beside_the_catalog_with_request_identity -v`
Expected: compile errors (`set_favorite_at`, `read`, `INVALID`, `MAX_FAVORITES` not defined); the CLI test fails with the usage error.

- [ ] **Step 3: Implement**

`src/state/opencode/favorites.rs` (above the tests):

```rust
//! OpenCode favorite models, stored on the agent computer beside the hgs
//! account catalog (`opencode-favorites.json`), so every desktop and phone
//! connected to this computer sees one list. A change is an explicit user
//! action with a request identity: a retried identity returns its first
//! receipt and never applies over a newer change.
use super::super::*;
use super::catalog::model_value;
use std::io::Read;

pub(crate) const MAX_FAVORITES: usize = 64;
const MAX_FILE: u64 = 64 * 1024;
const MAX_RECEIPT: u64 = 4096;
const MAX_RECEIPTS: usize = 256;
pub(super) const INVALID: &str = "Invalid opencode-favorites.json; remove it to reset OpenCode favorites";

pub(crate) fn favorites_path(config_dir: &Path) -> PathBuf {
    config_dir.join("opencode-favorites.json")
}

fn valid_model(model: &str) -> bool {
    model.split_once('/').and_then(|(p, m)| model_value(p, m)).as_deref() == Some(model)
}

fn bounded(path: &Path, limit: u64) -> std::io::Result<Option<Vec<u8>>> {
    let file = match std::fs::File::open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(e),
    };
    let mut bytes = Vec::new();
    file.take(limit + 1).read_to_end(&mut bytes)?;
    Ok(Some(bytes))
}

/// The stored list; `Ok(None)` without a file, `Err` for a damaged one.
fn read(path: &Path) -> Result<Option<Vec<String>>> {
    let Some(bytes) = bounded(path, MAX_FILE).map_err(|_| INVALID)? else { return Ok(None) };
    if bytes.len() as u64 > MAX_FILE {
        return Err(INVALID.into());
    }
    let value: Value = serde_json::from_slice(&bytes).map_err(|_| INVALID)?;
    let models = value["models"].as_array().filter(|_| value["version"] == 1).ok_or(INVALID)?;
    let mut list: Vec<String> = Vec::new();
    for model in models {
        let model = model.as_str().filter(|m| valid_model(m)).ok_or(INVALID)?;
        if !list.iter().any(|known| known == model) {
            list.push(model.to_owned());
        }
    }
    if list.len() > MAX_FAVORITES {
        return Err(INVALID.into());
    }
    Ok(Some(list))
}

pub(crate) fn set_favorite(config_dir: &Path, model: &str, favorite: bool, request_id: &str) -> Result<Value> {
    set_favorite_at(&favorites_path(config_dir), &root().join("opencode/favorite-requests"), model, favorite, request_id)
}

fn read_receipt(path: &Path) -> Option<Value> {
    let bytes = bounded(path, MAX_RECEIPT).ok()??;
    (bytes.len() as u64 <= MAX_RECEIPT).then(|| serde_json::from_slice(&bytes).ok()).flatten()
}

/// Keeps the newest receipts; `keep` (the one just written) always stays, even
/// when coarse timestamps make it look as old as others.
fn prune(receipts: &Path, keep: &Path) {
    let Ok(entries) = std::fs::read_dir(receipts) else { return };
    let mut files: Vec<(std::time::SystemTime, PathBuf)> = entries
        .filter_map(|e| e.ok())
        .filter(|e| e.path().extension().is_some_and(|x| x == "json") && e.path() != keep)
        .filter_map(|e| Some((e.metadata().ok()?.modified().ok()?, e.path())))
        .collect();
    if files.len() < MAX_RECEIPTS {
        return;
    }
    files.sort();
    for (_, path) in &files[..files.len() + 1 - MAX_RECEIPTS] {
        let _ = std::fs::remove_file(path);
    }
}

pub(super) fn set_favorite_at(path: &Path, receipts: &Path, model: &str, favorite: bool, request_id: &str) -> Result<Value> {
    let request = uuid::Uuid::parse_str(request_id)
        .map_err(|_| "Invalid favorite request identity")?
        .hyphenated()
        .to_string();
    if !valid_model(model) {
        return Err("Choose provider/model for OpenCode".into());
    }
    let receipt_path = receipts.join(format!("{request}.json"));
    let record = |status: &str| json!({"status": status, "request_id": request, "model": model, "favorite": favorite});
    let answer = |list: &[String]| {
        let mut receipt = record("applied");
        receipt["favorites"] = json!(list);
        receipt
    };
    if let Some(previous) = read_receipt(&receipt_path) {
        if previous["model"] != model || previous["favorite"] != favorite {
            return Err("This request identity belongs to another favorite change".into());
        }
        let current = read(path)?.unwrap_or_default();
        if previous["status"] == "applied" {
            return Ok(answer(&current));
        }
        if current.iter().any(|m| m == model) == favorite {
            atomic(&receipt_path, &record("applied").to_string())?;
            return Ok(answer(&current));
        }
        return Err("This favorite change was interrupted; refresh and choose again".into());
    }
    let mut list = read(path)?.unwrap_or_default();
    let present = list.iter().any(|m| m == model);
    if favorite && !present {
        if list.len() >= MAX_FAVORITES {
            return Err("OpenCode keeps at most 64 favorite models; remove one first".into());
        }
        list.push(model.to_owned());
    } else if !favorite {
        list.retain(|m| m != model);
    }
    atomic(&receipt_path, &record("pending").to_string())?;
    atomic(path, &json!({"version": 1, "models": list}).to_string())?;
    atomic(&receipt_path, &record("applied").to_string())?;
    prune(receipts, &receipt_path);
    Ok(answer(&list))
}
```

`src/accounts.rs` — add arms to the `match operation` in `dispatch` (before `"login"`), keeping the file's style:

```rust
        "favorite"=>{
            if id!="native-opencode" {return Err(Error::new(1,"Only the native OpenCode account has favorite models"));}
            let model=arg(args,"--model").ok_or_else(||Error::new(1,"Choose --model PROVIDER/MODEL"))?;
            let favorite=match arg(args,"--favorite") {Some("on")=>true,Some("off")=>false,_=>return Err(Error::new(1,"Choose --favorite on or off"))};
            let request=arg(args,"--request-id").ok_or_else(||Error::new(1,"A favorite change needs --request-id UUID"))?;
            if dry {println!("{}",json!({"dry_run":true,"model":model,"favorite":favorite}));return Ok(0);}
            #[cfg(unix)]
            {let _lock=locked(config)?;println!("{}",crate::state::opencode::set_favorite(&config.dir,model,favorite,request).map_err(|e|Error::new(1,e))?);}
            #[cfg(not(unix))]
            return Err(Error::new(2,platform::LOCAL_SESSIONS_UNSUPPORTED));
        },
```

Extend the final usage string with `| favorite native-opencode --model PROVIDER/MODEL --favorite on|off --request-id UUID`.

`src/cli.rs` — add after the `account start native-opencode` line of the usage text:

```text
       hgs [@host] account favorite native-opencode --model PROVIDER/MODEL --favorite on|off --request-id UUID   star an OpenCode model on that computer
```

and extend `usage_lists_opencode_effort_and_the_explicit_service_start`:

```rust
        // Clients look for these commands before offering stars and All models.
        assert!(usage.contains(
            "\n       hgs [@host] account favorite native-opencode --model PROVIDER/MODEL --favorite on|off --request-id UUID   star an OpenCode model on that computer\n"
        ), "{usage}");
```

In `docs/reference.md` (OpenCode section, after the `account start` sentence) add: "`hgs [@host] account favorite native-opencode --model PROVIDER/MODEL --favorite on|off --request-id UUID` stars a model on that computer (at most 64; stored in `opencode-favorites.json` beside the account catalog). A retried request identity returns its first result."

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo +1.99.1 test --locked opencode::favorites && cargo +1.99.1 test --locked usage_lists_opencode && cargo +1.99.1 build --locked && PYTHONPATH=tests python3 -m unittest test_accounts -v && cargo +1.99.1 clippy --locked --all-targets -- -D warnings && RUSTUP_TOOLCHAIN=1.99.1 bash scripts/ci/check-windows-target.sh clippy`
Expected: PASS (8 favorites tests and the usage test; the Python suite including the new test); no warnings on Linux or the Windows target.

- [ ] **Step 5: Commit**

```bash
git add src/state/opencode/favorites.rs src/state/opencode.rs src/accounts.rs src/cli.rs tests/test_accounts.py docs/reference.md
git commit -m "Store OpenCode favorite models on the agent computer with request identities"
```

---

### Task 4a: hgs — short list, the 32 KiB row budget and the account row

**Files:**
- Create: `src/state/opencode/shortlist.rs`
- Modify: `src/state/opencode/favorites.rs` (add `load_favorites`), `src/state/opencode/catalog.rs` (`row_from`, `empty_row`, `answer`, `favorites()`, `catalog()` returns the row keys), `src/state/opencode.rs` (`#[cfg_attr(not(unix), allow(dead_code))] mod shortlist;` and `#[cfg(unix)] pub(crate) use shortlist::{fit_row, ROW_BUDGET};`), `src/accounts.rs` (row keys and `fit_row`), `tests/test_accounts.py`, `docs/opencode.md` (Models section)
- Test: unit tests in `shortlist.rs` and `favorites.rs`

**Interfaces:**
- Consumes: `Snapshot`, `snapshot_from`, `read_snapshot` (Task 2); the favorites file format and `read` (Task 3).
- Produces:
  - `pub(crate) const ROW_BUDGET: usize = 32 * 1024;`, `pub(super) const MAX_SHORT: usize = 256;`, `pub(super) const KEEP_PROVIDERS: usize = 16;`
  - `pub(crate) fn ascii_len(value: &Value) -> usize`
  - `pub(super) fn ordered(models: &[Value], default: &str, pinned: &[&str], favorites: &[String], configured: &HashSet<String>, limit: usize) -> (Vec<Value>, bool)` — default, pinned models, favorites (stored order), configured models, free models (service order); each value once; at most `limit`; `true` when cut; marks `favorite`/`default` only when true
  - `pub(super) fn build(models: &[Value], default: &str, pinned: &[&str], favorites: &[String], configured: &HashSet<String>) -> (Vec<Value>, bool)` = `ordered(…, MAX_SHORT)`
  - `pub(crate) fn fit_row(row: &mut Value, budget: usize)`
  - `pub(crate) fn load_favorites(config_dir: &Path) -> Vec<String>` (favorites.rs; never fails, a missing or damaged file lists none)
  - `pub(super) fn row_from(snapshot: &Snapshot, favorites: &[String], pinned: &[&str]) -> Value` → `{"model_options","model_options_truncated","providers","providers_truncated","settings_model"}`
  - `fn answer(row: &Value, source: &str, state: &str, reason: &str, at: Value) -> Value` and `fn empty_row() -> Value` (catalog.rs, private)
  - `catalog(location)` returns the row keys plus `model_catalog_source`, `catalog_state`, `catalog_reason`, `catalog_at`. Its stopped-service answer and cache file keep the version 1 shape in this task (`model_options` only); Task 4b replaces both.

- [ ] **Step 1: Write the failing tests**

`src/state/opencode/shortlist.rs` test module:

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use super::super::catalog::{row_from, snapshot_from};

    fn shared() -> Value {
        serde_json::from_slice(include_bytes!(
            "../../../tests/fixtures/opencode-provider-catalog.json"
        ))
        .unwrap()
    }

    fn model(value: &str, free: bool, label: &str) -> Value {
        let provider = value.split('/').next().unwrap();
        json!({"id": value, "value": value, "label": label, "provider": provider, "provider_label": provider,
               "free": free, "effort_options": ["low"], "effort_labels": ["low"]})
    }

    #[test]
    fn ascii_len_matches_the_connector_measure() {
        for case in shared()["ascii_lengths"].as_array().unwrap() {
            assert_eq!(ascii_len(&case["value"]), case["length"].as_u64().unwrap() as usize, "{}", case["value"]);
        }
    }

    #[test]
    fn the_shared_row_orders_default_favorites_configured_free() {
        let fixture = shared();
        let service = &fixture["service"];
        let snapshot = snapshot_from(&service["provider"], &service["model"], &service["default"], Some(&service["integration"]));
        let favorites: Vec<String> = serde_json::from_value(fixture["favorites"].clone()).unwrap();
        let row = row_from(&snapshot, &favorites, &[]);
        for key in ["model_options", "providers", "settings_model", "model_options_truncated", "providers_truncated"] {
            assert_eq!(row[key], fixture["row"][key], "{key}");
        }
    }

    #[test]
    fn a_pinned_model_follows_the_default_once_with_its_efforts() {
        let fixture = shared();
        let service = &fixture["service"];
        let snapshot = snapshot_from(&service["provider"], &service["model"], &service["default"], Some(&service["integration"]));
        // A paid, unstarred, key-connected model that is a session's current model.
        let row = row_from(&snapshot, &[], &["groq/openai/gpt-oss-20b", "fixture/local", "gone/missing"]);
        let options = row["model_options"].as_array().unwrap();
        let values: Vec<&str> = options.iter().map(|o| o["value"].as_str().unwrap()).collect();
        assert_eq!(&values[..2], ["fixture/local", "groq/openai/gpt-oss-20b"]);
        assert_eq!(options[1]["effort_options"], json!(["low", "medium", "high"]));
        assert!(options[1].get("favorite").is_none());
        assert_eq!(values.iter().filter(|v| **v == "fixture/local").count(), 1);
        assert!(!values.contains(&"gone/missing"));
    }

    #[test]
    fn the_short_list_is_bounded_and_cuts_free_models_first() {
        let mut models = vec![model("cfg/m000", false, "Default")];
        models.extend((0..64).map(|i| model(&format!("paid/f{i:03}"), false, "Favorite")));
        models.extend((1..150).map(|i| model(&format!("cfg/m{i:03}"), false, "Configured")));
        models.extend((0..150).map(|i| model(&format!("zen/z{i:03}"), true, "Free")));
        let favorites: Vec<String> = (0..64).map(|i| format!("paid/f{i:03}")).collect();
        let configured: HashSet<String> = ["cfg".to_owned()].into();
        let (rows, truncated) = build(&models, "cfg/m000", &[], &favorites, &configured);
        assert!(truncated);
        assert_eq!(rows.len(), MAX_SHORT);
        assert_eq!((rows[0]["value"].as_str(), rows[0]["default"].as_bool()), (Some("cfg/m000"), Some(true)));
        assert!(rows[1..65].iter().all(|r| r["favorite"] == true));
        assert!(rows[65..214].iter().all(|r| r["provider"] == "cfg" && r.get("favorite").is_none()));
        assert_eq!(rows[214..].len(), 42);
        assert!(rows[214..].iter().all(|r| r["free"] == true));
        // A favorite that is not listed is skipped, nothing repeats, and a paid model
        // that is neither default, pinned, favorite nor configured stays out.
        let (rows, cut) = build(&models[..3], "cfg/m000", &[], &["gone/missing".to_owned(), "cfg/m000".to_owned()], &configured);
        assert_eq!(rows.iter().map(|r| r["value"].as_str().unwrap()).collect::<Vec<_>>(), ["cfg/m000"]);
        assert_eq!((&rows[0]["favorite"], &rows[0]["default"], cut), (&json!(true), &json!(true), false));
        // The cache uses the same priority with its own limit (Task 4b).
        let (rows, cut) = ordered(&models, "cfg/m000", &[], &favorites, &configured, 512);
        assert_eq!((rows.len(), cut), (364, false));
    }

    fn big_row() -> Value {
        let mut options = vec![{ let mut d = model("cfg/default", false, &"D".repeat(150)); d["default"] = json!(true); d }];
        options.extend((0..30).map(|i| { let mut m = model(&format!("paid/f{i:02}"), false, &"F".repeat(150)); m["favorite"] = json!(true); m }));
        options.extend((0..30).map(|i| model(&format!("cfg/c{i:02}"), false, &"C".repeat(150))));
        options.extend((0..30).map(|i| model(&format!("zen/r{i:02}"), true, &"R".repeat(150))));
        let providers: Vec<Value> = (0..40).map(|i| json!({"id": format!("p{i:02}"), "label": "P".repeat(60), "kind": "key",
            "credential_count": 1, "model_count": 3, "active_label": "A".repeat(40)})).collect();
        json!({"id": "native-opencode", "provider": "opencode", "label": "Native OpenCode configuration",
               "model_options": options, "model_options_truncated": false, "providers": providers, "providers_truncated": false})
    }

    #[test]
    fn the_row_budget_drops_free_then_configured_then_favorites() {
        let mut row = big_row();
        assert!(ascii_len(&row) > ROW_BUDGET);
        fit_row(&mut row, ROW_BUDGET);
        assert!(ascii_len(&row) <= ROW_BUDGET, "{}", ascii_len(&row));
        let options = row["model_options"].as_array().unwrap();
        assert_eq!(options[0]["value"], "cfg/default");
        assert_eq!(options.iter().filter(|o| o["favorite"] == true).count(), 30);
        assert_eq!(options.iter().filter(|o| o["provider"] == "cfg").count(), 31);
        let free = options.iter().filter(|o| o["free"] == true).count();
        assert!(free > 0 && free < 30, "{free}");
        assert_eq!(row["model_options_truncated"], true);
        assert_eq!((row["providers"].as_array().unwrap().len(), &row["providers_truncated"]), (40, &json!(false)));
    }

    #[test]
    fn providers_beyond_sixteen_go_only_after_every_model_but_the_default() {
        let mut row = big_row();
        let floor = {
            let mut r = row.clone();
            r["model_options"] = json!([row["model_options"][0].clone()]);
            r["providers"].as_array_mut().unwrap().truncate(KEEP_PROVIDERS);
            ascii_len(&r)
        };
        fit_row(&mut row, floor);
        assert_eq!(row["model_options"].as_array().unwrap().len(), 1);
        assert_eq!(row["model_options"][0]["default"], true);
        assert_eq!(row["providers"].as_array().unwrap().len(), KEEP_PROVIDERS);
        assert_eq!((&row["model_options_truncated"], &row["providers_truncated"]), (&json!(true), &json!(true)));
        // Below that, providers keep going; the default model stays.
        let mut row = big_row();
        fit_row(&mut row, floor - 1500);
        assert!(row["providers"].as_array().unwrap().len() < KEEP_PROVIDERS);
        assert_eq!(row["model_options"][0]["value"], "cfg/default");
        assert!(ascii_len(&row) <= floor - 1500);
    }

    #[test]
    fn a_row_within_the_budget_is_unchanged() {
        let fixture = shared();
        let mut row = fixture["hgs_account"].clone();
        fit_row(&mut row, ROW_BUDGET);
        assert_eq!(row, fixture["hgs_account"]);
    }
}
```

The `ordered(…, 512)` count is 364: 1 default, 64 favorites, 149 further configured models and 150 free models.

Add to the `favorites.rs` tests:

```rust
    #[test]
    fn listing_never_fails_on_a_missing_or_damaged_file() {
        let dir = tempfile::tempdir().unwrap();
        assert!(load_favorites(dir.path()).is_empty());
        std::fs::write(favorites_path(dir.path()), b"{not json").unwrap();
        assert!(load_favorites(dir.path()).is_empty());
        std::fs::write(favorites_path(dir.path()), json!({"version": 1, "models": ["groq/openai/gpt-oss-20b"]}).to_string()).unwrap();
        assert_eq!(load_favorites(dir.path()), ["groq/openai/gpt-oss-20b"]);
    }
```

Replace the `catalog_reasons_are_bounded_and_sanitized` calls of the old `answer(json!([]), "", source, state, reason, at)` with `answer(&empty_row(), source, state, reason, at)` and keep every assertion.

`tests/test_accounts.py` — extend `test_native_opencode_row_without_a_service_and_start_scope` after the first `row=…` lookup:

```python
            self.assertEqual((row['providers'],row['model_options_truncated'],row['providers_truncated']),([],False,False))
            self.assertLessEqual(len(json.dumps(row,ensure_ascii=True)),32*1024)
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cargo +1.99.1 test --locked opencode::`
Expected: compile errors (`shortlist` items, `row_from`, `empty_row`, `load_favorites` not defined).

- [ ] **Step 3: Implement**

`src/state/opencode/shortlist.rs` (above the tests):

```rust
//! The OpenCode short list New session and model change show first, and the
//! 32 KiB budget of the account row, measured as the mobile connector
//! measures its projection (`json.dumps(row, ensure_ascii=True)`).
use super::super::*;
use std::collections::{HashMap, HashSet};

pub(crate) const ROW_BUDGET: usize = 32 * 1024;
pub(super) const MAX_SHORT: usize = 256;
pub(super) const KEEP_PROVIDERS: usize = 16;

fn str_len(text: &str) -> usize {
    2 + text
        .chars()
        .map(|c| match c {
            '"' | '\\' | '\n' | '\r' | '\t' | '\u{8}' | '\u{c}' => 2,
            ' '..='~' => 1,
            c => 6 * c.len_utf16(),
        })
        .sum::<usize>()
}

/// `len(json.dumps(value, ensure_ascii=True))`, as the connector's
/// `zerus_relay::json::python_default_len(value, true)`; the shared fixture's
/// `ascii_lengths` pin both.
pub(crate) fn ascii_len(value: &Value) -> usize {
    match value {
        Value::Null | Value::Bool(true) => 4,
        Value::Bool(false) => 5,
        Value::Number(n) => n.to_string().len(),
        Value::String(s) => str_len(s),
        Value::Array(items) => 2 + items.iter().map(ascii_len).sum::<usize>() + 2 * items.len().saturating_sub(1),
        Value::Object(map) => {
            2 + map.iter().map(|(k, v)| str_len(k) + 2 + ascii_len(v)).sum::<usize>() + 2 * map.len().saturating_sub(1)
        }
    }
}

/// Default model, pinned models (a session's current and pending model),
/// favorites (stored order), configured models, free models (service order);
/// each once, at most `limit`; `true` when cut.
pub(super) fn ordered(models: &[Value], default: &str, pinned: &[&str], favorites: &[String],
                      configured: &HashSet<String>, limit: usize) -> (Vec<Value>, bool) {
    let by_value: HashMap<&str, &Value> = models.iter().filter_map(|m| Some((m["value"].as_str()?, m))).collect();
    let candidates = by_value
        .get(default)
        .copied()
        .into_iter()
        .chain(pinned.iter().filter_map(|p| by_value.get(p).copied()))
        .chain(favorites.iter().filter_map(|f| by_value.get(f.as_str()).copied()))
        .chain(models.iter().filter(|m| m["provider"].as_str().is_some_and(|p| configured.contains(p))))
        .chain(models.iter().filter(|m| m["free"] == true));
    let mut seen = HashSet::new();
    let mut rows = Vec::new();
    for model in candidates {
        let Some(value) = model["value"].as_str() else { continue };
        if !seen.insert(value) {
            continue;
        }
        if rows.len() == limit {
            return (rows, true);
        }
        let mut row = model.clone();
        if favorites.iter().any(|f| f == value) {
            row["favorite"] = json!(true);
        }
        if value == default {
            row["default"] = json!(true);
        }
        rows.push(row);
    }
    (rows, false)
}

pub(super) fn build(models: &[Value], default: &str, pinned: &[&str], favorites: &[String],
                    configured: &HashSet<String>) -> (Vec<Value>, bool) {
    ordered(models, default, pinned, favorites, configured, MAX_SHORT)
}

/// Keeps `row` within `budget`: short-list entries go from the end (free, then
/// configured, then favorites; a default model first in the list stays), then
/// providers beyond the first [`KEEP_PROVIDERS`], then any other provider.
/// The row must already carry both truncation flags as booleans.
pub(crate) fn fit_row(row: &mut Value, budget: usize) {
    let mut size = ascii_len(row);
    let keep = usize::from(row["model_options"][0]["default"] == true);
    let (mut models, mut providers) = (false, false);
    for (key, floor) in [("model_options", keep), ("providers", KEEP_PROVIDERS), ("providers", 0)] {
        let Some(items) = row.get_mut(key).and_then(Value::as_array_mut) else { continue };
        while size > budget && items.len() > floor {
            let last = items.pop().unwrap_or(Value::Null);
            size -= ascii_len(&last) + if items.is_empty() { 0 } else { 2 };
            if key == "model_options" { models = true } else { providers = true }
        }
    }
    if models {
        row["model_options_truncated"] = json!(true);
    }
    if providers {
        row["providers_truncated"] = json!(true);
    }
}
```

`src/state/opencode/favorites.rs` — add:

```rust
/// The stored favorites for the catalog; a missing or damaged file lists none.
pub(crate) fn load_favorites(config_dir: &Path) -> Vec<String> {
    read(&favorites_path(config_dir)).ok().flatten().unwrap_or_default()
}
```

`src/state/opencode/catalog.rs` — replace `answer`, `unavailable` and the Task 2 interim body of `catalog()`, and remove `MAX_MODELS`:

```rust
use super::shortlist;

fn favorites() -> Vec<String> {
    crate::platform::home()
        .map(|home| super::favorites::load_favorites(&crate::config::config_dir(&home)))
        .unwrap_or_default()
}

fn empty_row() -> Value {
    json!({"model_options": [], "model_options_truncated": false, "providers": [],
           "providers_truncated": false, "settings_model": ""})
}

pub(super) fn row_from(snapshot: &Snapshot, favorites: &[String], pinned: &[&str]) -> Value {
    let (options, cut) = shortlist::build(&snapshot.models, &snapshot.default, pinned, favorites, &snapshot.configured);
    json!({"model_options": options, "model_options_truncated": cut || snapshot.models_truncated,
           "providers": snapshot.providers, "providers_truncated": snapshot.providers_truncated,
           "settings_model": snapshot.default})
}

/// The catalog answer. Its reason can carry server text to phones, so it is
/// presented as session rows present OpenCode's messages (`server_reason`).
fn answer(row: &Value, source: &str, state: &str, reason: &str, at: Value) -> Value {
    let mut answer = row.clone();
    answer["model_catalog_source"] = json!(source);
    answer["catalog_state"] = json!(state);
    answer["catalog_reason"] = json!(super::server_reason(reason));
    answer["catalog_at"] = at;
    answer
}

fn unavailable(why: &Unavailable, home_location: bool) -> Value {
    let state = match why {
        Unavailable::NotRunning => "not_running",
        Unavailable::Disabled => "disabled",
        Unavailable::Incompatible(_) => "incompatible",
        Unavailable::Unreachable(_) => "unreachable",
    };
    let reason = why.reason(&crate::platform::hostname());
    match cached().filter(|_| home_location) {
        Some(last) => {
            let mut row = empty_row();
            row["model_options"] = last["model_options"].clone();
            row["settings_model"] = last["settings_model"].clone();
            answer(&row, "cache", state, &reason, last["catalog_at"].clone())
        }
        None => answer(&empty_row(), "unavailable", state, &reason, Value::Null),
    }
}

/// Never starts the service. Falls back to the last known home catalog.
pub(crate) fn catalog(location: Option<&Path>) -> Value {
    let home_dir = home();
    let location = location.unwrap_or(&home_dir);
    let is_home = location == home_dir;
    let service = match connect() {
        Ok(service) => service,
        Err(why) => return unavailable(&why, is_home),
    };
    match read_snapshot(&service, location, is_home) {
        Ok(snapshot) => {
            let at = json!(now());
            let row = row_from(&snapshot, &favorites(), &[]);
            if !snapshot.loaded {
                return answer(&row, "opencode_service", "loading", "OpenCode is still loading providers", at);
            }
            if is_home {
                let record = json!({"location": location.to_string_lossy(), "model_options": row["model_options"],
                    "settings_model": snapshot.default, "catalog_at": at});
                if let Ok(text) = serde_json::to_string(&record) {
                    if text.len() as u64 <= MAX_CACHE {
                        let _ = atomic(&cache_path(), &text);
                    }
                }
            }
            answer(&row, "opencode_service", "ready", "", at)
        }
        Err(reason) => unavailable(&Unavailable::Unreachable(reason), is_home),
    }
}
```

`parent_view` keeps calling `catalog(Some(&cwd))["model_options"]` in this task (Task 4b pins the session's model and stops reading providers there).

`src/state/opencode.rs`: `#[cfg_attr(not(unix), allow(dead_code))] mod shortlist;` and

```rust
// Accounts fits the OpenCode row only on Unix.
#[cfg(unix)]
pub(crate) use shortlist::{fit_row, ROW_BUDGET};
```

`src/accounts.rs` — in the OpenCode row block copy the new keys:

```rust
            for key in ["model_options", "model_options_truncated", "providers", "providers_truncated", "settings_model", "catalog_state", "catalog_reason", "catalog_at"] {
                row[key] = models[key].clone();
            }
```

and after the `for row in &mut rows { … permission_status::describe(…) }` loop:

```rust
    // The connector forwards at most 128 KiB of accounts; this row keeps within 32 KiB.
    #[cfg(unix)]
    for row in rows.iter_mut().filter(|r| r["id"] == "native-opencode" && r["model_options"].is_array()) {
        crate::state::opencode::fit_row(row, crate::state::opencode::ROW_BUDGET);
    }
```

The connector gives `hgs account ls` 3 seconds; the row adds one loopback `GET /api/integration`, bounded to 1 second by Task 2's `call_within`, to the three catalog reads.

`docs/opencode.md`, `## Models`: describe the short list (default model, favorites, configured models, free models; each once; at most 256), favorites stored on the agent computer (`opencode-favorites.json`, at most 64; unavailable favorites stay until removed), All models on the desktop, the 32 KiB row and what is dropped first, that a session's model picker uses its folder's list plus its current model, and that phones list only model values of at most 120 characters (longer ones are chosen on the desktop or with `hgs`).

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo +1.99.1 test --locked opencode:: && cargo +1.99.1 build --locked && PYTHONPATH=tests python3 -m unittest test_accounts test_opencode_provider_fixture -v && cargo +1.99.1 clippy --locked --all-targets -- -D warnings && RUSTUP_TOOLCHAIN=1.99.1 bash scripts/ci/check-windows-target.sh clippy`
Expected: PASS; no warnings on Linux or the Windows target.

- [ ] **Step 5: Commit**

```bash
git add src/state/opencode/shortlist.rs src/state/opencode/favorites.rs src/state/opencode/catalog.rs src/state/opencode.rs src/accounts.rs tests/test_accounts.py docs/opencode.md
git commit -m "Offer the OpenCode short list with providers within a 32 KiB account row"
```

---

### Task 4b: hgs — the stopped-service catalog and session details

**Files:**
- Modify: `src/state/opencode/catalog.rs` (`cache_record`, `cached_at`, `snapshot_from_cache`, `unavailable`, `catalog_with`, `session_options`), `src/state/opencode.rs` (`parent_view` pins the session's model; `#[cfg(unix)]` on `pub(crate) use catalog::catalog;`)
- Test: unit tests in `catalog.rs`

**Interfaces:**
- Consumes: `row_from`, `shortlist::ordered`, `answer`, `empty_row` (Task 4a).
- Produces:
  - `pub(super) fn cache_record(location: &Path, snapshot: &Snapshot, favorites: &[String], at: &Value) -> Value` → `{"version":2,"location","model_options","candidates","providers","providers_truncated","settings_model","catalog_at"}`; `model_options` is the short list at write time, so a rolled-back hgs still shows it.
  - `cached_at(path, home)` accepts a record whose `candidates` or `model_options` is an array, for the home location only.
  - `pub(super) fn snapshot_from_cache(cache: &Value) -> Snapshot` (version 1 records: `model_options` as candidates)
  - `pub(crate) fn session_options(location: &Path, pinned: &[&str]) -> Value` → `{"model_options","model_options_truncated"}`; no `/api/integration` read, no cache write.

- [ ] **Step 1: Write the failing tests**

Add to the `catalog.rs` tests:

```rust
    #[test]
    fn a_stopped_service_rebuilds_the_short_list_from_cached_candidates() {
        let fixture = shared();
        let service = &fixture["service"];
        let snapshot = snapshot_from(&service["provider"], &service["model"], &service["default"], Some(&service["integration"]));
        let favorites: Vec<String> = serde_json::from_value(fixture["favorites"].clone()).unwrap();
        let record = cache_record(Path::new("/home/example"), &snapshot, &favorites, &json!(1792267200.5));
        let text = record.to_string();
        assert!(!text.contains("fixture-secret-key") && !text.contains("solo-model"), "{text}");
        assert_eq!(record["model_options"], fixture["row"]["model_options"]);
        let cached = snapshot_from_cache(&record);
        assert_eq!(row_from(&cached, &favorites, &[])["model_options"], fixture["row"]["model_options"]);
        assert_eq!(row_from(&cached, &favorites, &[])["providers"], fixture["row"]["providers"]);
        // Unstarring while stopped drops a model that was listed only as a favorite.
        let fewer = row_from(&cached, &favorites[1..], &[]);
        assert!(fewer["model_options"].as_array().unwrap().iter().all(|o| o["value"] != "keyed/paid-model"));
        // A version 1 cache (model_options only) still answers.
        let old = json!({"location": "/home/example", "model_options": fixture["row"]["model_options"], "settings_model": "fixture/local", "catalog_at": 1});
        assert_eq!(row_from(&snapshot_from_cache(&old), &[], &[])["model_options"][0]["value"], "fixture/local");
    }

    #[test]
    fn the_cache_file_round_trips_through_cached_at() {
        let fixture = shared();
        let service = &fixture["service"];
        let snapshot = snapshot_from(&service["provider"], &service["model"], &service["default"], Some(&service["integration"]));
        let favorites: Vec<String> = serde_json::from_value(fixture["favorites"].clone()).unwrap();
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("catalog.json");
        let home = Path::new("/home/example");
        atomic(&path, &cache_record(home, &snapshot, &favorites, &json!(1792267200.5)).to_string()).unwrap();
        let read = cached_at(&path, home).expect("a version 2 record is read back");
        assert_eq!(row_from(&snapshot_from_cache(&read), &favorites, &[])["model_options"], fixture["row"]["model_options"]);
        assert_eq!(read["catalog_at"], json!(1792267200.5));
        assert!(cached_at(&path, Path::new("/home/example/project")).is_none());
    }
```

Extend `cache_is_ignored_for_another_location`:

```rust
        let v2 = json!({"version": 2, "location": "/srv/a", "candidates": [], "providers": [], "settings_model": "", "catalog_at": 1});
        std::fs::write(&path, v2.to_string()).unwrap();
        assert!(cached_at(&path, Path::new("/srv/a")).is_some());
        assert!(cached_at(&path, Path::new("/srv/b")).is_none());
        std::fs::write(&path, json!({"location": "/srv/a", "settings_model": ""}).to_string()).unwrap();
        assert!(cached_at(&path, Path::new("/srv/a")).is_none());
```

`session_options` passes `pinned` straight to `row_from`, whose pinning Task 4a's `a_pinned_model_follows_the_default_once_with_its_efforts` covers; native `test_20` (Task 12) checks it end to end.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cargo +1.99.1 test --locked opencode::catalog`
Expected: compile errors (`cache_record`, `snapshot_from_cache` not defined); with stubs, `the_cache_file_round_trips_through_cached_at` fails because `cached_at` refuses a record without `model_options`.

- [ ] **Step 3: Implement**

```rust
const CACHE_CANDIDATES: usize = 512;

/// What the short list can draw on without the service: the default, every
/// favorite, configured and free model in short-list priority (at most 512),
/// each marked `configured`; `model_options` is the short list at write time.
pub(super) fn cache_record(location: &Path, snapshot: &Snapshot, favorites: &[String], at: &Value) -> Value {
    let (candidates, _) = shortlist::ordered(&snapshot.models, &snapshot.default, &[], favorites, &snapshot.configured, CACHE_CANDIDATES);
    let candidates: Vec<Value> = candidates
        .into_iter()
        .map(|mut m| {
            let configured = m["provider"].as_str().is_some_and(|p| snapshot.configured.contains(p));
            if let Some(map) = m.as_object_mut() {
                map.remove("favorite");
                map.remove("default");
                map.insert("configured".into(), json!(configured));
            }
            m
        })
        .collect();
    json!({"version": 2, "location": location.to_string_lossy(),
           "model_options": row_from(snapshot, favorites, &[])["model_options"], "candidates": candidates,
           "providers": snapshot.providers, "providers_truncated": snapshot.providers_truncated,
           "settings_model": snapshot.default, "catalog_at": at})
}

fn cached_at(path: &Path, home: &Path) -> Option<Value> {
    let file = std::fs::File::open(path).ok()?;
    let mut bytes = Vec::new();
    file.take(MAX_CACHE + 1).read_to_end(&mut bytes).ok()?;
    if bytes.len() as u64 > MAX_CACHE {
        return None;
    }
    let value: Value = serde_json::from_slice(&bytes).ok()?;
    let listed = value["candidates"].is_array() || value["model_options"].is_array();
    (listed && value["location"].as_str() == home.to_str()).then_some(value)
}

/// A cached record as a snapshot; version 1 records list `model_options` only.
pub(super) fn snapshot_from_cache(cache: &Value) -> Snapshot {
    let entries = cache["candidates"].as_array().or_else(|| cache["model_options"].as_array()).cloned().unwrap_or_default();
    let mut configured = HashSet::new();
    let mut models = Vec::new();
    for mut entry in entries {
        let Some(map) = entry.as_object_mut() else { continue };
        let is_configured = map.remove("configured") == Some(json!(true));
        map.remove("favorite");
        map.remove("default");
        let (Some(_), Some(provider)) = (map.get("value").and_then(Value::as_str), map.get("provider").and_then(Value::as_str)) else { continue };
        if is_configured {
            configured.insert(provider.to_owned());
        }
        models.push(entry);
    }
    Snapshot {
        models,
        models_truncated: false,
        default: cache["settings_model"].as_str().unwrap_or_default().to_owned(),
        configured,
        providers: cache["providers"].as_array().cloned().unwrap_or_default(),
        providers_truncated: cache["providers_truncated"] == true,
        loaded: true,
    }
}

fn unavailable(why: &Unavailable, home_location: bool, favorites: &[String], pinned: &[&str]) -> Value {
    let state = match why {
        Unavailable::NotRunning => "not_running",
        Unavailable::Disabled => "disabled",
        Unavailable::Incompatible(_) => "incompatible",
        Unavailable::Unreachable(_) => "unreachable",
    };
    let reason = why.reason(&crate::platform::hostname());
    match cached().filter(|_| home_location) {
        Some(last) => answer(&row_from(&snapshot_from_cache(&last), favorites, pinned), "cache", state, &reason, last["catalog_at"].clone()),
        None => answer(&empty_row(), "unavailable", state, &reason, Value::Null),
    }
}

/// Never starts the service. Falls back to the last known home catalog.
pub(crate) fn catalog(location: Option<&Path>) -> Value {
    catalog_with(location, true, &[])
}

/// The short list of a session's folder for session details, with the
/// session's current and pending model right after the default: no
/// providers, so no `/api/integration` read, and no cache write.
pub(crate) fn session_options(location: &Path, pinned: &[&str]) -> Value {
    let answer = catalog_with(Some(location), false, pinned);
    json!({"model_options": answer["model_options"], "model_options_truncated": answer["model_options_truncated"]})
}

fn catalog_with(location: Option<&Path>, providers: bool, pinned: &[&str]) -> Value {
    let home_dir = home();
    let location = location.unwrap_or(&home_dir);
    let is_home = location == home_dir;
    let favorites = favorites();
    let service = match connect() {
        Ok(service) => service,
        Err(why) => return unavailable(&why, is_home, &favorites, pinned),
    };
    match read_snapshot(&service, location, providers && is_home) {
        Ok(snapshot) => {
            let at = json!(now());
            let row = row_from(&snapshot, &favorites, pinned);
            if !snapshot.loaded {
                return answer(&row, "opencode_service", "loading", "OpenCode is still loading providers", at);
            }
            if providers && is_home {
                if let Ok(text) = serde_json::to_string(&cache_record(location, &snapshot, &favorites, &at)) {
                    if text.len() as u64 <= MAX_CACHE {
                        let _ = atomic(&cache_path(), &text);
                    }
                }
            }
            answer(&row, "opencode_service", "ready", "", at)
        }
        Err(reason) => unavailable(&Unavailable::Unreachable(reason), is_home, &favorites, pinned),
    }
}
```

Session details of a session in the home folder fall back to the cached short list like the account row does.

`src/state/opencode.rs` `parent_view` (Full detail): pin the session's own model so its efforts stay offered even when it is neither default, favorite, configured nor free:

```rust
    if detail == Detail::Full {
        let pinned: Vec<String> = ["settings_model", "pending_model"]
            .iter()
            .filter_map(|key| result[*key].as_str())
            .filter(|value| !value.is_empty())
            .map(str::to_owned)
            .collect();
        let pinned: Vec<&str> = pinned.iter().map(String::as_str).collect();
        let options = catalog::session_options(&PathBuf::from(string(record, "cwd")), &pinned);
        result["model_options"] = options["model_options"].clone();
        result["model_options_truncated"] = options["model_options_truncated"].clone();
    }
```

Add `"model_options_truncated"` next to `"model_options"` in the listing's removed keys. `parent_view` no longer uses the `catalog` re-export, which only `#[cfg(unix)]` accounts code reads now:

```rust
// Accounts reads the catalog, the install state and starts the service only on Unix.
#[cfg(unix)]
pub(crate) use catalog::{catalog, install_state, start_from_accounts};
```

(replacing the separate unconditional `pub(crate) use catalog::catalog;`).

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo +1.99.1 test --locked opencode:: && cargo +1.99.1 build --locked && PYTHONPATH=tests python3 -m unittest test_accounts -v && cargo +1.99.1 clippy --locked --all-targets -- -D warnings && RUSTUP_TOOLCHAIN=1.99.1 bash scripts/ci/check-windows-target.sh clippy`
Expected: PASS; no warnings on Linux or the Windows target.

- [ ] **Step 5: Commit**

```bash
git add src/state/opencode/catalog.rs src/state/opencode.rs
git commit -m "Keep the last OpenCode short list while the service is stopped and pin a session's model"
```

---

### Task 5: hgs — `hgs account models`, the full catalog of one folder

**Files:**
- Modify: `src/state/opencode/catalog.rs` (`MAX_FULL_BYTES`, `models_from`, `models`), `src/state/opencode.rs` (`#[cfg(unix)] pub(crate) use catalog::models;` — only `#[cfg(unix)]` accounts code calls it), `src/accounts.rs` (operation `models`), `src/cli.rs` (`hgs --help` line and its test), `docs/reference.md`, `tests/test_accounts.py`
- Test: unit tests in `catalog.rs`; `tests/test_accounts.py`

**Interfaces:**
- Consumes: `read_snapshot`, `row_from`, `unavailable`, `answer`, `favorites()` (Tasks 2, 4a, 4b); `providers::labels`.
- Produces:
  - `pub(crate) const MAX_FULL_BYTES: usize = 4 * 1024 * 1024;`
  - `pub(super) fn models_from(snapshot: &Snapshot, favorites: &[String], home_providers: Option<&HashSet<String>>, location: &Path) -> Value` (pure)
  - `pub(crate) fn models(location: Option<&Path>) -> Result<Value>` — never starts the service; `Err("Choose an existing folder on this computer")` for a relative or missing folder.
  - CLI: `hgs [@host] account models native-opencode [--location DIR] [--json]`

- [ ] **Step 1: Write the failing tests**

`catalog.rs` tests:

```rust
    #[test]
    fn the_full_catalog_marks_favorites_folders_and_unavailable_favorites() {
        let fixture = shared();
        let service = &fixture["service"];
        let snapshot = snapshot_from(&service["provider"], &service["model"], &service["default"], Some(&service["integration"]));
        let favorites: Vec<String> = serde_json::from_value(fixture["favorites"].clone()).unwrap();
        let home: HashSet<String> = ["opencode", "groq", "fixture", "ollama", "keyed"].map(String::from).into();
        let answer = models_from(&snapshot, &favorites, Some(&home), Path::new("/home/example/project"));
        assert_eq!(answer["location"], "/home/example/project");
        assert_eq!(answer["models"].as_array().unwrap().len(), 9);
        assert_eq!(answer["models_truncated"], false);
        assert_eq!(answer["model_options"], fixture["row"]["model_options"]);
        let providers = answer["providers"].as_array().unwrap();
        assert!(providers.iter().all(|p| (p["id"] == "solo") == (p["folder"] == true)));
        assert_eq!(answer["favorites"], json!([
            {"value": "keyed/paid-model", "available": true},
            {"value": "groq/openai/gpt-oss-20b", "available": true},
            {"value": "gone/missing", "available": false}]));
        let marked: Vec<&str> = answer["models"].as_array().unwrap().iter()
            .filter(|m| m["favorite"] == true).map(|m| m["value"].as_str().unwrap()).collect();
        assert_eq!(marked, ["groq/openai/gpt-oss-20b", "keyed/paid-model"]);
        assert_eq!(answer["models"].as_array().unwrap().iter().filter(|m| m["default"] == true).count(), 1);
        assert!(!serde_json::to_string(&answer).unwrap().contains("fixture-secret-key"));
        // The home folder marks no provider.
        let home_answer = models_from(&snapshot, &favorites, None, Path::new("/home/example"));
        assert!(home_answer["providers"].as_array().unwrap().iter().all(|p| p.get("folder").is_none()));
    }

    #[test]
    fn the_full_catalog_output_stays_within_four_mebibytes() {
        let mut many = json!({"data": []});
        let variants: Vec<Value> = (0..16).map(|v| json!({"id": format!("variant-{v:02}")})).collect();
        for i in 0..10_000 {
            many["data"].as_array_mut().unwrap().push(json!({"id": format!("m{i:05}"), "providerID": "p",
                "name": "\u{754c}".repeat(160), "enabled": true, "variants": variants}));
        }
        let snapshot = snapshot_from(&json!({"data": [{"id": "p", "name": "P", "integrationID": "p"}]}), &many, &json!({"data": null}), None);
        let answer = models_from(&snapshot, &[], None, Path::new("/home/example"));
        assert_eq!(answer["models_truncated"], true);
        // The final keys and a longest reason still fit.
        let full = super::answer(&answer, "opencode_service", "unreachable", &"\u{754c}".repeat(400), json!(1792267200.5));
        assert!(serde_json::to_string(&full).unwrap().len() <= MAX_FULL_BYTES);
        assert!(answer["models"].as_array().unwrap().len() > 1000);
    }
```

`tests/test_accounts.py`:

```python
    def test_opencode_full_catalog_never_starts_and_checks_its_folder(self):
        # Private OpenCode state: an inherited XDG_STATE_HOME must never reach a real service.
        self.env.update(XDG_STATE_HOME=str(self.root/'xdg-state'),XDG_CONFIG_HOME=str(self.root/'xdg-config'))
        binary=self.bin/"opencode";binary.write_text("#!/bin/sh\n[ \"$1\" = --version ] && echo v2.0.32 || exit 9\n");binary.chmod(0o700)
        catalog=json.loads(self.run_hgs('account','models','native-opencode','--json').stdout)
        self.assertEqual((catalog['catalog_state'],catalog['models'],catalog['model_options']),('not_running',[],[]))
        self.assertEqual(catalog['location'],os.path.realpath(self.home))
        folder=self.root/'project';folder.mkdir()
        located=json.loads(self.run_hgs('account','models','native-opencode','--location',str(folder),'--json').stdout)
        self.assertEqual((located['location'],located['providers']),(os.path.realpath(folder),[]))
        for location in ('relative/folder',str(self.root/'missing')):
            refused=self.run_hgs('account','models','native-opencode','--location',location,ok=False)
            self.assertIn(b'Choose an existing folder on this computer',refused.stderr)
        refused=self.run_hgs('account','models','native-codex',ok=False)
        self.assertIn(b'Only the native OpenCode account lists OpenCode models',refused.stderr)
        self.assertFalse((self.root/'xdg-state/opencode').exists(),'a service registration appeared')
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cargo +1.99.1 test --locked opencode::catalog; cargo +1.99.1 build --locked && PYTHONPATH=tests python3 -m unittest test_accounts.Accounts.test_opencode_full_catalog_never_starts_and_checks_its_folder -v`
Expected: compile errors (`models_from`, `MAX_FULL_BYTES`); the CLI test fails with the usage error.

- [ ] **Step 3: Implement**

```rust
pub(crate) const MAX_FULL_BYTES: usize = 4 * 1024 * 1024;
/// Room for the keys `answer` adds afterwards (state, source, a reason of at most
/// 301 characters, the time).
const ANSWER_RESERVE: usize = 2048;

pub(super) fn models_from(snapshot: &Snapshot, favorites: &[String], home_providers: Option<&HashSet<String>>, location: &Path) -> Value {
    let mut answer = row_from(snapshot, favorites, &[]);
    if let (Some(home), Some(providers)) = (home_providers, answer["providers"].as_array_mut()) {
        for provider in providers {
            if provider["id"].as_str().is_some_and(|id| !home.contains(id)) {
                provider["folder"] = json!(true);
            }
        }
    }
    answer["location"] = json!(location.to_string_lossy());
    answer["favorites"] = json!(favorites.iter().map(|f| json!({"value": f,
        "available": snapshot.models.iter().any(|m| m["value"] == f.as_str())})).collect::<Vec<_>>());
    answer["models_truncated"] = json!(snapshot.models_truncated);
    answer["models"] = json!([]);
    let mut size = serde_json::to_string(&answer).map_or(usize::MAX, |t| t.len());
    let mut models = Vec::new();
    for model in &snapshot.models {
        let mut row = model.clone();
        let value = row["value"].as_str().unwrap_or_default().to_owned();
        if favorites.iter().any(|f| *f == value) {
            row["favorite"] = json!(true);
        }
        if value == snapshot.default {
            row["default"] = json!(true);
        }
        let add = serde_json::to_string(&row).map_or(usize::MAX, |t| t.len()) + usize::from(!models.is_empty());
        if size.saturating_add(add) > MAX_FULL_BYTES - ANSWER_RESERVE {
            answer["models_truncated"] = json!(true);
            break;
        }
        size += add;
        models.push(row);
    }
    answer["models"] = json!(models);
    answer
}

/// `hgs account models`: the full catalog of one folder for the desktop.
pub(crate) fn models(location: Option<&Path>) -> Result<Value> {
    const NO_FOLDER: &str = "Choose an existing folder on this computer";
    let home_dir = std::fs::canonicalize(home()).unwrap_or_else(|_| home());
    let location = match location {
        None => home_dir.clone(),
        Some(path) if path.is_absolute() => std::fs::canonicalize(path).ok().filter(|p| p.is_dir()).ok_or(NO_FOLDER)?,
        Some(_) => return Err(NO_FOLDER.into()),
    };
    let is_home = location == home_dir;
    let favorites = favorites();
    let stopped = |why: &Unavailable| {
        let mut answer = unavailable(why, is_home, &favorites, &[]);
        answer["location"] = json!(location.to_string_lossy());
        answer["models"] = json!([]);
        answer["models_truncated"] = json!(false);
        answer["favorites"] = json!(favorites.iter().map(|f| json!({"value": f})).collect::<Vec<_>>());
        answer
    };
    let service = match connect() {
        Ok(service) => service,
        Err(why) => return Ok(stopped(&why)),
    };
    let read = read_snapshot(&service, &location, true).and_then(|snapshot| {
        let home_providers = if is_home {
            None
        } else {
            let listed = service.call("GET", &query("/api/provider", &home_dir), None)?;
            Some(super::providers::labels(&listed).into_keys().collect::<HashSet<_>>())
        };
        Ok((snapshot, home_providers))
    });
    match read {
        Ok((snapshot, home_providers)) => {
            let state = if snapshot.loaded { "ready" } else { "loading" };
            let reason = if snapshot.loaded { "" } else { "OpenCode is still loading providers" };
            let row = models_from(&snapshot, &favorites, home_providers.as_ref(), &location);
            Ok(answer(&row, "opencode_service", state, reason, json!(now())))
        }
        Err(reason) => Ok(stopped(&Unavailable::Unreachable(reason))),
    }
}
```

`src/accounts.rs` arm (before `"favorite"`):

```rust
        "models"=>{
            if id!="native-opencode" {return Err(Error::new(1,"Only the native OpenCode account lists OpenCode models"));}
            #[cfg(unix)]
            {let location=arg(args,"--location").map(PathBuf::from);println!("{}",crate::state::opencode::models(location.as_deref()).map_err(|e|Error::new(1,e))?);}
            #[cfg(not(unix))]
            return Err(Error::new(2,platform::LOCAL_SESSIONS_UNSUPPORTED));
        },
```

Extend the usage string with `| models native-opencode [--location DIR]`. `src/cli.rs` — add after the `account favorite` help line:

```text
       hgs [@host] account models native-opencode [--location DIR]   every OpenCode model of that computer and folder as JSON; never starts OpenCode
```

and assert it in `usage_lists_opencode_effort_and_the_explicit_service_start`:

```rust
        assert!(usage.contains(
            "\n       hgs [@host] account models native-opencode [--location DIR]   every OpenCode model of that computer and folder as JSON; never starts OpenCode\n"
        ), "{usage}");
``` `docs/reference.md`: "`hgs [@host] account models native-opencode [--location DIR] --json` lists every available model of that computer and folder (connected providers only, at most 10,000 models and 4 MiB; `models_truncated` reports a cut). It never starts OpenCode; with the service stopped it reports `not_running` and, for the home folder, the last known short list."

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo +1.99.1 test --locked opencode:: && cargo +1.99.1 test --locked usage_lists_opencode && cargo +1.99.1 build --locked && PYTHONPATH=tests python3 -m unittest test_accounts -v && cargo +1.99.1 clippy --locked --all-targets -- -D warnings && RUSTUP_TOOLCHAIN=1.99.1 bash scripts/ci/check-windows-target.sh clippy`
Expected: PASS; no warnings on Linux or the Windows target.

- [ ] **Step 5: Commit**

```bash
git add src/state/opencode/catalog.rs src/state/opencode.rs src/accounts.rs src/cli.rs tests/test_accounts.py docs/reference.md
git commit -m "List every OpenCode model of a folder for the desktop on demand"
```

---

### Task 6: Mobile connector — providers, marks and truncation within 128 KiB

**Files:**
- Modify: `services/mobile/connector/src/accounts.rs`
- Create: `services/mobile/connector/tests/opencode_providers.rs`
- Modify: `docs/mobile-architecture.md` (section "Read-only Accounts")
- Test: `cd services/mobile/connector && cargo +1.99.1 test --locked`

**Interfaces:**
- Consumes: the Task 1 fixture (`hgs_account`, `heartbeat_account`, `launch_account`, `ascii_lengths`).
- Produces:
  - `model_options(profile)` adds `favorite: true` / `default: true` when the hgs entry has `true`.
  - `opencode_status(profile, public)` adds `catalog_at` (a bounded number) and `model_options_truncated` (bool).
  - `pub fn opencode_providers(profile: &Map<String, Value>, public: &mut Map<String, Value>)`: `providers` (≤64 valid entries) and `providers_truncated` (bool), heartbeat only.
  - Degrade order over the bound: `effort_labels`, `effort_options`, then `providers` (sets `providers_truncated: true`).

Provider entry rules: object with `id` matching `[A-Za-z0-9_][A-Za-z0-9_.-]{0,79}` (the existing model-provider rule; move it into `fn provider_identity(&str) -> bool` and use it in `model_options` too), else dropped; `label` = `text(label, 160)` when non-empty, else the ID; `kind` only when one of the eight kinds; `credential_count` only for integers 0–64; `model_count` only for integers 0–10,000; `active_label` = `text(active_label, 80)` only when non-empty. Any other key is dropped. More than 64 valid entries → the first 64 and `providers_truncated: true`.

- [ ] **Step 1: Write the failing tests**

`services/mobile/connector/tests/opencode_providers.rs`:

```rust
//! OpenCode providers phase 1: the connector forwards only allowlisted
//! provider and model fields, pinned by tests/fixtures/opencode-provider-catalog.json.
use serde_json::{json, Value};
use zerus_mobile_connector::accounts;
use zerus_relay::json as pyjson;

const CONTRACT: &str = include_str!("../../../../tests/fixtures/opencode-provider-catalog.json");
const NOW: f64 = 1_792_267_200.0;

fn contract() -> Value {
    serde_json::from_str(CONTRACT).unwrap()
}

fn view(profiles: Value) -> Value {
    let mut cache = accounts::AccountCache::default();
    cache.apply_catalog(accounts::catalog(&json!({"profiles": profiles})), NOW);
    cache.view(NOW)
}

#[test]
fn the_ascii_measure_is_the_one_hgs_budgets_with() {
    for case in contract()["ascii_lengths"].as_array().unwrap() {
        assert_eq!(pyjson::python_default_len(&case["value"], true).unwrap() as u64, case["length"].as_u64().unwrap());
    }
}

#[test]
fn the_heartbeat_forwards_allowlisted_providers_and_marks() {
    let c = contract();
    assert_eq!(view(json!([c["hgs_account"]]))["accounts"][0], c["heartbeat_account"]);
}

#[test]
fn the_launch_catalog_forwards_marks_but_not_providers() {
    let c = contract();
    let launch = accounts::launch_catalog(&json!({"profiles": [c["hgs_account"]]})).unwrap();
    assert_eq!(launch["accounts"][0], c["launch_account"]);
    assert_eq!(launch["agents"], json!(["opencode"]));
}

#[test]
fn hostile_provider_entries_are_dropped_or_bounded() {
    let mut row = contract()["hgs_account"].clone();
    let mut providers = vec![
        json!({"id": "bad id", "label": "X", "kind": "key"}),
        json!("not an object"),
        json!({"id": "ok", "label": "\u{1b}[31m", "kind": "magic", "credential_count": -1, "model_count": 10_001,
               "active_label": "", "settings": {"apiKey": "fixture-secret-key"}, "url": "https://example.invalid"}),
    ];
    providers.extend((0..70).map(|i| json!({"id": format!("p{i}"), "label": format!("P{i}"), "kind": "env",
        "credential_count": 0, "model_count": 1})));
    row["providers"] = json!(providers);
    let shown = view(json!([row]));
    let listed = shown["accounts"][0]["providers"].as_array().unwrap();
    assert_eq!(listed.len(), 64);
    assert_eq!(listed[0], json!({"id": "ok", "label": "[31m"}));
    assert_eq!(listed[1], json!({"id": "p0", "label": "P0", "kind": "env", "credential_count": 0, "model_count": 1}));
    assert_eq!(shown["accounts"][0]["providers_truncated"], true);
    assert!(!shown.to_string().contains("fixture-secret-key") && !shown.to_string().contains("example.invalid"));
}

fn others() -> Vec<Value> {
    (0..99)
        .map(|i| json!({"id": format!("work-{i:02}"), "provider": "codex", "label": format!("Work {i:02} {}", "w".repeat(150)),
            "native": false, "installed": true, "is_default": false, "home": format!("/home/example/.config/hgs/accounts/work-{i:02}"),
            "auth_revision": format!("rev-{i}"), "credential_file": true,
            "account_status": {"status": "ok", "identity": {"email": format!("work-{i:02}@example.invalid"), "plan": "Example plan"},
                "windows": [{"id": "five_hour", "label": "Session", "used_percent": 42.5, "window_minutes": 300, "resets_at": 1792270800}],
                "checked_at": 1792267200}}))
        .collect()
}

#[test]
fn an_opencode_row_at_its_budget_fits_beside_ninety_nine_accounts() {
    let c = contract();
    let mut row = c["hgs_account"].clone();
    let template = row["model_options"][5].clone();
    for index in 0.. {
        let mut entry = template.clone();
        let value = format!("opencode/budget-{index:04}");
        entry["id"] = json!(value);
        entry["value"] = json!(value);
        entry["label"] = json!(format!("Budget {index:04} {}", "\u{754c}".repeat(8)));
        let mut next = row.clone();
        next["model_options"].as_array_mut().unwrap().push(entry);
        if pyjson::python_default_len(&next, true).unwrap() > 32 * 1024 {
            break;
        }
        row = next;
    }
    assert!(pyjson::python_default_len(&row, true).unwrap() > 31 * 1024);
    let mut profiles = vec![row.clone()];
    profiles.extend(others());
    let shown = view(json!(profiles));
    assert_eq!(shown["available"], true);
    assert!(pyjson::python_default_len(&shown, true).unwrap() <= accounts::MAX_CATALOG_BYTES);
    let opencode = &shown["accounts"][0];
    assert_eq!(opencode["model_options"].as_array().unwrap().len(), row["model_options"].as_array().unwrap().len());
    assert_eq!(opencode["providers"], c["heartbeat_account"]["providers"]);
    assert!(opencode["model_options"][5]["effort_options"].is_array(), "nothing was degraded");
}

#[test]
fn an_oversized_projection_drops_providers_after_efforts_and_keeps_every_account() {
    let c = contract();
    let mut row = c["hgs_account"].clone();
    row["providers"] = json!((0..64).map(|i| json!({"id": format!("p{i:02}"), "label": "\u{754c}".repeat(160),
        "kind": "key", "credential_count": 1, "model_count": 1, "active_label": "\u{754c}".repeat(80)})).collect::<Vec<_>>());
    let mut profiles = vec![row];
    profiles.extend(others());
    let shown = view(json!(profiles));
    assert_eq!(shown["available"], true);
    assert_eq!(shown["accounts"].as_array().unwrap().len(), 100);
    let opencode = &shown["accounts"][0];
    assert!(opencode.get("providers").is_none());
    assert_eq!(opencode["providers_truncated"], true);
    assert!(opencode["model_options"][0].get("effort_options").is_none());
    assert_eq!(opencode["model_options"][0]["value"], "fixture/local");
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cd services/mobile/connector && cargo +1.99.1 test --locked --test opencode_providers`
Expected: FAIL — `heartbeat_account` mismatch (no `providers`, marks, `catalog_at`, truncation flags), missing `providers`, and the oversized case reports `available: false`.

- [ ] **Step 3: Implement**

In `services/mobile/connector/src/accounts.rs`:

```rust
const KINDS: [&str; 8] = ["free", "key", "oauth", "env", "config", "command", "external", "local"];
const MAX_PROVIDERS: usize = 64;

/// The provider part of an OpenCode model value and an OpenCode provider ID.
fn provider_identity(provider: &str) -> bool {
    provider.bytes().next().is_some_and(|b| b.is_ascii_alphanumeric() || b == b'_')
        && provider.len() <= 80
        && provider.bytes().all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b))
}

fn count(value: Option<&Value>, maximum: u64) -> Option<u64> {
    value.and_then(Value::as_u64).filter(|n| *n <= maximum)
}

/// OpenCode providers for Accounts: identity, kind and counts only.
pub fn opencode_providers(profile: &Map<String, Value>, public: &mut Map<String, Value>) {
    let Some(Value::Array(rows)) = profile.get("providers") else { return };
    let mut listed = Vec::new();
    let mut truncated = matches!(profile.get("providers_truncated"), Some(Value::Bool(true)));
    for row in rows.iter().take(1024) {
        let Some(row) = row.as_object() else { continue };
        let Some(id) = row.get("id").and_then(Value::as_str).filter(|id| provider_identity(id)) else { continue };
        if listed.len() == MAX_PROVIDERS {
            truncated = true;
            break;
        }
        let mut entry = Map::new();
        entry.insert("id".into(), json!(id));
        let label = text(row.get("label"), 160).filter(|l| !l.is_empty()).unwrap_or_else(|| id.into());
        entry.insert("label".into(), json!(label));
        if let Some(kind) = member(row.get("kind"), &KINDS) {
            entry.insert("kind".into(), json!(kind));
        }
        if let Some(n) = count(row.get("credential_count"), 64) {
            entry.insert("credential_count".into(), json!(n));
        }
        if let Some(n) = count(row.get("model_count"), 10_000) {
            entry.insert("model_count".into(), json!(n));
        }
        if let Some(label) = text(row.get("active_label"), 80).filter(|l| !l.is_empty()) {
            entry.insert("active_label".into(), json!(label));
        }
        listed.push(Value::Object(entry));
    }
    public.insert("providers".into(), Value::Array(listed));
    public.insert("providers_truncated".into(), json!(truncated));
}
```

In `model_options`, replace the inline provider check with `provider_identity(provider)` and after `free` add:

```rust
        for mark in ["favorite", "default"] {
            if let Some(Value::Bool(true)) = row.get(mark) {
                option.insert(mark.into(), json!(true));
            }
        }
```

In `opencode_status`, after the reasons:

```rust
    if let Some(at) = profile.get("catalog_at").filter(|value| bounded_number(value)) {
        public.insert("catalog_at".into(), at.clone());
    }
    if let Some(Value::Bool(truncated)) = profile.get("model_options_truncated") {
        public.insert("model_options_truncated".into(), json!(truncated));
    }
```

In `catalog()` for `provider == "opencode"` call `opencode_providers(profile, &mut public)` after `opencode_status`. Make the degrade sequence one function used by both `catalog()` and `fit_rows`:

```rust
/// One degradation step over every row: 0 drops `effort_labels`, 1
/// `effort_options`, 2 OpenCode `providers` (marking `providers_truncated`).
const DEGRADE_STEPS: usize = 3;
fn degrade(row: &mut Value, step: usize) {
    match step {
        0 => strip_model_key(row, "effort_labels"),
        1 => strip_model_key(row, "effort_options"),
        _ => {
            if let Some(object) = row.as_object_mut() {
                if object.remove("providers").is_some() {
                    object.insert("providers_truncated".into(), json!(true));
                }
            }
        }
    }
}
```

Replace `for key in DEGRADE_KEYS` in `fit_rows` with `for step in 0..DEGRADE_STEPS { for row in rows.iter_mut() { degrade(row, step) } … }`, and in `catalog()` apply the same steps to each `entry.public` (wrap it in `Value::Object` as the existing code does). Remove `DEGRADE_KEYS`.

`docs/mobile-architecture.md`, "Read-only Accounts": add that OpenCode rows may carry `providers` (≤64: `id`, `label`, `kind`, `credential_count`, `model_count`, `active_label`), `providers_truncated`, `model_options_truncated`, `catalog_at`, and model `favorite`/`default` marks (only `true`), that the launch catalog carries the marks but not providers, and that over the bound `effort_labels`, `effort_options` and then `providers` are dropped before the projection is refused.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd services/mobile/connector && cargo +1.99.1 test --locked && cargo +1.99.1 clippy --locked --all-targets -- -D warnings`
Expected: PASS, including the existing `runtime_vectors` and `launch_runtime` suites unchanged.

- [ ] **Step 5: Commit**

```bash
git add services/mobile/connector/src/accounts.rs services/mobile/connector/tests/opencode_providers.rs docs/mobile-architecture.md
git commit -m "Forward OpenCode providers, favorites and truncation through the connector"
```

---

### Task 7: Desktop — the grouped OpenCode model picker

**Files:**
- Create: `tray/src/OpenCodeModels.h`, `tray/src/OpenCodeModels.cpp`, `tray/src/OpenCodeModelPicker.h`, `tray/src/OpenCodeModelPicker.cpp`
- Modify: `tray/CMakeLists.txt` (add both `.cpp` files to `hgs-tray-ui`)
- Create: `tray/tests/test_opencodemodelpicker.cpp`; Modify: `tray/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `OpenCodeEffort::valid` (`tray/src/OpenCodeEffort.h`); the Task 1 fixture.
- Produces:

```cpp
// tray/src/OpenCodeModels.h
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// OpenCode models and providers as hgs reports them (account row
// `model_options`, `hgs account models`, session details), pinned by
// tests/fixtures/opencode-provider-catalog.json.
namespace OpenCodeModels {
struct Model {
    QString value, label, provider, providerLabel;
    bool free = false, favorite = false, isDefault = false;
    QStringList efforts, effortLabels;
};
struct Provider { QString id, label, kind, activeLabel; int credentials = -1, models = -1; bool folder = false; };
struct Group { QString provider, label; bool folder = false; QList<Model> models; };
// Non-empty, at most 256 UTF-8 bytes, contains '/', no control characters.
bool validValue(const QString &value);
// Valid, distinct entries (value, else id), at most `limit`; efforts as OpenCodeEffort::valid, at most 16.
QList<Model> parse(const QJsonArray &entries, int limit);
QList<Provider> providers(const QJsonArray &entries, int limit = 64);
// Provider groups in order of first appearance; models keep list order.
QList<Group> group(const QList<Model> &models, const QList<Provider> &providers = {});
// Every whitespace-separated word must match label, value or provider label (case-insensitive).
QList<Model> filter(const QList<Model> &models, const QString &query);
QString kindText(const QString &kind);
// "Default", "Favorite", "Free" in that order, joined by " · ".
QString marks(const Model &model);
// An OpenCode row or session details from an hgs that has `account models` and
// `account favorite`; every such hgs reports `model_options_truncated`.
bool catalogSupported(const QJsonObject &rowOrDetails);
// "Update Zerus on %1 to search all models and star favorites."
QString updateText(const QString &computer);
// A failure of those commands for display: an older hgs answers with its usage
// text (`usage: hgs account …`), shown as updateText(computer).
QString errorText(const QString &error, const QString &computer);
}
```

```cpp
// tray/src/OpenCodeModelPicker.h
#pragma once
#include "OpenCodeModels.h"
#include <QJsonObject>
#include <QWidget>
#include <optional>
class QLabel; class QLineEdit; class QPushButton; class QTreeWidget;

// The grouped OpenCode model list with search, Free/favorite/default marks, a
// star per model and All models. Owners fetch catalogs and favorites; the
// picker only asks for them.
class OpenCodeModelPicker : public QWidget {
    Q_OBJECT
public:
    explicit OpenCodeModelPicker(QWidget *parent = nullptr);
    void setShortList(const QJsonArray &options, bool truncated);
    void setCatalog(const QJsonObject &catalog);           // `hgs account models` output
    void setCatalogFailed(const QString &reason);
    void setCurrent(const QString &value);                  // an unknown value stays listed
    QString current() const;
    QStringList values() const;                             // rows shown now, in display order
    std::optional<OpenCodeModels::Model> model(const QString &value) const;
    void select(const QString &value);                      // as a click on that row
    void setFavoritesAvailable(bool available, const QString &reason = {});
    void setAllModelsAvailable(bool available, const QString &reason = {});   // off: the button is disabled with `reason` as tooltip
    void setFavoritePending(const QString &value, bool pending);
    void setFavorite(const QString &value, bool favorite);
    bool showingAll() const;
signals:
    void chosen(const QString &value);
    void favoriteRequested(const QString &value, bool favorite);
    void catalogRequested();
};
```

Child object names: `openCodeModelSearch` (QLineEdit, placeholder "Search models"), `openCodeModelList` (QTreeWidget, 2 columns: 0 = label and marks, 1 = star; top-level items are groups, children are models with `Qt::UserRole` = value), `openCodeAllModels` (checkable QPushButton "All models"), `openCodeModelNotice` (QLabel). Group header text: provider label, plus " · This folder" when `folder`. Star text: "★" (favorite) / "☆"; tooltips "Remove from favorites" / "Add to favorites"; while a change is pending the star shows "…" and clicks on it are ignored. `setCatalog` stores the catalog and never switches modes; it is listed while All models is on. With All models on, unavailable favorites from the catalog (`available: false`) form a last group "Unavailable favorites" whose rows cannot be chosen and whose star removes them. An unknown current value is listed first in a group named by its provider ID. Notices: short list truncated → "Some models are not in this short list. Use All models to search every model."; loading → "Loading all models…"; `models_truncated` → "Showing the first %1 models. Search to narrow the list."; failure → the reason; a catalog whose `catalog_state` is not `ready` → its `catalog_reason`. `setCatalogFailed` unchecks All models and shows the short list with the reason. Search runs 150 ms after the last keystroke and filters every group, the "Unavailable favorites" group included. At most 500 matching models are rendered; when more match, the notice is "Showing the first 500 matches. Search to narrow the list." (this takes precedence over the `models_truncated` notice). With All models unavailable the button is disabled, its tooltip is the given reason and no `catalogRequested` is emitted. `kindText`: free "Free, no key", key "API key", oauth "Signed in", env "Environment variable", config "Configuration", command "Command", external "External credentials", local "Detected locally", other "Connected".

- [ ] **Step 1: Write the failing tests**

`tray/tests/test_opencodemodelpicker.cpp`:

```cpp
#include "OpenCodeModelPicker.h"
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTreeWidget>

class TestOpenCodeModelPicker : public QObject {
    Q_OBJECT
    QJsonObject m_contract;
    static QTreeWidgetItem *row(QTreeWidget *tree, const QString &value) {
        for (int g = 0; g < tree->topLevelItemCount(); ++g)
            for (int m = 0; m < tree->topLevelItem(g)->childCount(); ++m)
                if (tree->topLevelItem(g)->child(m)->data(0, Qt::UserRole).toString() == value) return tree->topLevelItem(g)->child(m);
        return nullptr;
    }
    QJsonObject catalog() const {
        auto models = m_contract["row"].toObject()["model_options"].toArray();
        models.append(QJsonObject{{"id","solo/solo-model"},{"value","solo/solo-model"},{"label","Solo model"},{"provider","solo"},
            {"provider_label","Solo fixture"},{"free",false},{"effort_options",QJsonArray{"low"}},{"effort_labels",QJsonArray{"low"}}});
        auto providers = m_contract["row"].toObject()["providers"].toArray();
        auto solo = providers.last().toObject(); solo["folder"] = true; providers.replace(providers.size() - 1, solo);
        return {{"catalog_state","ready"},{"model_options",m_contract["row"].toObject()["model_options"]},{"model_options_truncated",false},
                {"providers",providers},{"models",models},{"models_truncated",true},
                {"favorites",QJsonArray{QJsonObject{{"value","gone/missing"},{"available",false}}}}};
    }
private slots:
    void initTestCase() {
        QFile file(QStringLiteral(ZERUS_OPENCODE_PROVIDER_CATALOG)); QVERIFY(file.open(QIODevice::ReadOnly));
        m_contract = QJsonDocument::fromJson(file.readAll()).object(); QVERIFY(!m_contract.isEmpty());
    }
    void parsesTheSharedShortListInOrderWithMarks() {
        const auto models = OpenCodeModels::parse(m_contract["row"].toObject()["model_options"].toArray(), 256);
        QStringList values; for (const auto &m : models) values << m.value;
        QCOMPARE(values, QStringList({"fixture/local","keyed/paid-model","groq/openai/gpt-oss-20b","fixture/alternate",
            "ollama/qwen3-coder:30b","opencode/glm-5.3-flash-free","opencode/big-pickle","groq/groq/compound"}));
        QCOMPARE(OpenCodeModels::marks(models[0]), QString("Default"));
        QCOMPARE(OpenCodeModels::marks(models[1]), QString("Favorite"));
        QCOMPARE(OpenCodeModels::marks(models[5]), QString("Free"));
        QCOMPARE(models[2].efforts, QStringList({"low","medium","high"}));
        const auto groups = OpenCodeModels::group(models);
        QStringList labels; for (const auto &g : groups) labels << g.label;
        QCOMPARE(labels, QStringList({"Private keyless fixture","Keyed fixture","Groq","Ollama","OpenCode"}));
        QCOMPARE(groups[2].models.size(), 2); QCOMPARE(groups[2].models[1].value, QString("groq/groq/compound"));
    }
    void providersNameTheirConnection() {
        const auto providers = OpenCodeModels::providers(m_contract["row"].toObject()["providers"].toArray());
        QCOMPARE(providers.size(), 6);
        QCOMPARE(OpenCodeModels::kindText(providers[0].kind), QString("Free, no key"));
        QCOMPARE(OpenCodeModels::kindText(providers[3].kind), QString("Detected locally"));
        QCOMPARE(providers[4].credentials, 2); QCOMPARE(providers[5].activeLabel, QString("Work"));
        QCOMPARE(OpenCodeModels::kindText("future-kind"), QString("Connected"));
    }
    void searchMatchesEveryWord() {
        const auto models = OpenCodeModels::parse(m_contract["row"].toObject()["model_options"].toArray(), 256);
        const auto values = [&](const QString &q) { QStringList v; for (const auto &m : OpenCodeModels::filter(models, q)) v << m.value; return v; };
        QCOMPARE(values("gpt 20B"), QStringList({"groq/openai/gpt-oss-20b"}));
        QCOMPARE(values("opencode"), QStringList({"opencode/glm-5.3-flash-free","opencode/big-pickle"}));
        QCOMPARE(values("keyless alternate"), QStringList({"fixture/alternate"}));
        QCOMPARE(values("  ").size(), 8);
    }
    void invalidEntriesAreDropped() {
        const QJsonArray entries{QJsonObject{{"value","noslash"}}, QJsonObject{{"value",QString("p/") + QString(255,'m')}},
            QJsonObject{{"value","p/a\u0007"}}, QJsonObject{{"id","p/ok"},{"label","OK"}}, QJsonObject{{"value","p/ok"}},
            QJsonObject{{"value","p/e"},{"effort_options",QJsonArray{"low","has space","a/b","low"}}}};
        const auto models = OpenCodeModels::parse(entries, 256);
        QCOMPARE(models.size(), 2); QCOMPARE(models[0].value, QString("p/ok")); QCOMPARE(models[1].efforts, QStringList({"low"}));
    }
    void groupsMarksAndStarsRequestFavorites() {
        OpenCodeModelPicker picker; picker.setShortList(m_contract["row"].toObject()["model_options"].toArray(), false); picker.show();
        auto *tree = picker.findChild<QTreeWidget *>("openCodeModelList"); QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 5);
        QVERIFY(row(tree, "fixture/local")->text(0).contains("Default"));
        QCOMPARE(row(tree, "keyed/paid-model")->text(1), QString("★")); QCOMPARE(row(tree, "fixture/alternate")->text(1), QString("☆"));
        QSignalSpy requested(&picker, &OpenCodeModelPicker::favoriteRequested), chosen(&picker, &OpenCodeModelPicker::chosen);
        emit tree->itemClicked(row(tree, "fixture/alternate"), 1);
        QCOMPARE(requested.size(), 1); QCOMPARE(requested[0], QVariantList({"fixture/alternate", true})); QCOMPARE(chosen.size(), 0);
        picker.setFavoritePending("fixture/alternate", true); QCOMPARE(row(tree, "fixture/alternate")->text(1), QString("…"));
        emit tree->itemClicked(row(tree, "fixture/alternate"), 1); QCOMPARE(requested.size(), 1);
        picker.setFavorite("fixture/alternate", true); QCOMPARE(row(tree, "fixture/alternate")->text(1), QString("★"));
        emit tree->itemClicked(row(tree, "groq/groq/compound"), 0); QCOMPARE(chosen.size(), 1); QCOMPARE(picker.current(), QString("groq/groq/compound"));
        picker.setFavoritesAvailable(false, "mac is offline.");
        emit tree->itemClicked(row(tree, "groq/groq/compound"), 1); QCOMPARE(requested.size(), 1);
        QCOMPARE(row(tree, "groq/groq/compound")->toolTip(1), QString("mac is offline."));
    }
    void allModelsRequestsTheCatalogOnceAndSearchesIt() {
        OpenCodeModelPicker picker; picker.setShortList(m_contract["row"].toObject()["model_options"].toArray(), true); picker.show();
        auto *notice = picker.findChild<QLabel *>("openCodeModelNotice"); auto *all = picker.findChild<QPushButton *>("openCodeAllModels");
        auto *search = picker.findChild<QLineEdit *>("openCodeModelSearch"); auto *tree = picker.findChild<QTreeWidget *>("openCodeModelList");
        QCOMPARE(notice->text(), QString("Some models are not in this short list. Use All models to search every model."));
        QSignalSpy requested(&picker, &OpenCodeModelPicker::catalogRequested), favorites(&picker, &OpenCodeModelPicker::favoriteRequested);
        all->click(); QCOMPARE(requested.size(), 1); QCOMPARE(notice->text(), QString("Loading all models…"));
        picker.setCatalog(catalog()); QVERIFY(picker.showingAll());
        QVERIFY(picker.values().contains("solo/solo-model"));
        QVERIFY(notice->text().startsWith("Showing the first 9 models."));
        bool folder = false; for (int g = 0; g < tree->topLevelItemCount(); ++g) folder |= tree->topLevelItem(g)->text(0) == "Solo fixture · This folder";
        QVERIFY(folder);
        search->setText("solo"); QTRY_COMPARE(picker.values(), QStringList({"solo/solo-model"}));
        search->clear(); QTRY_VERIFY(row(tree, "gone/missing")); QTreeWidgetItem *gone = row(tree, "gone/missing"); QCOMPARE(gone->text(1), QString("★"));
        emit tree->itemClicked(gone, 0); QVERIFY(picker.current() != "gone/missing");
        emit tree->itemClicked(gone, 1); QCOMPARE(favorites[0], QVariantList({"gone/missing", false}));
        all->click(); QVERIFY(!picker.showingAll()); all->click(); QCOMPARE(requested.size(), 1);
        QCOMPARE(picker.model("solo/solo-model")->efforts, QStringList({"low"}));
    }
    void anUnknownCurrentModelStaysListedAndFailuresExplain() {
        OpenCodeModelPicker picker; picker.setShortList(m_contract["row"].toObject()["model_options"].toArray(), false);
        picker.setCurrent("custom/model"); QCOMPARE(picker.values().first(), QString("custom/model"));
        picker.findChild<QPushButton *>("openCodeAllModels")->click();
        picker.setCatalogFailed("OpenCode is not running on mac");
        QCOMPARE(picker.findChild<QLabel *>("openCodeModelNotice")->text(), QString("OpenCode is not running on mac"));
        QVERIFY(!picker.showingAll()); QVERIFY(picker.values().contains("fixture/local"));
    }
    void largeCatalogsRenderAtMostFiveHundredMatches() {
        QJsonArray models;
        for (int i = 0; i < 2000; ++i)
            models.append(QJsonObject{{"value", QString("p/m%1").arg(i)}, {"label", QString("Model %1").arg(i)}, {"provider", "p"}, {"provider_label", "P"}});
        OpenCodeModelPicker picker; picker.setShortList({}, false);
        picker.findChild<QPushButton *>("openCodeAllModels")->click();
        picker.setCatalog({{"catalog_state","ready"},{"models",models},{"models_truncated",false}});
        QCOMPARE(picker.values().size(), 500);
        QCOMPARE(picker.findChild<QLabel *>("openCodeModelNotice")->text(), QString("Showing the first 500 matches. Search to narrow the list."));
        picker.findChild<QLineEdit *>("openCodeModelSearch")->setText("model 1999");
        QTRY_COMPARE(picker.values(), QStringList({"p/m1999"}));
    }
    void olderComputersOfferNeitherAllModelsNorStars() {
        QVERIFY(!OpenCodeModels::catalogSupported({{"model_options", QJsonArray{}}}));
        QVERIFY(OpenCodeModels::catalogSupported({{"model_options_truncated", false}}));
        QCOMPARE(OpenCodeModels::updateText("mac"), QString("Update Zerus on mac to search all models and star favorites."));
        QCOMPARE(OpenCodeModels::errorText("usage: hgs account ls | add ID --provider codex", "mac"), OpenCodeModels::updateText("mac"));
        QCOMPARE(OpenCodeModels::errorText("Account catalog is busy.", "mac"), QString("Account catalog is busy."));
        OpenCodeModelPicker picker; picker.setShortList(m_contract["row"].toObject()["model_options"].toArray(), false);
        picker.setAllModelsAvailable(false, OpenCodeModels::updateText("mac"));
        auto *all = picker.findChild<QPushButton *>("openCodeAllModels"); QVERIFY(!all->isEnabled());
        QCOMPARE(all->toolTip(), OpenCodeModels::updateText("mac"));
        QSignalSpy requested(&picker, &OpenCodeModelPicker::catalogRequested); all->click(); QCOMPARE(requested.size(), 0);
    }
};

QTEST_MAIN(TestOpenCodeModelPicker)
#include "test_opencodemodelpicker.moc"
```

`tray/tests/CMakeLists.txt`:

```cmake
add_executable(test_opencodemodelpicker test_opencodemodelpicker.cpp)
target_include_directories(test_opencodemodelpicker PRIVATE ../src)
target_link_libraries(test_opencodemodelpicker PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
target_compile_definitions(test_opencodemodelpicker PRIVATE ZERUS_OPENCODE_PROVIDER_CATALOG="${PROJECT_SOURCE_DIR}/../tests/fixtures/opencode-provider-catalog.json")
zerus_add_test(opencodemodelpicker test_opencodemodelpicker)
set_tests_properties(opencodemodelpicker PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
```

- [ ] **Step 2: Run them to verify they fail**

Run: `nice -n 10 bash scripts/ci/gui.sh build`
Expected: build fails (`OpenCodeModelPicker.h` not found).

- [ ] **Step 3: Implement**

`OpenCodeModels.cpp`: `parse` reads `value` (else `id`), `label` (≤160 characters, no control characters, else the value), `provider`, `provider_label` (else provider), `free`/`favorite`/`default` booleans, and efforts exactly as `NewSessionDialog::updateModels` validates them today (≤64 read, ≤16 kept, `OpenCodeEffort::valid`, distinct, label ≤80 characters else the ID). `group` keys groups by `provider`, labels them from the first model's `providerLabel` (or a matching `Provider.label`), and sets `folder` from a matching provider. `filter` lowercases with `QString::toCaseFolded()` and splits the query with `QRegularExpression("\\s+")`, `Qt::SkipEmptyParts`. `catalogSupported` is `rowOrDetails.contains("model_options_truncated")`; `errorText` returns `updateText(computer)` when the error starts with `usage: hgs account`, else the error.

`OpenCodeModelPicker.cpp`: a `QVBoxLayout` with the search field, the tree (`setHeaderHidden(true)`, `setRootIsDecorated(false)`, group items not selectable, expanded), a footer row with the All models button and the notice (word-wrapped). `render()` (run by a single-shot 150 ms search timer, and directly for every other change) rebuilds the tree from the short list or the catalog `models` (when `showingAll()`), through `filter` and `group`, then the "Unavailable favorites" group; it keeps the current row selected. `itemClicked(item, column)`: column 1 on a model row → `favoriteRequested(value, !favorite)` unless favorites are unavailable or the value is pending; column 0 on an available row → set current and emit `chosen`. `select(value)` calls the same path as a column 0 click. The All models button: checked without a catalog → emit `catalogRequested` once (later clicks reuse the catalog); unchecked → short list. Keyboard: Enter or Return on a model row chooses it.

Add `src/OpenCodeModels.cpp src/OpenCodeModelPicker.cpp` to the `hgs-tray-ui` source list in `tray/CMakeLists.txt`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `nice -n 10 bash scripts/ci/gui.sh build && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_opencodemodelpicker`
Expected: PASS (9 tests).

- [ ] **Step 5: Commit**

```bash
git add tray/src/OpenCodeModels.h tray/src/OpenCodeModels.cpp tray/src/OpenCodeModelPicker.h tray/src/OpenCodeModelPicker.cpp \
  tray/CMakeLists.txt tray/tests/test_opencodemodelpicker.cpp tray/tests/CMakeLists.txt
git commit -m "Add a grouped OpenCode model picker with search and favorites"
```

---

### Task 8a: Desktop — OpenCode providers in Accounts

**Files:**
- Modify: `tray/src/AccountsPage.h`, `tray/src/AccountsPage.cpp` (providers list and notes)
- Test: `tray/tests/test_accountspage.cpp`

**Interfaces:**
- Consumes: `OpenCodeModels::providers`, `OpenCodeModels::kindText` (Task 7); the row keys of Task 4a.
- Produces: `QTreeWidget` `openCodeProviders` and `QLabel` `openCodeProvidersNote` in the account details.

- [ ] **Step 1: Write the failing test**

In the fake `hgs` script's OpenCode block (`oc={…}`), add before `profiles.append(oc)`:

```python
  if (root/'opencode-providers').exists():
   oc['providers']=[{'id':'opencode','label':'OpenCode','kind':'free','credential_count':0,'model_count':12},
                    {'id':'keyed','label':'Keyed fixture','kind':'key','credential_count':2,'model_count':1},
                    {'id':'solo','label':'Solo fixture','kind':'oauth','credential_count':1,'active_label':'Work','model_count':1}]
   oc['providers_truncated']=(root/'opencode-truncated').exists()
   if state=='not_running':oc['catalog_at']=1792267200
```

Declare `void openCodeProvidersShowConnectionAndCounts();` beside `openCodeReasonsAndExplicitStart` and add:

```cpp
void TestAccountsPage::openCodeProvidersShowConnectionAndCounts()
{
    const auto write=[&](const QString &name,const QByteArray &text={}){QVERIFY(FakeCli::replace(m_dir.filePath(name),text));};
    write("opencode");write("opencode-providers");
    AccountsPage page(FakeCli::program(m_script));page.setFleet(fleet(false));page.show();
    auto *list=page.findChild<QListWidget *>("accountProfiles");QTRY_COMPARE(list->count(),5);
    page.showAccount({},"native-opencode");QTRY_COMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject()["provider"].toString(),QString("opencode"));
    auto *providers=page.findChild<QTreeWidget *>("openCodeProviders");QVERIFY(providers);QTRY_VERIFY(providers->isVisible());
    QCOMPARE(providers->topLevelItemCount(),3);
    const auto line=[&](int i){return QStringList{providers->topLevelItem(i)->text(0),providers->topLevelItem(i)->text(1),providers->topLevelItem(i)->text(2)};};
    QCOMPARE(line(0),QStringList({"OpenCode","Free, no key","12 models"}));
    QCOMPARE(line(1),QStringList({"Keyed fixture","API key · 2 accounts","1 model"}));
    QCOMPARE(line(2),QStringList({"Solo fixture","Signed in · Work","1 model"}));
    auto *note=page.findChild<QLabel *>("openCodeProvidersNote");QVERIFY(note);QVERIFY(!note->isVisible());
    write("opencode-truncated");write("opencode-state","not_running");page.ensureCatalogs(true);
    QTRY_VERIFY(note->isVisible());
    QVERIFY(note->text().contains("Showing the first 3 providers."));
    QVERIFY(note->text().contains("Last known catalog from"));
    for(int i=0;i<providers->topLevelItemCount();++i)for(int c=0;c<3;++c)QVERIFY(!providers->topLevelItem(i)->text(c).contains("http"));
    // Other accounts show no providers.
    page.showAccount({},"native-codex");QTRY_VERIFY(!providers->isVisible());QVERIFY(!note->isVisible());
}
```

Add `#include <QTreeWidget>` to the test if it is missing.

- [ ] **Step 2: Run it to verify it fails**

Run: `nice -n 10 bash scripts/ci/gui.sh build && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_accountspage`
Expected: the new test fails (`openCodeProviders` not found).

- [ ] **Step 3: Implement**

AccountsPage: a `QTreeWidget *m_openCodeProviders` (`openCodeProviders`, 3 columns, header hidden, no root decoration, not editable, at most 64 rows) and a `QLabel *m_openCodeProvidersNote` (`openCodeProvidersNote`, word wrap) below the OpenCode detail text. In the `nativeConfig` branch of the detail renderer fill them from `p["providers"]` through `OpenCodeModels::providers`: column 0 the label, column 1 `kindText(kind)` plus " · " + `active_label`, or " · N accounts" when `credential_count > 1`, column 2 "1 model" / "%n models". Notes: `providers_truncated` → "Showing the first %1 providers."; `catalog_state != "ready"` with a numeric `catalog_at` → "Last known catalog from %1." (`QLocale().toString(QDateTime::fromSecsSinceEpoch(at), QLocale::ShortFormat)`). Hide both for other accounts. Never show `home` or any URL in them.

- [ ] **Step 4: Run tests to verify they pass**

Run: `nice -n 10 bash scripts/ci/gui.sh build && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_accountspage`
Expected: PASS, existing tests included.

- [ ] **Step 5: Commit**

```bash
git add tray/src/AccountsPage.h tray/src/AccountsPage.cpp tray/tests/test_accountspage.cpp
git commit -m "Show connected OpenCode providers in Accounts"
```

---

### Task 8b: Desktop — the New session picker, folder catalogs and favorites

**Files:**
- Modify: `tray/src/HgsClient.h`, `tray/src/HgsClient.cpp` (two requests and their signals), `tray/src/NewSessionDialog.h`, `tray/src/NewSessionDialog.cpp` (picker button and popup, folder catalog, kept choices, favorites, older computers)
- Test: `tray/tests/test_newsessiondialog.cpp`

**Interfaces:**
- Consumes: `OpenCodeModelPicker`, `OpenCodeModels` (Task 7); CLI `account models` and `account favorite` (Tasks 3, 5).
- Produces (HgsClient):

```cpp
    // `hgs [@host] account models native-opencode --location DIR --json`: 30 s, at most 5 MiB of output.
    quint64 requestOpenCodeModels(const QString &host, const QString &location);
    // `hgs [@host] account favorite native-opencode --model VALUE --favorite on|off --request-id UUID --json`.
    quint64 requestOpenCodeFavorite(const QString &host, const QString &value, bool favorite);
signals:
    void openCodeModelsReady(quint64 request, const QString &host, const QString &location, const QJsonObject &catalog);
    void openCodeModelsFailed(quint64 request, const QString &host, const QString &location, const QString &error);
    void openCodeFavoriteReady(quint64 request, const QString &host, const QJsonObject &receipt);
    void openCodeFavoriteFailed(quint64 request, const QString &host, const QString &error);
```

The request ID is `QUuid::createUuid().toString(QUuid::WithoutBraces)`, one per click; a failure is reported, never retried automatically. The dialog shows failures through `OpenCodeModels::errorText(error, computer)` (Task 7), which turns an older hgs's usage text into `OpenCodeModels::updateText(computer)`; `OpenCodeModels::catalogSupported(row)` decides whether to call these commands at all.

- [ ] **Step 1: Write the failing tests**

1. In `init()` add the flag names `"model-calls","favorite-calls","favorite-fails","models-stopped","old-hgs","favorites"` to the cleared list.
2. Fake script: in `catalog()` add `'model_options_truncated':False` to the returned object unless `(root/'old-hgs').exists()` (an older hgs never reports it). Add branches before `elif a and a[0]=='dirs'`:

```python
elif a[:2]==['account','models']:
 with open(root/'model-calls','a') as calls:calls.write(json.dumps([host]+a)+'\n')
 if (root/'old-hgs').exists():sys.stderr.write('usage: hgs account ls | add ID --provider codex|claude|kimi --label NAME\n');sys.exit(1)
 if (root/'models-stopped').exists():
  print(json.dumps({'catalog_state':'not_running','catalog_reason':'OpenCode is not running on '+(host or 'pc'),'model_options':[],'models':[],'providers':[],
                    'model_options_truncated':False,'providers_truncated':False,'models_truncated':False,'favorites':[]}));sys.exit(0)
 starred=json.loads((root/'favorites').read_text()) if (root/'favorites').exists() else []
 short=catalog()['model_options']+[{'id':'folderonly/proj','value':'folderonly/proj','label':'Project model','provider':'folderonly',
        'provider_label':'Folder-only fixture','free':False,'effort_options':[],'effort_labels':[]}]
 full=short+[{'id':'solo/solo-model','value':'solo/solo-model','label':'Solo model','provider':'solo','provider_label':'Solo fixture',
        'free':False,'effort_options':['low'],'effort_labels':['low']}]
 for entry in short+full:
  if entry['value'] in starred:entry['favorite']=True
 print(json.dumps({'catalog_state':'ready','catalog_reason':'','location':a[a.index('--location')+1],'model_options':short,'models':full,
                   'providers':[{'id':'folderonly','label':'Folder-only fixture','kind':'config','credential_count':0,'model_count':1,'folder':True}],
                   'model_options_truncated':False,'providers_truncated':False,'models_truncated':False,'favorites':[{'value':v} for v in starred]}))
elif a[:2]==['account','favorite']:
 with open(root/'favorite-calls','a') as calls:calls.write(json.dumps([host]+a)+'\n')
 if (root/'favorite-fails').exists():sys.stderr.write('Account catalog is busy. Retry after the current operation finishes.\n');sys.exit(1)
 model=a[a.index('--model')+1];on=a[a.index('--favorite')+1]=='on'
 starred=json.loads((root/'favorites').read_text()) if (root/'favorites').exists() else []
 starred=[v for v in starred if v!=model]+([model] if on else [])
 (root/'favorites').write_text(json.dumps(starred))
 print(json.dumps({'status':'applied','request_id':a[a.index('--request-id')+1],'model':model,'favorite':on,'favorites':starred}))
```

3. Update the existing OpenCode tests: the model combo `launchModel` stays as the hidden selected value, so `findData`/`setCurrentIndex` calls keep working; replace `QVERIFY(models->isVisible())` / `QVERIFY(!models->isVisible())` with the same checks on `dialog.findChild<QPushButton *>("launchModelButton")`.
4. Add to the test class the includes `OpenCodeModelPicker.h`, `<QLineEdit>`, `<QTreeWidget>`, `<QUuid>` and this helper:

```cpp
    static QTreeWidgetItem *row(QTreeWidget *tree, const QString &value) {
        for (int g = 0; g < tree->topLevelItemCount(); ++g)
            for (int m = 0; m < tree->topLevelItem(g)->childCount(); ++m)
                if (tree->topLevelItem(g)->child(m)->data(0, Qt::UserRole).toString() == value) return tree->topLevelItem(g)->child(m);
        return nullptr;
    }
    QStringList lines(const QString &name) const {
        QFile f(m_fixture.filePath(name)); if (!f.open(QIODevice::ReadOnly)) return {};
        return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
    }
```

5. New tests (`launchError` is the dialog's existing error label):

```cpp
    void openCodePickerUsesTheSelectedFolderCatalog() {
        flag("opencode"); flag("opencode-models");
        NewSessionDialog dialog(program(), fleet(), "mac", "opencode"); dialog.selectPath("/workspace/example", false); dialog.show();
        auto *button = dialog.findChild<QPushButton *>("launchModelButton"); QTRY_VERIFY(button && button->isVisible());
        QCOMPARE(button->text(), QString("Native default model"));
        button->click();
        auto *picker = dialog.findChild<OpenCodeModelPicker *>("launchModelPicker"); QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        QTRY_VERIFY(picker->values().contains("folderonly/proj"));
        QCOMPARE(QJsonDocument::fromJson(lines("model-calls").value(0).toUtf8()).array(),
                 QJsonArray({"mac","account","models","native-opencode","--location","/workspace/example","--json"}));
        auto *tree = picker->findChild<QTreeWidget *>("openCodeModelList"); bool folder = false;
        for (int g = 0; g < tree->topLevelItemCount(); ++g) folder |= tree->topLevelItem(g)->text(0) == "Folder-only fixture · This folder";
        QVERIFY(folder);
        picker->select("folderonly/proj"); QTRY_VERIFY(!picker->isVisible()); QCOMPARE(button->text(), QString("Project model"));
        QVERIFY(!dialog.findChild<QComboBox *>("launchEffort")->isVisible());
        auto *start = dialog.findChild<QPushButton *>("primary"); QTRY_VERIFY(start->isEnabled());
        QSignalSpy launched(&dialog, &NewSessionDialog::launchRequested); start->click(); QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launched[0][8].toString(), QString("folderonly/proj")); QCOMPARE(launched[0][9].toString(), QString());
    }
    void openCodeChoicesOutsideTheRowSurviveAnAccountRefresh() {
        flag("opencode"); flag("opencode-models");
        NewSessionDialog dialog(program(), fleet(), {}, "opencode"); dialog.selectPath("/workspace/example", false); dialog.show();
        auto *button = dialog.findChild<QPushButton *>("launchModelButton"); QTRY_VERIFY(button->isVisible()); button->click();
        auto *picker = dialog.findChild<OpenCodeModelPicker *>("launchModelPicker"); QTRY_VERIFY(picker->values().contains("folderonly/proj"));
        picker->select("folderonly/proj"); QTRY_COMPARE(button->text(), QString("Project model"));
        const int before = lines("model-calls").size();
        dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTest::qWait(300);
        QCOMPARE(button->text(), QString("Project model"));
        QCOMPARE(dialog.findChild<QComboBox *>("launchModel")->currentData().toString(), QString("folderonly/proj"));
        button->click(); QTRY_VERIFY(picker->values().contains("folderonly/proj")); QCOMPARE(lines("model-calls").size(), before);
        // An All models choice is kept the same way, with its efforts.
        picker->findChild<QPushButton *>("openCodeAllModels")->click(); picker->select("solo/solo-model");
        auto *effort = dialog.findChild<QComboBox *>("launchEffort"); QTRY_VERIFY(effort->isVisible()); effort->setCurrentIndex(effort->findData("low"));
        dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click(); QTest::qWait(300);
        QCOMPARE(button->text(), QString("Solo model")); QCOMPARE(effort->currentData().toString(), QString("low"));
        auto *start = dialog.findChild<QPushButton *>("primary"); QTRY_VERIFY(start->isEnabled());
        QSignalSpy launched(&dialog, &NewSessionDialog::launchRequested); start->click(); QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launched[0][8].toString(), QString("solo/solo-model")); QCOMPARE(launched[0][9].toString(), QString("low"));
    }
    void openCodeAllModelsSearchesTheFolderCatalog() {
        flag("opencode"); flag("opencode-models");
        NewSessionDialog dialog(program(), fleet(), {}, "opencode"); dialog.selectPath("/workspace/example", false); dialog.show();
        dialog.findChild<QPushButton *>("launchModelButton")->click();
        auto *picker = dialog.findChild<OpenCodeModelPicker *>("launchModelPicker"); QTRY_VERIFY(picker->values().contains("folderonly/proj"));
        picker->findChild<QPushButton *>("openCodeAllModels")->click();
        picker->findChild<QLineEdit *>("openCodeModelSearch")->setText("solo");
        QTRY_COMPARE(picker->values(), QStringList({"solo/solo-model"}));
        QCOMPARE(lines("model-calls").size(), 1);   // the folder catalog is fetched once
        picker->select("solo/solo-model"); auto *effort = dialog.findChild<QComboBox *>("launchEffort");
        QVERIFY(effort->isVisible()); QCOMPARE(effort->count(), 2);
    }
    void openCodeStarSendsOneFavoriteRequestWithIdentity() {
        flag("opencode"); flag("opencode-models");
        NewSessionDialog dialog(program(), fleet(), {}, "opencode"); dialog.selectPath("/workspace/example", false); dialog.show();
        dialog.findChild<QPushButton *>("launchModelButton")->click();
        auto *picker = dialog.findChild<OpenCodeModelPicker *>("launchModelPicker"); QTRY_VERIFY(picker->values().contains("fixture/plain"));
        emit picker->favoriteRequested("fixture/plain", true);
        QTRY_COMPARE(lines("favorite-calls").size(), 1);
        const auto args = QJsonDocument::fromJson(lines("favorite-calls")[0].toUtf8()).array();
        QCOMPARE(args.toVariantList().mid(0, 8), QVariantList({"","account","favorite","native-opencode","--model","fixture/plain","--favorite","on"}));
        QCOMPARE(args[8].toString(), QString("--request-id")); QVERIFY(!QUuid::fromString(args[9].toString()).isNull());
        QCOMPARE(args[10].toString(), QString("--json"));
        // The folder catalog is fetched again, and it marks the favorite.
        QTRY_COMPARE(lines("model-calls").size(), 2);
        auto *tree = picker->findChild<QTreeWidget *>("openCodeModelList"); QTRY_COMPARE(row(tree, "fixture/plain")->text(1), QString("★"));
        QVERIFY(picker->values().contains("folderonly/proj"));
        flag("favorite-fails");
        emit picker->favoriteRequested("fixture/other", true);
        auto *error = dialog.findChild<QLabel *>("launchError");
        QTRY_VERIFY(error->text().startsWith("Could not change the favorite on pc:"));
        QCOMPARE(row(tree, "fixture/other")->text(1), QString("☆"));
    }
    void openCodeStoppedServiceKeepsTheRowListAndNeverStarts() {
        flag("opencode"); flag("opencode-models"); flag("models-stopped");
        NewSessionDialog dialog(program(), fleet(), {}, "opencode"); dialog.selectPath("/workspace/example", false); dialog.show();
        dialog.findChild<QPushButton *>("launchModelButton")->click();
        auto *picker = dialog.findChild<OpenCodeModelPicker *>("launchModelPicker");
        QTRY_COMPARE(picker->findChild<QLabel *>("openCodeModelNotice")->text(), QString("OpenCode is not running on pc"));
        QVERIFY(picker->values().contains("fixture/local"));
        QVERIFY(!QFile::exists(m_fixture.filePath("service-started")));
    }
    void openCodeOlderComputerOffersNoStarsOrAllModels() {
        flag("opencode"); flag("opencode-models"); flag("old-hgs");
        NewSessionDialog dialog(program(), fleet(), "mac", "opencode"); dialog.selectPath("/workspace/example", false); dialog.show();
        dialog.findChild<QPushButton *>("launchModelButton")->click();
        auto *picker = dialog.findChild<OpenCodeModelPicker *>("launchModelPicker"); QTRY_VERIFY(picker->values().contains("fixture/local"));
        QTest::qWait(200);
        QVERIFY(lines("model-calls").isEmpty());   // never asks an older hgs
        auto *all = picker->findChild<QPushButton *>("openCodeAllModels"); QVERIFY(!all->isEnabled());
        QCOMPARE(all->toolTip(), QString("Update Zerus on mac to search all models and star favorites."));
        QSignalSpy favorites(picker, &OpenCodeModelPicker::favoriteRequested);
        auto *tree = picker->findChild<QTreeWidget *>("openCodeModelList");
        emit tree->itemClicked(row(tree, "fixture/plain"), 1); QCOMPARE(favorites.size(), 0);
        QCOMPARE(row(tree, "fixture/plain")->toolTip(1), QString("Update Zerus on mac to search all models and star favorites."));
        picker->select("fixture/local");
        auto *start = dialog.findChild<QPushButton *>("primary"); QTRY_VERIFY(start->isEnabled());
    }
```

- [ ] **Step 2: Run them to verify they fail**

Run: `nice -n 10 bash scripts/ci/gui.sh build && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_newsessiondialog`
Expected: the new tests fail (`launchModelButton`, `launchModelPicker` not found).

- [ ] **Step 3: Implement**

HgsClient: `requestOpenCodeModels` builds `[@host] account models native-opencode --location <dir> --json`, runs `runHgs` with 30000 ms, rejects output over 5 MiB with "The OpenCode model catalog is too large", parses an object and emits ready/failed with the location. `requestOpenCodeFavorite` builds the favorite arguments with a fresh UUID, 25000 ms, and emits ready/failed.

NewSessionDialog state (all keyed by the selected computer):
- `QHash<QString, QJsonObject> m_folderCatalogs` keyed by `host + '\n' + folder` (ready catalogs only), `quint64 m_folderRequest` with its key, and `QJsonObject m_chosenModel` with `QString m_chosenHost` — the picker entry (`value`, `label`, `effort_options`, `effort_labels`) of a choice made in the picker.

Layout: add `QPushButton *m_modelButton` (`launchModelButton`) in the Model row; hide `m_model` but keep it as the selected value (index 0 = native default). Add `QFrame *m_modelPopup` (`launchModelPopup`, `Qt::Popup`) holding `OpenCodeModelPicker *m_modelPicker` (`launchModelPicker`) and a `QPushButton` "Use native default model" (`launchModelDefault`).

`updateModels()` (runs on every account load and selection change):

Context change first (picker contract, Task 7 header): whenever the selected computer, folder or session differs from the context last shown, call `m_modelPicker->setCatalog({})` BEFORE any step below, even when `m_folderCatalogs` already has a catalog for the new context; only then set its short list, catalog and current model. The dialog keeps its favorite request identities and ignores late answers for the old context.
1. Fill `m_model` from the account row exactly as today.
2. If `m_folderCatalogs` has a catalog for the current host and folder, append combo items for its `model_options` values that are missing (label, `EffortRole`, `EffortLabelRole`), and call `m_modelPicker->setShortList(catalog["model_options"], catalog["model_options_truncated"].toBool())` and `setCatalog(catalog)`; otherwise `setShortList(row["model_options"], row["model_options_truncated"].toBool())`.
3. If `m_chosenHost == host()` and `m_chosenModel` is not in the combo, append it as an item with its efforts.
4. Restore the previous selection with `findData(selected)` (as today); `setCurrent` on the picker; set the button text to the current item's label (or "Native default model").
5. `bool supported = OpenCodeModels::catalogSupported(row)`: `setAllModelsAvailable(supported, OpenCodeModels::updateText(machine))`; favorites available only when `supported` and the computer is online (`OpenCodeModels::updateText(machine)` or "%1 is offline." as the reason).

Opening the popup (`m_modelButton` clicked): position it under the button and show it. When the row is supported, a folder is selected and no ready catalog is cached for `host()+'\n'+folder` (and no request for that key runs), call `requestOpenCodeModels(host(), folder)`. On ready for the same key: cache it only when its `catalog_state` is `ready`, then `updateModels()`; any other state calls only `m_modelPicker->setCatalog(catalog)` so the picker shows the reason while the account row's list stays. On failure: `setCatalogFailed(OpenCodeModels::errorText(error, machine))`. `catalogRequested` (All models) uses the cached catalog or the same request.

`chosen(value)`: store `m_chosenModel` from `m_modelPicker->model(value)` and `m_chosenHost = host()`; ensure a combo item exists; select it; update the button text; hide the popup; `updateEfforts()`. "Use native default model" clears `m_chosenModel` and selects index 0. Changing the computer clears `m_chosenModel` (its value belongs to that computer); changing the folder keeps it only when the new folder's ready catalog lists it.

`favoriteRequested(value, on)`: `setFavoritePending(value, true)`; `requestOpenCodeFavorite(host(), value, on)`; on ready `setFavorite(value, on)`, `setFavoritePending(value, false)`, request the folder catalog again for the current key (replacing the cached one when it arrives) and `loadAccounts(true)`; on failure `setFavoritePending(value, false)` and set `launchError` to "Could not change the favorite on %1: %2" (computer name, `OpenCodeModels::errorText(error, machine)`). Never call `startOpenCode()` from picker paths.

- [ ] **Step 4: Run tests to verify they pass**

Run: `nice -n 10 bash scripts/ci/gui.sh build && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_newsessiondialog`
Expected: PASS, existing tests included.

- [ ] **Step 5: Commit**

```bash
git add tray/src/HgsClient.h tray/src/HgsClient.cpp tray/src/NewSessionDialog.h tray/src/NewSessionDialog.cpp tray/tests/test_newsessiondialog.cpp
git commit -m "Choose OpenCode models in New session from the folder's grouped catalog"
```

---

### Task 9: Desktop — the model change picker in the composer

**Files:**
- Modify: `tray/src/MessageComposer.h`, `tray/src/MessageComposer.cpp`, `tray/src/SessionsWindow.h`, `tray/src/SessionsWindow.cpp`
- Test: `tray/tests/test_messagecomposer.cpp`, `tray/tests/test_sessionswindow.cpp`, `tray/tests/CMakeLists.txt` (add the `ZERUS_OPENCODE_PROVIDER_CATALOG` definition to `test_messagecomposer`)

**Interfaces:**
- Consumes: `OpenCodeModelPicker` (Task 7); `HgsClient::requestOpenCodeModels`, `requestOpenCodeFavorite` (Task 8b); `OpenCodeModels::catalogSupported`, `updateText`, `errorText` (Task 7).
- Produces (MessageComposer):

```cpp
    // OpenCode only. `supported`: the session's hgs has `account models` and `account favorite`
    // (its details report `model_options_truncated`); `online`: its computer answers.
    void setOpenCodeContext(bool supported, bool truncated, bool online, const QString &computer);
    void setOpenCodeCatalog(const QString &sessionKey, const QJsonObject &catalog);
    void setOpenCodeCatalogFailed(const QString &sessionKey, const QString &reason);
    void setOpenCodeFavorite(const QString &sessionKey, const QString &value, bool favorite, const QString &error);
signals:
    void openCodeCatalogRequested(const QString &sessionKey);
    void openCodeFavoriteRequested(const QString &sessionKey, const QString &value, bool favorite);
```

The OpenCode settings popup shows `OpenCodeModelPicker` (`sessionOpenCodeModels`) and keeps the `sessionModelChoice` combo hidden; the combo stays the selected value, so the existing effort and Apply logic is unchanged. Other agents keep the combo. Entries chosen in the picker are kept per session key and merged into the model options on every `setModelSettings` (each details refresh calls it).

- [ ] **Step 1: Write the failing tests**

`test_messagecomposer.cpp` (declare the slots in the class; include `OpenCodeModelPicker.h`; load the fixture through `ZERUS_OPENCODE_PROVIDER_CATALOG`):

```cpp
static QJsonArray sharedShortList()
{
    QFile file(QStringLiteral(ZERUS_OPENCODE_PROVIDER_CATALOG)); if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object()["row"].toObject()["model_options"].toArray();
}

static QJsonArray withSolo(QJsonArray models)
{
    models.append(QJsonObject{{"id","solo/solo-model"},{"value","solo/solo-model"},{"label","Solo model"},{"provider","solo"},
        {"provider_label","Solo fixture"},{"free",false},{"effort_options",QJsonArray{"low"}},{"effort_labels",QJsonArray{"Low"}}});
    return models;
}

void TestMessageComposer::openCodePickerChoosesFromTheShortListAndAllModels()
{
    const auto options = sharedShortList(); QCOMPARE(options.size(), 8);
    MessageComposer composer; composer.resize(640, 220); setSessionReady(composer, "opencode/session");
    composer.setModelSettings("fixture/local", {}, options, true, {}, {}, {}, "now", true);
    composer.setOpenCodeContext(true, false, true, "arch");
    QSignalSpy changes(&composer, &MessageComposer::settingsRequested), catalogs(&composer, &MessageComposer::openCodeCatalogRequested);
    composer.show(); QTest::qWait(20);
    composer.findChild<QPushButton *>("sessionModelSettings")->click();
    auto *picker = composer.findChild<OpenCodeModelPicker *>("sessionOpenCodeModels"); QVERIFY(picker && picker->isVisible());
    QVERIFY(!composer.findChild<QComboBox *>("sessionModelChoice")->isVisible());
    picker->select("keyed/paid-model");
    auto *apply = composer.findChild<QPushButton *>("applySessionSettings"); QVERIFY(apply->isEnabled()); apply->click();
    QCOMPARE(changes[0], QVariantList({"opencode/session", "keyed/paid-model", ""}));
    composer.findChild<QPushButton *>("sessionModelSettings")->click();
    picker->findChild<QPushButton *>("openCodeAllModels")->click();
    QCOMPARE(catalogs.size(), 1); QCOMPARE(catalogs[0][0].toString(), QString("opencode/session"));
    const QJsonObject catalog{{"catalog_state","ready"},{"models",withSolo(options)},{"model_options",options}};
    composer.setOpenCodeCatalog("other/session", catalog);
    QVERIFY(!picker->values().contains("solo/solo-model"));
    composer.setOpenCodeCatalog("opencode/session", catalog);
    picker->select("solo/solo-model");
    auto *efforts = composer.findChild<QComboBox *>("sessionEffortChoice");
    QStringList shown; for (int i = 0; i < efforts->count(); ++i) shown << efforts->itemText(i);
    QCOMPARE(shown, QStringList({"Default", "Low"}));
    efforts->setCurrentIndex(1); apply->click();
    QCOMPARE(changes[1], QVariantList({"opencode/session", "solo/solo-model", "low"}));
}

void TestMessageComposer::openCodeAllModelsChoiceSurvivesADetailsRefresh()
{
    const auto options = sharedShortList();
    MessageComposer composer; composer.resize(640, 220); setSessionReady(composer, "opencode/session");
    composer.setModelSettings("fixture/local", {}, options, true, {}, {}, {}, "now", true);
    composer.setOpenCodeContext(true, false, true, "arch");
    composer.show(); composer.findChild<QPushButton *>("sessionModelSettings")->click();
    auto *picker = composer.findChild<OpenCodeModelPicker *>("sessionOpenCodeModels");
    picker->findChild<QPushButton *>("openCodeAllModels")->click();
    composer.setOpenCodeCatalog("opencode/session", {{"catalog_state","ready"},{"models",withSolo(options)},{"model_options",options}});
    picker->select("solo/solo-model");
    // SessionsWindow calls setModelSettings on every details refresh with the session's own list.
    composer.setModelSettings("fixture/local", {}, options, true, {}, {}, {}, "now", true);
    auto *apply = composer.findChild<QPushButton *>("applySessionSettings");
    QCOMPARE(composer.findChild<QComboBox *>("sessionModelChoice")->currentData().toString(), QString("solo/solo-model"));
    QVERIFY(apply->isEnabled());
    QVERIFY(!composer.findChild<QComboBox *>("sessionModelChoice")->isVisible());
    QSignalSpy changes(&composer, &MessageComposer::settingsRequested); apply->click();
    QCOMPARE(changes[0], QVariantList({"opencode/session", "solo/solo-model", ""}));
}

void TestMessageComposer::openCodeStarRequestsAFavoriteForThisSession()
{
    const auto options = sharedShortList();
    MessageComposer composer; setSessionReady(composer, "opencode/session");
    composer.setModelSettings("fixture/local", {}, options, true, {}, {}, {}, "now", true);
    composer.setOpenCodeContext(true, false, true, "arch");
    QSignalSpy favorites(&composer, &MessageComposer::openCodeFavoriteRequested);
    composer.show(); composer.findChild<QPushButton *>("sessionModelSettings")->click();
    auto *picker = composer.findChild<OpenCodeModelPicker *>("sessionOpenCodeModels");
    emit picker->favoriteRequested("fixture/alternate", true);
    QCOMPARE(favorites[0], QVariantList({"opencode/session", "fixture/alternate", true}));
    composer.setOpenCodeFavorite("other/session", "fixture/alternate", true, {});
    QVERIFY(!picker->model("fixture/alternate")->favorite);
    composer.setOpenCodeFavorite("opencode/session", "fixture/alternate", true, {});
    QVERIFY(picker->model("fixture/alternate")->favorite);
    composer.setOpenCodeContext(true, false, false, "arch");
    emit picker->favoriteRequested("fixture/local", true); QCOMPARE(favorites.size(), 1);
}

void TestMessageComposer::openCodeOnAnOlderComputerOffersNoStarsOrAllModels()
{
    MessageComposer composer; setSessionReady(composer, "opencode/session");
    composer.setModelSettings("fixture/local", {}, sharedShortList(), true, {}, {}, {}, "now", true);
    composer.setOpenCodeContext(false, false, true, "mac");
    QSignalSpy catalogs(&composer, &MessageComposer::openCodeCatalogRequested), favorites(&composer, &MessageComposer::openCodeFavoriteRequested);
    composer.show(); composer.findChild<QPushButton *>("sessionModelSettings")->click();
    auto *picker = composer.findChild<OpenCodeModelPicker *>("sessionOpenCodeModels");
    auto *all = picker->findChild<QPushButton *>("openCodeAllModels"); QVERIFY(!all->isEnabled());
    QCOMPARE(all->toolTip(), QString("Update Zerus on mac to search all models and star favorites."));
    all->click(); emit picker->favoriteRequested("fixture/local", true);
    QCOMPARE(catalogs.size(), 0); QCOMPARE(favorites.size(), 0);
}

void TestMessageComposer::otherAgentsKeepThePlainModelList()
{
    MessageComposer composer; setSessionReady(composer, "claude/session");
    composer.setModelSettings("opus", {}, QJsonArray{QJsonObject{{"id","opus"},{"label","Opus"}}}, true, {}, {}, {}, "now", false);
    composer.show(); composer.findChild<QPushButton *>("sessionModelSettings")->click();
    QVERIFY(composer.findChild<QComboBox *>("sessionModelChoice")->isVisible());
    QVERIFY(!composer.findChild<OpenCodeModelPicker *>("sessionOpenCodeModels")->isVisible());
}
```

`test_sessionswindow.cpp`: in the fake `hgs` add an `account models` branch that records its arguments to a `model-calls` file and prints `{"catalog_state":"ready","model_options":[],"models":[{"id":"solo/solo-model","value":"solo/solo-model","label":"Solo model","provider":"solo","provider_label":"Solo fixture","free":false}],"favorites":[]}`; make the OpenCode session's details include `"model_options_truncated": false`. Add a test that selects that OpenCode session (follow the file's existing OpenCode session fixtures), emits `MessageComposer::openCodeCatalogRequested` for it, and asserts the recorded call is `[host, "account","models","native-opencode","--location",<the session's cwd>,"--json"]` and that `sessionOpenCodeModels` then lists `solo/solo-model` with All models on. Remove `"model_options_truncated"` from those details and assert All models is disabled with `OpenCodeModels::updateText(<machine>)` as its tooltip.

- [ ] **Step 2: Run them to verify they fail**

Run: `nice -n 10 bash scripts/ci/gui.sh build`
Expected: compile errors (`setOpenCodeContext`, `openCodeCatalogRequested`, … not declared).

- [ ] **Step 3: Implement**

MessageComposer:
- Create the picker inside `m_settingsPopup` above the effort row; keep `QHash<QString, QJsonArray> m_openCodeChosen` (picker-chosen entries per session key).
- `setModelSettings`: when `modelDefaultEffort` (OpenCode) is true, append to `options` every entry of `m_openCodeChosen[sessionKey]` whose value it lacks, store the result in `m_modelOptions`, then `m_openCodeModels->setShortList(options without the appended entries, m_openCodeTruncated)` and `setCurrent(model)`. Other agents use the combo as today.
- `openSettings()` also calls `m_openCodeModels->setCurrent(pending model or model)`.
- `updateSettingsApply()`: keep `m_models` hidden for OpenCode (`setVisible(!native && !m_modelDefaultEffort)` for that widget only; the effort widgets keep `!native`), and show the picker only for OpenCode.
- `chosen(value)`: build `{"id","value","label","effort_options","effort_labels"}` from `m_openCodeModels->model(value)`; add it to `m_openCodeChosen[m_popupKey]` and to `m_modelOptions` when absent (so the `offered` check in `updateSettingsApply` and `updateSettingsEfforts` find it); ensure an `m_models` item; select it; `updateSettingsEfforts()`.
- `catalogRequested` → `emit openCodeCatalogRequested(m_popupKey)`; `favoriteRequested` → when favorites are available, `setFavoritePending(value, true)` and `emit openCodeFavoriteRequested(m_popupKey, value, on)`.
- `setOpenCodeContext(supported, truncated, online, computer)`: remembers `truncated`; `setAllModelsAvailable(supported, OpenCodeModels::updateText(computer))`; `setFavoritesAvailable(supported && online, supported ? tr("%1 is offline.").arg(computer) : OpenCodeModels::updateText(computer))`.
- Context change first (picker contract, Task 7 header): every session, computer or folder change begins with `m_modelPicker->setCatalog({})`, even when the new session already has a catalog, before its short list, catalog and current model are set. The composer keeps its favorite request identities and ignores late answers for the old context.
- The setters ignore a session key other than the current one.
- Widen the popup for OpenCode to `qMin(420, available.width() - 24)` and cap the tree's height at 320 px.

SessionsWindow:
- Where `setModelSettings` is called, also call `setOpenCodeContext(OpenCodeModels::catalogSupported(m_details), m_details.value("model_options_truncated").toBool(), entry->online, entry->machine)`.
- Connect `openCodeCatalogRequested` to `m_client.requestOpenCodeModels(entry->host, m_details.value("cwd").toString(entry->session.cwd))`; remember `request → sessionKey`; route `openCodeModelsReady/Failed` to `setOpenCodeCatalog/setOpenCodeCatalogFailed`.
- Connect `openCodeFavoriteRequested` to `requestOpenCodeFavorite(entry->host, …)`; on ready `setOpenCodeFavorite(key, value, favorite, {})` and refresh the selected session's details through the existing details refresh; on failure `setOpenCodeFavorite(key, value, !favorite, OpenCodeModels::errorText(error, entry->machine))` (the composer shows it in the popup hint). Catalog failures go through `errorText` the same way.

- [ ] **Step 4: Run tests to verify they pass**

Run: `nice -n 10 bash scripts/ci/gui.sh build && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_messagecomposer && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_sessionswindow`
Expected: PASS, including `openCodeModelSettingsOfferModelDefaultEffort` and `openCodeEffortIdsFollowSharedRule` unchanged.

- [ ] **Step 5: Commit**

```bash
git add tray/src/MessageComposer.h tray/src/MessageComposer.cpp tray/src/SessionsWindow.h tray/src/SessionsWindow.cpp \
  tray/tests/test_messagecomposer.cpp tray/tests/test_sessionswindow.cpp tray/tests/CMakeLists.txt
git commit -m "Change OpenCode models from a grouped picker with All models and favorites"
```

---

### Task 10: Android — providers in Accounts and the grouped short-list pickers

**Files:**
- Create: `mobile/android/app/src/main/java/app/zerus/mobile/OpenCodeCatalog.kt`, `mobile/android/app/src/main/java/app/zerus/mobile/OpenCodeModelListUi.kt`
- Modify: `LaunchPresentation.kt`, `LaunchUi.kt`, `SessionDetailsPresentation.kt`, `NativeSettingsUi.kt`, `AccountSnapshots.kt`, `AccountsUi.kt` (same folder)
- Test: create `mobile/android/app/src/test/java/app/zerus/mobile/OpenCodeCatalogTest.kt`; extend `LaunchPresentationTest.kt`, `AccountSnapshotsTest.kt`, `OpenCodePresentationTest.kt`
- Do not touch: `ZerusViewModel.observeScheduledSettings`, `ZerusViewModel.settleSessionAction`, `QuestionUi.kt`, `QuestionPolicies.kt`.

**Interfaces:**
- Consumes: the Task 1 fixture (Android copy), `NativeEfforts` (existing).
- Produces:

```kotlin
data class OpenCodeModel(val value:String,val label:String,val provider:String,val providerLabel:String,
    val free:Boolean,val favorite:Boolean,val isDefault:Boolean,val efforts:List<NativeEffort>)
data class OpenCodeProvider(val id:String,val label:String,val kind:String,val credentials:Int?,val activeLabel:String,val models:Int?)
data class OpenCodeGroup(val provider:String,val label:String,val models:List<OpenCodeModel>)
data class OpenCodeAccountCatalog(val state:String,val reason:String,val catalogAt:Double?,val providers:List<OpenCodeProvider>,
    val providersTruncated:Boolean,val modelsTruncated:Boolean)
object OpenCodeCatalog {
    const val MAX_MODELS=256; const val MAX_PROVIDERS=64; const val PERSISTED_PROVIDERS=16
    fun models(raw:JSONArray?):List<OpenCodeModel>
    fun providers(raw:JSONArray?,limit:Int=MAX_PROVIDERS):List<OpenCodeProvider>
    fun account(raw:JSONObject):OpenCodeAccountCatalog?          // null unless provider == "opencode"
    fun group(models:List<OpenCodeModel>):List<OpenCodeGroup>
    fun filter(models:List<OpenCodeModel>,query:String):List<OpenCodeModel>
    fun marks(model:OpenCodeModel):List<String>                   // "Default", "Favorite", "Free"
    fun kindLabel(kind:String):String
    fun stateMessage(state:String,reason:String):String           // "" when ready
    fun truncationMessage(models:Boolean,providers:Boolean):String
}
```

- `LaunchModel` gains `provider`, `providerLabel`, `free`, `favorite`, `isDefault` (defaults keep call sites); `LaunchPresentation.models()` fills them through `OpenCodeCatalog.models`; `sanitizeCatalog` keeps them (`favorite`/`default` only when true) and, for OpenCode accounts, `model_options_truncated`, `catalog_state`, `catalog_reason`; add `LaunchPresentation.catalogNotice(raw,provider,accountId):String` = the non-blank parts of `stateMessage(state,reason)` and `truncationMessage(modelsTruncated,false)` joined by one space (launch catalogs carry no providers).
- `NativeModelChoice` gains `provider`, `providerLabel`, `free`, `favorite`, `isDefault` with defaults; `NativeSettings` gains `modelsTruncated:Boolean=false`; `signature` includes `modelsTruncated` and each model's `provider`, `free`, `favorite` and `isDefault`, so a change of marks alone re-renders the sheet.
- `ReportedAccount` gains `val openCode:OpenCodeAccountCatalog?=null` (last parameter); `AccountSnapshots.account` parses it; `accountJson` persists `catalog_state`, `catalog_reason`, `catalog_at`, `model_options_truncated`, the first 16 providers and `providers_truncated` (true when more were reported).

Texts (identical on iOS): kinds as on the desktop ("Free, no key", "API key", "Signed in", "Environment variable", "Configuration", "Command", "External credentials", "Detected locally", other "Connected"); models truncated "Showing a shortened list. Choose other models on the desktop."; providers truncated "Some providers are not shown."; states: `loading` "OpenCode is still loading providers.", other non-ready states use `catalog_reason` when non-blank, else `not_running` "OpenCode is not running on this computer.", `disabled` "The OpenCode service is disabled on this computer.", `incompatible` "This OpenCode version is not supported.", `unreachable` "OpenCode did not answer.".

- [ ] **Step 1: Write the failing tests**

`OpenCodeCatalogTest.kt`:

```kotlin
package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

/** OpenCode providers phase 1, shared with hgs, the connector, the desktop and iOS (opencode-provider-catalog.json). */
class OpenCodeCatalogTest {
    private val contract = JSONObject(checkNotNull(javaClass.getResourceAsStream("/opencode-provider-catalog.json")).bufferedReader().use { it.readText() })

    @Test fun providerCatalogFixtureMatchesTheSharedCopy() {
        val shared = generateSequence(java.io.File("").absoluteFile) { it.parentFile }
            .map { java.io.File(it,"tests/fixtures/opencode-provider-catalog.json") }.firstOrNull { it.isFile }
        org.junit.Assume.assumeTrue("The shared fixture is outside this checkout",shared != null)
        assertArrayEquals(shared!!.readBytes(),checkNotNull(javaClass.getResourceAsStream("/opencode-provider-catalog.json")).use { it.readBytes() })
    }

    @Test fun heartbeatAccountParsesProvidersAndState() {
        val catalog = checkNotNull(AccountSnapshots.account(contract.getJSONObject("heartbeat_account"))?.openCode)
        assertEquals(listOf("free","env","config","local","key","oauth"),catalog.providers.map { it.kind })
        assertEquals(2,catalog.providers.single { it.id=="keyed" }.credentials)
        assertEquals("Work",catalog.providers.last().activeLabel)
        assertEquals(1,catalog.providers.single { it.id=="solo" }.models)
        assertEquals("ready",catalog.state); assertFalse(catalog.providersTruncated); assertFalse(catalog.modelsTruncated)
        assertEquals(1792267200.5,catalog.catalogAt!!,0.0)
        assertNull(AccountSnapshots.account(JSONObject().put("id","work").put("provider","codex"))!!.openCode)
    }

    @Test fun launchShortListKeepsOrderMarksAndGroups() {
        val models = OpenCodeCatalog.models(contract.getJSONObject("launch_account").getJSONArray("model_options"))
        assertEquals(listOf("fixture/local","keyed/paid-model","groq/openai/gpt-oss-20b","fixture/alternate","ollama/qwen3-coder:30b",
            "opencode/glm-5.3-flash-free","opencode/big-pickle","groq/groq/compound"),models.map { it.value })
        assertTrue(models[0].isDefault)
        assertEquals(listOf("keyed/paid-model","groq/openai/gpt-oss-20b"),models.filter { it.favorite }.map { it.value })
        val groups = OpenCodeCatalog.group(models)
        assertEquals(listOf("Private keyless fixture","Keyed fixture","Groq","Ollama","OpenCode"),groups.map { it.label })
        assertEquals(listOf("groq/openai/gpt-oss-20b","groq/groq/compound"),groups[2].models.map { it.value })
        assertEquals(listOf("Default"),OpenCodeCatalog.marks(models[0]))
        assertEquals(listOf("Free"),OpenCodeCatalog.marks(models[5]))
        assertEquals(listOf("low","high","max"),models[5].efforts.map { it.value })
    }

    @Test fun searchMatchesEveryWordAcrossLabelValueAndProvider() {
        val models = OpenCodeCatalog.models(contract.getJSONObject("launch_account").getJSONArray("model_options"))
        assertEquals(listOf("groq/openai/gpt-oss-20b"),OpenCodeCatalog.filter(models,"gpt 20B").map { it.value })
        assertEquals(listOf("fixture/alternate"),OpenCodeCatalog.filter(models,"keyless alternate").map { it.value })
        assertEquals(8,OpenCodeCatalog.filter(models,"  ").size)
    }

    @Test fun stoppedAccountExplainsStateAndTruncation() {
        val catalog = checkNotNull(AccountSnapshots.account(contract.getJSONObject("stopped_account"))?.openCode)
        assertEquals("OpenCode is not running on fixture",OpenCodeCatalog.stateMessage(catalog.state,catalog.reason))
        assertEquals("OpenCode is not running on this computer.",OpenCodeCatalog.stateMessage("not_running",""))
        assertEquals("",OpenCodeCatalog.stateMessage("ready",""))
        assertEquals("Showing a shortened list. Choose other models on the desktop. Some providers are not shown.",
            OpenCodeCatalog.truncationMessage(catalog.modelsTruncated,catalog.providersTruncated))
    }

    @Test fun hostileEntriesAreDroppedOrBounded() {
        val providers = JSONArray().put(JSONObject().put("id","bad id").put("kind","key")).put("text")
            .put(JSONObject().put("id","ok").put("label","\u001b").put("kind","magic").put("credential_count",-1).put("model_count",20000))
        val parsed = OpenCodeCatalog.providers(providers)
        assertEquals(1,parsed.size); assertEquals("ok",parsed[0].label); assertEquals("",parsed[0].kind)
        assertNull(parsed[0].credentials); assertNull(parsed[0].models)
        assertEquals("Connected",OpenCodeCatalog.kindLabel(parsed[0].kind))
        val models = JSONArray().put(JSONObject().put("value","noslash")).put(JSONObject().put("value","p/"+"m".repeat(200)))
            .put(JSONObject().put("value","p/ok").put("favorite","yes"))
        assertEquals(listOf("p/ok"),OpenCodeCatalog.models(models).map { it.value })
        assertFalse(OpenCodeCatalog.models(models).single().favorite)
    }

    @Test fun persistedAccountsKeepAtMostSixteenProviders() {
        val raw = JSONObject(contract.getJSONObject("heartbeat_account").toString())
        raw.put("providers",JSONArray((0 until 64).map { JSONObject().put("id","p$it").put("label","P$it").put("kind","env").put("credential_count",0).put("model_count",1) }))
        val account = checkNotNull(AccountSnapshots.account(raw))
        assertEquals(64,account.openCode!!.providers.size)
        val restored = checkNotNull(AccountSnapshots.account(AccountSnapshots.accountJson(account)))
        assertEquals(16,restored.openCode!!.providers.size); assertTrue(restored.openCode!!.providersTruncated)
    }

    @Test fun olderCachesWithoutOpenCodeFieldsStillDecode() {
        val old = JSONObject().put("id","native-opencode").put("provider","opencode").put("label","OpenCode").put("usage",JSONObject().put("status","unavailable"))
        val account = checkNotNull(AccountSnapshots.account(old))
        assertEquals(emptyList<OpenCodeProvider>(),account.openCode!!.providers)
        assertEquals("",account.openCode!!.state)
    }
}
```

`LaunchPresentationTest.kt`:

```kotlin
    @Test fun openCodeMarksSurviveTheSanitizedCatalog() {
        val contract = JSONObject(checkNotNull(javaClass.getResourceAsStream("/opencode-provider-catalog.json")).bufferedReader().use { it.readText() })
        val raw = JSONObject().put("agents",JSONArray(listOf("opencode"))).put("accounts",JSONArray().put(contract.getJSONObject("stopped_account")))
        val clean = LaunchPresentation.sanitizeCatalog(raw)
        val models = LaunchPresentation.models(clean,"opencode","native-opencode")
        assertEquals("fixture/local",models.single().value); assertTrue(models.single().isDefault)
        assertEquals("Private keyless fixture",models.single().providerLabel)
        assertEquals("OpenCode is not running on fixture Showing a shortened list. Choose other models on the desktop.",
            LaunchPresentation.catalogNotice(clean,"opencode","native-opencode"))
    }
```

`OpenCodePresentationTest.kt`:

```kotlin
    @Test fun sessionModelSheetReadsMarksAndTruncation() {
        val contract = JSONObject(checkNotNull(javaClass.getResourceAsStream("/opencode-provider-catalog.json")).bufferedReader().use { it.readText() })
        val raw = JSONObject().put("settings_model","fixture/local").put("settings_change_supported",true)
            .put("model_options",contract.getJSONObject("row").getJSONArray("model_options")).put("model_options_truncated",true)
        val settings = SessionDetailsPresentation.settings(raw)
        assertTrue(settings.modelsTruncated)
        assertEquals("Groq",settings.models.single { it.id=="groq/groq/compound" }.providerLabel)
        assertTrue(settings.models.single { it.id=="groq/groq/compound" }.free)
        assertTrue(settings.models.single { it.id=="keyed/paid-model" }.favorite)
    }
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cd mobile/android && nice -n 10 ./gradlew testDebugUnitTest --tests 'app.zerus.mobile.OpenCode*' --tests 'app.zerus.mobile.LaunchPresentationTest'` (set `JAVA_HOME` to the JDK 21 used by this lane if Gradle needs it)
Expected: compile errors (`OpenCodeCatalog`, `openCode`, `catalogNotice`, `modelsTruncated` unresolved).

- [ ] **Step 3: Implement**

`OpenCodeCatalog.kt`: model values follow `LaunchPresentation.validModel` (≤120 characters, contains `/`, no control characters), first occurrence, at most 256; labels ≤160 characters without control characters, else the value; `providerLabel` else `provider`; booleans only when JSON `true`; efforts through `NativeEfforts.parse(option)`. Providers: ID `[A-Za-z0-9_][A-Za-z0-9_.-]{0,79}`; label ≤160 without control characters else the ID; `kind` only from the eight kinds, else `""`; counts only within 0–64 and 0–10,000; active label ≤80. `account(raw)` returns `null` for other providers and an empty catalog (state `""`) when fields are missing. `group` keeps first-appearance order; `filter` splits on whitespace and matches every word case-insensitively in label, value or provider label.

`OpenCodeModelListUi.kt`: `@Composable fun OpenCodeModelList(models:List<OpenCodeModel>,selected:String,onSelect:(String)->Unit,modifier:Modifier=Modifier,nativeDefault:String?=null)` — a search field (`OutlinedTextField`, label "Search models") shown when there are more than 8 models, then a `LazyColumn` with a sticky provider header per group and rows (`selectable`, role RadioButton) showing the label, the marks as small `AssistChip`-style labels (a filled star icon for Favorite, read-only), and a check for the selected row; an optional first row for `nativeDefault` ("Native default model"). Content descriptions: "Model, <label>, <marks>".

`LaunchUi.kt`: in the OpenCode "Model" section replace the model `LaunchMenuField` with a field that opens a `ModalBottomSheet` containing `OpenCodeModelList(modelOptions…, nativeDefault = "Native default model")`; selecting closes the sheet and sets `selectedModel` (keep the effort field and the existing reset effects). Below it show `LaunchHint(LaunchPresentation.catalogNotice(...))` when non-blank, then the existing hint.

`NativeSettingsUi.kt`: when any model has a non-blank `provider`, render the model page with `OpenCodeModelList` (grouped, searchable) instead of the flat `itemsIndexed` list, and show `OpenCodeCatalog.truncationMessage(settings.modelsTruncated,false)` under it when truncated; the pinned Effort row and the effort page stay as they are.

`AccountsUi.kt`: in the account details of an `opencode` account add a "Providers" section: each row shows the label, `kindLabel(kind)` plus " · " active label or " · N accounts" when more than one, and "N models"; then the state message, "Last known catalog from <date>" when the state is not ready and `catalogAt` is known (use `AccountPresentation.date`), and the truncation message.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd mobile/android && nice -n 10 ./gradlew testDebugUnitTest lintDebug`
Expected: PASS; lint clean.

- [ ] **Step 5: Commit**

```bash
git add mobile/android/app/src/main/java/app/zerus/mobile/OpenCodeCatalog.kt mobile/android/app/src/main/java/app/zerus/mobile/OpenCodeModelListUi.kt \
  mobile/android/app/src/main/java/app/zerus/mobile/LaunchPresentation.kt mobile/android/app/src/main/java/app/zerus/mobile/LaunchUi.kt \
  mobile/android/app/src/main/java/app/zerus/mobile/SessionDetailsPresentation.kt mobile/android/app/src/main/java/app/zerus/mobile/NativeSettingsUi.kt \
  mobile/android/app/src/main/java/app/zerus/mobile/AccountSnapshots.kt mobile/android/app/src/main/java/app/zerus/mobile/AccountsUi.kt \
  mobile/android/app/src/test/java/app/zerus/mobile/
git commit -m "Show OpenCode providers and a grouped model short list on Android"
```

---

### Task 11: iOS — providers in Accounts and the grouped short-list pickers

**Files:**
- Create: `mobile/ios/ZerusKit/Sources/ZerusKit/OpenCodeCatalog.swift`, `mobile/ios/Zerus/Common/OpenCodeModelList.swift`
- Modify: `mobile/ios/ZerusKit/Sources/ZerusKit/Launch.swift`, `SessionDetails.swift`, `Accounts.swift`; `mobile/ios/Zerus/Launch/LaunchView.swift`, `mobile/ios/Zerus/Conversation/SessionSheets.swift`, `mobile/ios/Zerus/Accounts/AccountsScreen.swift`
- Test: create `mobile/ios/ZerusKit/Tests/ZerusKitTests/OpenCodeCatalogTests.swift`; extend `LaunchTests.swift`, `AccountsTests.swift`, `SessionControlsTests.swift`

**Interfaces:**
- Consumes: the Android fixture copy through `SharedFixture`; `EffortOption`.
- Rules (as on Android): model values ≤120 characters, containing `/`, no control characters, first occurrence, at most 256; labels ≤160 characters without control characters, else the value; `providerLabel` else `provider`; marks only from JSON `true`; efforts through `EffortOption.options`. Providers: ID `[A-Za-z0-9_][A-Za-z0-9_.-]{0,79}`; label ≤160 without control characters, else the ID; `kind` only one of the eight kinds, else `""`; `credential_count` 0–64 and `model_count` 0–10,000, else nil; active label ≤80. `account` is nil for other providers and an empty catalog (state `""`) when fields are missing.
- Texts (as on Android and the desktop): kinds "Free, no key", "API key", "Signed in", "Environment variable", "Configuration", "Command", "External credentials", "Detected locally", other "Connected"; "Showing a shortened list. Choose other models on the desktop."; "Some providers are not shown."; `loading` "OpenCode is still loading providers."; other non-ready states use `catalog_reason` when non-empty, else `not_running` "OpenCode is not running on this computer.", `disabled` "The OpenCode service is disabled on this computer.", `incompatible` "This OpenCode version is not supported.", `unreachable` "OpenCode did not answer."; `catalogNotice` joins the non-empty state message and the models truncation message with one space.
- Produces: the Android API in Swift:

```swift
public struct OpenCodeModel: Hashable, Sendable { public let value, label, provider, providerLabel: String; public let free, favorite, isDefault: Bool; public let efforts: [EffortOption] }
public struct OpenCodeProvider: Codable, Hashable, Sendable { public var id, label, kind: String; public var credentials: Int?; public var activeLabel: String; public var models: Int? }
public struct OpenCodeGroup: Hashable, Sendable { public let provider, label: String; public let models: [OpenCodeModel] }
public struct OpenCodeAccountCatalog: Codable, Hashable, Sendable { public var state, reason: String; public var catalogAt: Double?; public var providers: [OpenCodeProvider]; public var providersTruncated, modelsTruncated: Bool }
public enum OpenCodeCatalog {
    public static let maxModels = 256, maxProviders = 64
    public static func models(_ raw: [JSON]?) -> [OpenCodeModel]
    public static func providers(_ raw: [JSON]?, limit: Int = maxProviders) -> [OpenCodeProvider]
    public static func account(_ raw: JSONObject) -> OpenCodeAccountCatalog?
    public static func group(_ models: [OpenCodeModel]) -> [OpenCodeGroup]
    public static func filter(_ models: [OpenCodeModel], query: String) -> [OpenCodeModel]
    public static func marks(_ model: OpenCodeModel) -> [String]
    public static func kindLabel(_ kind: String) -> String
    public static func stateMessage(state: String, reason: String) -> String
    public static func truncationMessage(models: Bool, providers: Bool) -> String
}
```

- `ReportedAccount` gains `public var openCode: OpenCodeAccountCatalog? = nil` (optional, so persisted caches without it decode); `AccountSnapshots.account` parses it. iOS persists `[AccountCatalog]` through `Codable` in its sealed store, so the catalog is kept whole (at most 64 providers); the 16-provider cap is Android-only, where it protects the 1 MiB account cache. `accountJSON` (used only for launch-catalog usage) is unchanged.
- `LaunchModel` gains `provider`, `providerLabel`, `free`, `favorite`, `isDefault` (defaulted); `LaunchPresentation.models` fills them; `sanitizeCatalog` keeps them, `model_options_truncated`, `catalog_state` and `catalog_reason`; `LaunchPresentation.catalogNotice(_:provider:accountId:) -> String`.
- `NativeModelChoice` gains `provider`, `providerLabel`, `free`, `favorite`, `isDefault` (defaulted); `NativeSettings` gains `modelsTruncated`.

- [ ] **Step 1: Write the failing tests**

`OpenCodeCatalogTests.swift` mirrors Task 10's tests one for one (same names in Swift style, same expected values and texts):

```swift
import XCTest
@testable import ZerusKit

/// OpenCode providers phase 1 (opencode-provider-catalog.json), shared with hgs, the connector, the desktop and Android.
final class OpenCodeCatalogTests: XCTestCase {
    private func contract() throws -> JSONObject { try XCTUnwrap(SharedFixture.json("opencode-provider-catalog.json").object) }

    func testProviderCatalogFixtureIsSharedByteForByte() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../../../tests/fixtures/opencode-provider-catalog.json").standardized
        XCTAssertEqual(try Data(contentsOf: SharedFixture.url("opencode-provider-catalog.json")), try Data(contentsOf: root))
    }

    func testHeartbeatAccountParsesProvidersAndState() throws {
        let catalog = try XCTUnwrap(AccountSnapshots.account(try XCTUnwrap(contract().object("heartbeat_account")))?.openCode)
        XCTAssertEqual(catalog.providers.map(\.kind), ["free", "env", "config", "local", "key", "oauth"])
        XCTAssertEqual(catalog.providers.first { $0.id == "keyed" }?.credentials, 2)
        XCTAssertEqual(catalog.providers.last?.activeLabel, "Work")
        XCTAssertEqual(catalog.state, "ready"); XCTAssertEqual(catalog.catalogAt, 1792267200.5)
        XCTAssertFalse(catalog.providersTruncated); XCTAssertFalse(catalog.modelsTruncated)
    }

    func testLaunchShortListKeepsOrderMarksAndGroups() throws {
        let models = OpenCodeCatalog.models(try XCTUnwrap(contract().object("launch_account")?.array("model_options")))
        XCTAssertEqual(models.map(\.value), ["fixture/local", "keyed/paid-model", "groq/openai/gpt-oss-20b", "fixture/alternate",
                                             "ollama/qwen3-coder:30b", "opencode/glm-5.3-flash-free", "opencode/big-pickle", "groq/groq/compound"])
        XCTAssertTrue(models[0].isDefault)
        XCTAssertEqual(models.filter(\.favorite).map(\.value), ["keyed/paid-model", "groq/openai/gpt-oss-20b"])
        let groups = OpenCodeCatalog.group(models)
        XCTAssertEqual(groups.map(\.label), ["Private keyless fixture", "Keyed fixture", "Groq", "Ollama", "OpenCode"])
        XCTAssertEqual(groups[2].models.map(\.value), ["groq/openai/gpt-oss-20b", "groq/groq/compound"])
        XCTAssertEqual(OpenCodeCatalog.marks(models[0]), ["Default"]); XCTAssertEqual(OpenCodeCatalog.marks(models[5]), ["Free"])
    }

    func testSearchMatchesEveryWord() throws {
        let models = OpenCodeCatalog.models(try XCTUnwrap(contract().object("launch_account")?.array("model_options")))
        XCTAssertEqual(OpenCodeCatalog.filter(models, query: "gpt 20B").map(\.value), ["groq/openai/gpt-oss-20b"])
        XCTAssertEqual(OpenCodeCatalog.filter(models, query: "keyless alternate").map(\.value), ["fixture/alternate"])
        XCTAssertEqual(OpenCodeCatalog.filter(models, query: "  ").count, 8)
    }

    func testStoppedAccountExplainsStateAndTruncation() throws {
        let catalog = try XCTUnwrap(AccountSnapshots.account(try XCTUnwrap(contract().object("stopped_account")))?.openCode)
        XCTAssertEqual(OpenCodeCatalog.stateMessage(state: catalog.state, reason: catalog.reason), "OpenCode is not running on fixture")
        XCTAssertEqual(OpenCodeCatalog.stateMessage(state: "not_running", reason: ""), "OpenCode is not running on this computer.")
        XCTAssertEqual(OpenCodeCatalog.truncationMessage(models: true, providers: true),
                       "Showing a shortened list. Choose other models on the desktop. Some providers are not shown.")
    }

    func testProvidersSurviveTheCodableCacheAndOldCachesDecode() throws {
        var raw = try XCTUnwrap(contract().object("heartbeat_account"))
        raw["providers"] = .array((0..<64).map { .object(["id": .string("p\($0)"), "label": .string("P\($0)"), "kind": "env",
                                                          "credential_count": .int(0), "model_count": .int(1)]) })
        let account = try XCTUnwrap(AccountSnapshots.account(raw))
        XCTAssertEqual(account.openCode?.providers.count, 64)
        let restored = try JSONDecoder().decode(ReportedAccount.self, from: JSONEncoder().encode(account))
        XCTAssertEqual(restored.openCode, account.openCode)
        // A cache written before this field existed still decodes.
        var stored = try XCTUnwrap(JSONSerialization.jsonObject(with: JSONEncoder().encode(account)) as? [String: Any])
        stored.removeValue(forKey: "openCode")
        let legacy = try JSONSerialization.data(withJSONObject: stored)
        XCTAssertNil(try JSONDecoder().decode(ReportedAccount.self, from: legacy).openCode)
    }
}
```

`LaunchTests.swift`:

```swift
    func testOpenCodeMarksSurviveTheSanitizedCatalog() throws {
        let contract = try XCTUnwrap(SharedFixture.json("opencode-provider-catalog.json").object)
        let raw: JSONObject = ["agents": ["opencode"], "accounts": .array([.object(try XCTUnwrap(contract.object("stopped_account")))])]
        let clean = LaunchPresentation.sanitizeCatalog(raw)
        let models = LaunchPresentation.models(clean, provider: "opencode", accountId: "native-opencode")
        XCTAssertEqual(models.map(\.value), ["fixture/local"]); XCTAssertTrue(models[0].isDefault)
        XCTAssertEqual(models[0].providerLabel, "Private keyless fixture")
        XCTAssertEqual(LaunchPresentation.catalogNotice(clean, provider: "opencode", accountId: "native-opencode"),
                       "OpenCode is not running on fixture Showing a shortened list. Choose other models on the desktop.")
    }
```

`SessionControlsTests.swift`:

```swift
    func testSessionModelSheetReadsMarksAndTruncation() throws {
        let contract = try XCTUnwrap(SharedFixture.json("opencode-provider-catalog.json").object)
        var raw: JSONObject = ["settings_model": "fixture/local", "settings_change_supported": true, "model_options_truncated": true]
        raw["model_options"] = .array(try XCTUnwrap(contract.object("row")?.array("model_options")))
        let settings = SessionDetailsPresentation.settings(raw)
        XCTAssertTrue(settings.modelsTruncated)
        XCTAssertEqual(settings.models.first { $0.id == "groq/groq/compound" }?.providerLabel, "Groq")
        XCTAssertEqual(settings.models.first { $0.id == "groq/groq/compound" }?.free, true)
        XCTAssertEqual(settings.models.first { $0.id == "keyed/paid-model" }?.favorite, true)
    }
```

- [ ] **Step 2: Run them to verify they fail (on the Mac, in a fresh copy)**

Run:
```bash
dest=$(ssh -o BatchMode=yes mac mktemp -d)
rsync -a --exclude target --exclude .ci-build --exclude mobile/android/build ./ mac:"$dest"/
ssh -o BatchMode=yes mac "cd '$dest/mobile/ios/ZerusKit' && swift test --filter 'OpenCodeCatalogTests|LaunchTests|SessionControlsTests'"
```
Expected: compile errors (`OpenCodeCatalog`, `openCode` unresolved).

- [ ] **Step 3: Implement**

`OpenCodeCatalog.swift` with the Task 10 rules and texts. `OpenCodeModelList.swift` (SwiftUI): a `List` with one `Section` per group (header: provider label), rows showing the label, marks as small capsules (a `star.fill` image for Favorite, read-only), a checkmark when selected, an optional "Native default model" row first, and `.searchable(text:)` when there are more than 8 models; it returns the chosen value through a closure. `LaunchView`: the Model section becomes a `NavigationLink` ("Model", current label) to `OpenCodeModelList`; show `LaunchPresentation.catalogNotice` as a hint when non-empty. `SessionSheets` model section: when any model has a provider, use grouped sections, `.searchable` and the truncation text instead of the flat `ForEach`; keep the pinned Effort row. `AccountsScreen.AccountDetails`: a "Providers" section for `opencode` accounts with the same rows and notes as Android.

- [ ] **Step 4: Run tests to verify they pass, then remove the copy**

Run:
```bash
ssh -o BatchMode=yes mac "cd '$dest/mobile/ios/ZerusKit' && swift test && cd '$dest' && xcodebuild -project mobile/ios/Zerus.xcodeproj -scheme Zerus -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build -quiet"
ssh -o BatchMode=yes mac "rm -rf '$dest'"
```
Expected: all ZerusKit tests pass (0 failures); the app builds for the simulator.

- [ ] **Step 5: Commit**

```bash
git add mobile/ios/ZerusKit/Sources/ZerusKit/OpenCodeCatalog.swift mobile/ios/ZerusKit/Sources/ZerusKit/Launch.swift \
  mobile/ios/ZerusKit/Sources/ZerusKit/SessionDetails.swift mobile/ios/ZerusKit/Sources/ZerusKit/Accounts.swift \
  mobile/ios/Zerus/Common/OpenCodeModelList.swift mobile/ios/Zerus/Launch/LaunchView.swift mobile/ios/Zerus/Conversation/SessionSheets.swift \
  mobile/ios/Zerus/Accounts/AccountsScreen.swift mobile/ios/ZerusKit/Tests/ZerusKitTests/
git commit -m "Show OpenCode providers and a grouped model short list on iOS"
```

---

### Task 12: Native qualification against private OpenCode homes

Run this task right after Task 5: it needs only Tasks 2–5 and is the first test of the stopped-service cache (Task 4b) and of slash model IDs against a real OpenCode.

**Files:**
- Modify: `tests/test_opencode_native.py`, `docs/opencode.md` (Qualification section)

**Interfaces:**
- Consumes: `hgs account ls`, `account models`, `account favorite`, launch and inspect (Tasks 2–5).

- [ ] **Step 1: Extend the private setup and write the tests**

In `prepare()`:
- add `'vendor/slash': {'name': 'Slash fixture', 'limit': {'context': 32768, 'output': 2048}}` to the fixture provider's `models` (OpenCode 2.0.32 lists it with `id: "vendor/slash"`);
- add `'project'` to the created folders and write `root/'project/opencode.json'`:

```python
        folder = {'$schema': 'https://opencode.ai/config.json',
                  'provider': {'folderonly': {'npm': '@ai-sdk/openai-compatible', 'name': 'Folder-only fixture',
                                              'options': {'baseURL': f'http://127.0.0.1:{cls.model.server_port}/v1'},
                                              'models': {'proj': {'name': 'Project model', 'limit': {'context': 32768, 'output': 2048}}}}}}
        (root / 'project/opencode.json').write_text(json.dumps(folder))
        cls.project = root / 'project'
```

Add a helper beside `account()`:

```python
    def ready_account(self):
        """Start the private service explicitly and wait until its catalog is ready."""
        self.stop_service()
        self.hgs(['account', 'start', 'native-opencode'])
        until = time.monotonic() + 30
        account = self.account()
        while account['catalog_state'] == 'loading' and time.monotonic() < until:
            time.sleep(.5)
            account = self.account()
        self.assertEqual(account['catalog_state'], 'ready', account)
        return account

    def favorite(self, model, on, request=None, success=True):
        return self.hgs(['account', 'favorite', 'native-opencode', '--model', model, '--favorite', 'on' if on else 'off',
                         '--request-id', request or str(uuid.uuid4()), '--json'], success=success)
```

Tests (numbers from 20 so they never collide with in-flight lanes):

```python
    def test_20_catalog_short_list_providers_and_slash_ids(self):
        account = self.ready_account()
        options = [row['value'] for row in account['model_options']]
        self.assertEqual(options[0], 'fixture/local')
        self.assertTrue(account['model_options'][0]['default'])
        self.assertIn('fixture/vendor/slash', options)
        free = [row for row in account['model_options'] if row['provider'] == 'opencode']
        self.assertTrue(free and all(row['free'] for row in free), account['model_options'])
        kinds = {p['id']: p['kind'] for p in account['providers']}
        self.assertEqual((kinds['fixture'], kinds['opencode']), ('config', 'free'))
        self.assertLessEqual(len(json.dumps(account, ensure_ascii=True)), 32 * 1024)
        catalog = self.hgs(['account', 'models', 'native-opencode', '--json'])
        self.assertIn('fixture/vendor/slash', [m['value'] for m in catalog['models']])
        self.assertFalse(catalog['models_truncated'])
        self.assert_private(account, catalog)
        for text in (json.dumps(account), json.dumps(catalog)):
            self.assertNotIn(f'127.0.0.1:{self.model.server_port}', text)
        tag = uuid.uuid4().hex[:10]
        self.launch(tag, '--model', 'fixture/vendor/slash')
        inspected = self.hgs(['inspect', f'opencode/workspace/{tag}'])
        self.assertEqual(inspected['settings_model'], 'fixture/vendor/slash')
        # Session details pin the session's model, with the catalog's efforts.
        listed = {m['value']: m for m in catalog['models']}
        pinned = next(m for m in inspected['model_options'] if m['value'] == 'fixture/vendor/slash')
        self.assertEqual(pinned['effort_options'], listed['fixture/vendor/slash']['effort_options'])
        self.assertIn('model_options_truncated', inspected)
        # The connector gives `hgs account ls` 3 seconds.
        started = time.monotonic()
        self.account()
        self.assertLess(time.monotonic() - started, 3.0)

    def test_21_folder_specific_provider_only_in_its_folder(self):
        self.ready_account()
        home = self.hgs(['account', 'models', 'native-opencode', '--json'])
        self.assertNotIn('folderonly', [p['id'] for p in home['providers']])
        folder = self.hgs(['account', 'models', 'native-opencode', '--location', str(self.project), '--json'])
        provider = next(p for p in folder['providers'] if p['id'] == 'folderonly')
        self.assertEqual((provider['folder'], provider['kind']), (True, 'config'))
        self.assertIn('folderonly/proj', [m['value'] for m in folder['model_options']])
        refused = self.hgs(['account', 'models', 'native-opencode', '--location', str(self.root / 'missing'), '--json'], success=False)
        self.assertIn('Choose an existing folder on this computer', refused.stderr)
        tag = uuid.uuid4().hex[:10]
        self.hgs(['opencode', str(self.project), '--new', '-n', tag, '-d', '--launch-id', str(uuid.uuid4()),
                  '--account', 'native-opencode', '--model', 'folderonly/proj'])
        elsewhere = self.launch(uuid.uuid4().hex[:10], '--model', 'folderonly/proj', success=False)
        self.assertIn('not offered', elsewhere.stderr)

    def test_22_not_running_keeps_the_last_catalog_and_never_starts(self):
        (self.root / 'hgs-config/opencode-favorites.json').unlink(missing_ok=True)
        ready = self.ready_account()
        self.stop_service()
        self.assertFalse(self.native_processes())
        account = self.account()
        self.assertEqual(account['catalog_state'], 'not_running')
        self.assertEqual([r['value'] for r in account['model_options']], [r['value'] for r in ready['model_options']])
        self.assertEqual(account['providers'], ready['providers'])
        catalog = self.hgs(['account', 'models', 'native-opencode', '--json'])
        self.assertEqual((catalog['catalog_state'], catalog['models']), ('not_running', []))
        folder = self.hgs(['account', 'models', 'native-opencode', '--location', str(self.project), '--json'])
        self.assertEqual((folder['model_options'], folder['providers']), ([], []))
        self.favorite('fixture/alternate', True)
        self.assertTrue(self.account()['model_options'][1]['favorite'])
        self.assertFalse(self.native_processes(), 'an inspection or favorite started OpenCode')

    def test_23_favorites_reorder_the_short_list_with_request_identity(self):
        (self.root / 'hgs-config/opencode-favorites.json').unlink(missing_ok=True)
        account = self.ready_account()
        free = next(r['value'] for r in account['model_options'] if r['provider'] == 'opencode')
        first = str(uuid.uuid4())
        receipt = self.favorite(free, True, first)
        self.assertEqual((receipt['status'], receipt['favorite']), ('applied', True))
        options = self.account()['model_options']
        self.assertEqual((options[1]['value'], options[1]['favorite']), (free, True))
        self.favorite(free, False)
        replay = self.favorite(free, True, first)
        self.assertEqual(replay['status'], 'applied')
        self.assertNotIn(free, replay['favorites'])
        self.assertFalse(any(r.get('favorite') for r in self.account()['model_options']))
        self.favorite('gone/missing', True)
        catalog = self.hgs(['account', 'models', 'native-opencode', '--json'])
        self.assertIn({'value': 'gone/missing', 'available': False}, catalog['favorites'])
        self.assertNotIn('gone/missing', [r['value'] for r in self.account()['model_options']])
        self.assertTrue((self.root / 'hgs-config/opencode-favorites.json').is_file())
```

`test_22` stars `fixture/alternate`, which is configured and therefore already listed; it must move to position 1 with `favorite: true` while the service is stopped (rebuilt from the cached candidates). Favorites persist in the shared private config, so both tests start by removing the favorites file.

- [ ] **Step 2: Run the suite**

Run:
```bash
cargo +1.99.1 build --locked
ZERUS_TEST_OPENCODE_NATIVE=1 HGS_TEST_BIN="$PWD/target/debug/hgs" PYTHONPATH=tests python3 -m unittest test_opencode_native -v
pgrep -af zerus-opencode-native || echo clean
```
Expected: every test passes against the installed OpenCode 2.0.32, including 20–23; the last command prints `clean`. The private service may also list an `ollama` provider when a local Ollama answers; no assertion depends on it.

- [ ] **Step 3: Document**

`docs/opencode.md`, Qualification: the native suite covers the provider catalog (keyless free models, a configured model ID with `/`, a folder-only provider), a stopped service and favorites with request identities, all in private homes.

- [ ] **Step 4: Commit**

```bash
git add tests/test_opencode_native.py docs/opencode.md
git commit -m "Qualify OpenCode providers, folder catalogs and favorites natively"
```

---

### Task 13: Final verification and tracking

- [ ] **Step 1:** `cargo +1.99.1 fmt --check && cargo +1.99.1 clippy --locked --all-targets -- -D warnings && cargo +1.99.1 test --locked && RUSTUP_TOOLCHAIN=1.99.1 bash scripts/ci/check-windows-target.sh clippy`
- [ ] **Step 2:** `PYTHONPATH=tests python3 -m unittest discover -s tests` (the ordinary CLI suite, as `scripts/ci/cli.sh` runs it)
- [ ] **Step 3:** `cd services/mobile/connector && cargo +1.99.1 test --locked && cargo +1.99.1 clippy --locked --all-targets -- -D warnings`
- [ ] **Step 4:** `nice -n 10 bash scripts/ci/gui.sh build && nice -n 10 bash scripts/ci/gui.sh test`
- [ ] **Step 5:** `cd mobile/android && nice -n 10 ./gradlew testDebugUnitTest lintDebug`; iOS `swift test` and the simulator build on the Mac in a fresh copy (Task 11 commands), then remove the copy.
- [ ] **Step 6:** The Task 12 native suite; `pgrep -af zerus-opencode-native` prints nothing.
- [ ] **Step 7:** `python3 scripts/check-source-privacy.py` — no finding may name a file of this plan (CI runs the same guard through `scripts/ci/check-source.py`); resolve findings of another lane's untracked drafts with that lane before pushing.
- [ ] **Step 8:** Beads: add the evidence to `zerus-dg1g.1` (commits, test counts, native result), file follow-ups for anything Appendix B leaves open, `bd export -o .beads/issues.jsonl`, run the privacy check again (no absolute paths in Beads text), commit locally. Do not push or merge (lane constraints).
- [ ] **Step 9 (owner, after merge and install):** Manual check on a computer with the real OpenCode service: Accounts shows the providers; New session lists `opencode/glm-5.3-flash-free` under OpenCode with Free; starring a model on the desktop moves it up on a phone after the next account refresh.

## Wave 2 notes (wave 1 final review, 2026-10-18)

- A star can meet "Account catalog is busy. Retry after the current operation finishes.": account mutations such as `default` hold `accounts.lock` while they print the new listing, which reads the OpenCode catalog as `hgs account ls` does (up to three 10-second reads, a 1-second read and the 5-second `--version` run), and `account favorite` waits at most 5 seconds for the lock. The desktop (Tasks 8b, 9) either retries with the same request UUID, which G1 makes safe (a retried identity returns its first change and never re-applies an older state), or shows the text. No code change in wave 1.

## Seams for phases 2 and 3

- Provider rows are keyed by provider `id`; the integration ID is derived (`integrationID`, else `id`) inside `providers::project`. Phase 2 adds `integration` and the allowlisted `methods` to `hgs account models` providers only (desktop), never to the 32 KiB row.
- `command` and `external` kinds are reserved although 2.0.32 data yields only `key`/`oauth`/`external` credential methods.
- `active_label` is emitted only for exactly one credential connection (Appendix B, item 1). Phase 2's Switch account needs the active flag; decide there whether an explicit desktop action may read `/api/credential` with values dropped in memory.
- The favorites receipt pattern (pending → applied, no replay over a newer state, newest 256 receipts) is the model for phase 2 key changes, which also carry request identities. G1 memory is that receipt window: a retry arriving after 256 newer changes is treated as a new request. The plan accepts this for favorites; phase 2 key changes that reuse the pattern must decide whether the window suffices for them.
- The desktop Accounts providers list is a `QTreeWidget` with one row per provider; phase 2 adds per-row actions (Replace key, Add/Switch/Rename account, Remove) and Connect provider beside Refresh.
- `read_snapshot(…, with_providers)` is the re-read phase 2 uses after a key connect to confirm the provider appears; nothing in phase 1 writes to OpenCode.

## Appendix A: probe findings (OpenCode 2.0.32, 2026-10-18)

Isolated probes: private `HOME` and `XDG_*` under a temporary directory, a private service port (`opencode service set port`), `OPENCODE_DISABLE_MODELS_FETCH=true`, a loopback keyless fixture provider; the service was started with `opencode service start` and stopped with `opencode service stop` under that environment; no process carried the private root afterwards and the directory was removed. `/api/credential` was never called; the developer's own OpenCode data was not touched.

- `/api/provider` lists connected providers only: `opencode` (`integrationID: "opencode"`, keyless, `activation: "enabled"`), an environment-connected `groq` (`activation: "auto"`), configured `fixture` (no `integrationID`) and an auto-detected `ollama` (no `integrationID`, present because a local Ollama answered). Entries carry `settings.apiKey`/`baseURL`. The first call right after start returned an empty list. A folder `opencode.json` adds its provider only for that `location`; a missing folder answers 404 `LocationNotFoundError`.
- `/api/model` is the full catalog of connected providers (27 models, about 640 bytes each raw). Model IDs can contain `/` (Groq `openai/gpt-oss-20b`, `groq/compound`; a configured `vendor/slash` was listed as `id: "vendor/slash"`), and `POST /api/session` accepted `{"providerID":"fixture","id":"vendor/slash"}`. OpenCode's keyless models cost `[{input:0, output:0, cache}]`; configured and local models cost `[]`; variants are `{id, settings}`. `/api/model/default` returns a full `Model.Info`, including `settings`.
- `/api/integration` lists 234 integrations (34 KB) with methods `key`, `env`, `oauth`, `external`. Connections are `{type:"env", name}` or `{type:"credential", id, label, method, status?}`; `Connection.Status` can carry `message` and `url`. Configured providers get an integration keyed by the provider ID although `Provider.integrationID` is absent; the auto-detected Ollama has none.
- `POST /api/integration/{id}/connect/key` answers 204 with no body. After two keys the integration listed both connections newest first, with no active flag; neither key appeared in `/api/provider` settings.
- `tests/fixtures/opencode2/openapi-2.0.32.json` confirms the shapes: `Provider.Info` (`id`, `canonical`, `integrationID`, `name`, `activation`, `package`, `compatibility`, `settings`, `headers`, `body`), `Model.Info` (`id`, `modelID`, `providerID`, `name`, `capabilities`, `variants`, `time`, `cost[]`, `status`, `enabled`, `limit`, plus `settings`/`headers`/`body`), `Integration.Info` (`id`, `name`, `metadata`, `methods`, `connections`), and `location` as a `deepObject` query parameter on all four.

## Appendix B: decisions and review

### Controller rulings (2026-10-18, decided)

1. **Active credential label:** `active_label` only when a provider has exactly one credential; polling never calls `/api/credential`, which the spec forbids for polling. Cost: no label for providers with several credentials until phase 2.
2. **Where favorites live:** a separate `opencode-favorites.json` under the accounts lock, because `accounts.json` rejects unknown fields and a rolled-back hgs must keep reading it. Cost: one more private file.
3. **Model IDs with `/`:** fix the merged phase-1 drop of model IDs containing `/` (Groq `openai/…`) in Task 2 and report it to root as a finding. Cost: launch validation accepts more models.
4. **Configured models:** models of providers without `integrationID`. Cost: key-connected models reach the short list only as default, favorite or free.
5. **Phones and folders:** phones use the home short list for New session; folder-only providers launch from the desktop or CLI, because the connector validates launches against the account row. Documented limitation.
6. **Unavailable favorites:** stay stored; the desktop shows them with a star to remove them; never sent to phones, which would otherwise offer a model that cannot launch.
7. **Rows still over 32 KiB after the spec's drop order:** keep dropping providers, finally down to the default model alone, with the truncation flags — fail-safe under the connector's 128 KiB bound.

### Independent plan review (2026-10-18)

Every Critical, Important and Minor finding is accepted and addressed:

- C1 stopped-service cache: Task 4b (`cached_at` accepts version 2 records, `cache_record` also writes `model_options`, file round-trip test, version 2 case in `cache_is_ignored_for_another_location`); Task 12 runs right after Task 5.
- C2 privacy guard: synthetic `/home/example` paths in the generator, fixtures and tests; the trailer is referenced, not spelled out; Tasks 1 and 13 check the guard.
- I1 Windows clippy: `#[cfg(unix)]` on the new re-exports (and on `catalog::catalog` from Task 4b), `#[cfg_attr(not(unix), allow(dead_code))]` on `providers`, `favorites` and `shortlist`, and the Windows-target clippy step in Tasks 2–5 and 13.
- I2 session model pinned: `session_options(location, pinned)` and `shortlist::ordered` (Tasks 4a, 4b), the frozen contract, native `test_20`.
- I3 choices survive refreshes: Task 8b (kept folder catalogs and chosen entry, refetch after a star) and Task 9 (kept entries per session, combo stays hidden), with refresh tests.
- I4 older computers: `hgs --help` lists both commands (Tasks 3, 5); clients detect support by `model_options_truncated` and show "Update Zerus on %1 to search all models and star favorites." (Tasks 7, 8b, 9).
- M1 Task 8a uses the real AccountsPage test helpers; M2 `HashMap` import (Task 2); M3 cache candidates in short-list priority (Task 4b); M4 1 s `/api/integration` deadline (Task 2) and the `account ls` time check (Task 12); M5 the 16-provider persistence cap is Android-only (Tasks 10, 11); M6 2 KiB reserve under 4 MiB (Task 5); M7 search filters the unavailable group and a failure leaves All models (Task 7); M8 150 ms search debounce and at most 500 rendered matches (Task 7); M9 marks in `NativeSettings.signature` (Task 10); M10 Task 12 order, Task 4 split into 4a/4b and Task 8 into 8a/8b (letter suffixes keep the other numbers stable); M11 pruning never removes the receipt just written (Task 3); M12 `load_favorites` moves to Task 4a; M13 the 120-character phone limit is documented (Task 4a).

Rejected findings: none.
