# OpenCode in Zerus

[Reference](reference.md) | [Local models](local-models.md)

OpenCode owns its tools, permissions and conversations. Zerus connects to its
native API and presents that conversation in desktop Activity and on Android.
The model provider is configured on the computer running OpenCode; the client
and mobile relay do not run inference or receive provider credentials.

## Reviewed native version

The initial adapter is pinned to OpenCode **1.18.35**. Check `opencode --version`
on the agent computer. An incompatible installed or resident version is rejected;
Zerus does not upgrade or stop an existing native host automatically.

The installed version exposes a native session API with durable prompt admission
and history sequence identities. Zerus uses those identities for status and
message delivery rather than reading the TUI or treating model generation as an
agent turn. Public upstream API documentation can describe a different route
revision; the adapter's native tests qualify the exact pinned version.

## Configure a local model

Install Ollama and load a model that supports tools on the same computer.
`ollama list` shows the existing models. The local example in this repository
uses `qwen3-coder:30b`; embedding models are not coding agents.

Merge this provider into the host's existing OpenCode configuration, normally
`~/.config/opencode/opencode.json` or `opencode.jsonc`. Retain other providers,
credentials and permission rules. A configured `OPENCODE_CONFIG_DIR` or
`OPENCODE_CONFIG` changes the native CLI's configuration location:

The initial adapter accepts file configuration. Nonempty
`OPENCODE_CONFIG_CONTENT` is rejected before creating a host or session because
inline settings are not part of its configuration identity. The mobile connector
clears inherited OpenCode configuration selectors and uses the host's normal
configuration files.

```json
{
  "$schema": "https://opencode.ai/config.json",
  "provider": {
    "ollama": {
      "npm": "@ai-sdk/openai-compatible",
      "name": "Ollama (local)",
      "options": { "baseURL": "http://127.0.0.1:11434/v1" },
      "models": {
        "qwen3-coder:30b": {
          "name": "Qwen3-Coder 30B",
          "tool_call": true,
          "temperature": true,
          "limit": { "context": 32768, "output": 2048 }
        }
      }
    }
  }
}
```

This adds an explicitly selectable `ollama/qwen3-coder:30b`; it does not change
the native default model. `temperature: true` declares model capability, not a
numeric temperature. Choose a model and context that fit the available GPU
memory while other local services remain running. See the
[OpenCode provider documentation](https://opencode.ai/docs/providers/#ollama).

On the development host, the installed Qwen model performed native `write`,
`read` and `bash` calls and produced the expected file. Other attempts returned
literal tool markup as ordinary text instead of calling tools, including a fresh
repeat with the ordinary configuration. Zerus preserves those replies as text;
it never executes tool-like prose. This model's tool behavior with the reviewed
OpenCode version is variable. The [Codex + Ollama example](local-models.md) has
separate successful edit, test and exact-resume evidence. A `useResponses` probe
still called Chat Completions and is not a demonstrated fix.

## Start a conversation

In New session, select the host computer, **OpenCode**, its native account and
an offered model. A blank model selection uses the host's native default.
Inspecting that form creates neither a worktree nor a native session.
The cold catalog includes models declared in the host's configuration files;
it does not fetch a cloud catalog or verify provider authentication and quota.

From the CLI:

```sh
hgs opencode /workspace/orbit --new -n local-review --account native-opencode --model ollama/qwen3-coder:30b
hgs inspect opencode/orbit/local-review
hgs @build-01 opencode /workspace/orbit --new -n local-review --model ollama/qwen3-coder:30b
```

The project directory remains on the agent host. Desktop and Android use the
same HGS run and native conversation identity. Only explicit Start session
creates a native conversation.

## Controls and limits

The initial adapter supports history, status, text messages, model selection,
Stop, native questions and permissions, pause/resume, rename and forgetting a
saved binding. Voice recognition delivers its transcript through the same guarded
text-message path; original audio remains in Zerus history.

The initial API-host path has no tmux Terminal or embedded Native UI. Attachments,
child-session controls, archive/fork, context reset and compaction are not yet
supported. Clients keep those controls unavailable with a reason. Configure
native permissions and provider credentials on the host; additional managed
OpenCode accounts are outside this initial native-account contract. Accounts can
rename the native configuration, remove it from a machine's Zerus catalog and add
it back; its files, credentials and sessions on the host stay unchanged.

Pause keeps the native host and exact native conversation available; it does not
stop other conversations. Use each OpenCode session's own Pause/Resume controls;
terminal-session bulk `hgs pause --all` / `hgs resume --all` do not include this
native API backend. Forget removes only the Zerus binding and preserves
native history. Interrupted or failed delivery is never automatically resubmitted.
If a native acknowledgement is lost, Zerus reconciles the exact message identity
when possible and otherwise retains an uncertain result for explicit inspection.

The initial history scan is bounded; a conversation beyond its event/byte budget
reports unavailable history rather than claiming that all messages were loaded.

The native server listens only on authenticated loopback. Its endpoint metadata
and access secret are private host state, not relay data or public catalog fields.
Closing or updating Zerus does not dispose that native host.

## Qualification

The ordinary CLI suite covers parsing and projections. The native adapter suite
runs against the actual reviewed OpenCode binary with private synthetic state
and a keyless local model fixture; it does not contact paid model APIs. Run the
suite explicitly with the prerequisites installed:

```sh
cargo build --locked
ZERUS_TEST_OPENCODE_NATIVE=1 HGS_TEST_BIN="$PWD/target/debug/hgs" \
  PYTHONPATH=tests python3 -m unittest test_opencode_native -v
```

See [contributing](../CONTRIBUTING.md) for the common CLI, Qt and Android
gates. Native prerequisites must be present for an explicitly requested native
qualification; a skipped suite is not compatibility evidence.
