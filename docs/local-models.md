# Local models with Ollama

The agent and the inference provider are separate choices. Codex owns tools,
permissions, conversation history and agent state; Ollama runs the selected model.
Zerus connects to Codex through its existing adapter. The model runs on the computer
hosting that session, including when you control it from another computer or Android.

## Check the inference host

Run these commands on the computer that will host the agent:

```sh
codex --version
ollama --version
ollama list
ollama ps
```

Use an installed model that supports tools. Embedding models such as `nomic-embed-text`
and `bge-m3` do not run a coding agent. The example below uses `qwen3-coder:30b` and
Ollama's local endpoint, `http://127.0.0.1:11434`.

Current Codex uses the Responses protocol, served by Ollama at `/v1/responses`.
Chat Completions compatibility alone is insufficient. See the official
[Codex local-provider configuration](https://learn.chatgpt.com/docs/config-file/config-advanced#oss-mode-local-providers)
and [Ollama Codex integration](https://docs.ollama.com/integrations/codex).

## Create a separate local Codex profile

A dedicated Zerus account keeps the local model settings and native history separate
from your cloud Codex account. It does not require an OpenAI login. Create it once on
the inference host:

```sh
hgs account add local-ollama --provider codex --label "Local Qwen"
```

The command reports the profile's `home`. Append the following settings to its
`config.toml` (normally `~/.config/hgs/accounts/local-ollama/config.toml`). Keep the
`cli_auth_credentials_store = "file"` entry created by Zerus:

```toml
model_provider = "ollama"
oss_provider = "ollama"
model = "qwen3-coder:30b"
model_context_window = 32768
model_auto_compact_token_limit = 24576
web_search = "disabled"
approval_policy = "on-request"
sandbox_mode = "workspace-write"
```

Setting `model_provider` explicitly matters: a fresh Codex 0.162.1 profile with only
`--oss` can still show OpenAI login onboarding. Do not define
`[model_providers.ollama]`; Codex reserves that built-in provider ID. Its built-in
provider uses the local Ollama Responses endpoint. A different HTTP inference
endpoint requires a separately named custom Codex provider with `wire_api = "responses"`,
instead of overriding `ollama`.

Ollama recommends at least 64k context for Codex. The 32k setting above is a bounded
starting point for the 30B model on a 24 GB GPU shared with speech inference. Confirm
the actual allocation with `ollama ps`; the model's advertised maximum context is
not the loaded context. Larger contexts need more memory and may offload to the CPU.
Changing Codex's context setting is not a reason to restart the Ollama service.
See [Ollama context length](https://docs.ollama.com/context-length).

## Start and continue a session

Choose an existing project directory, then launch on its owning computer:

```sh
hgs codex /path/to/project --account local-ollama --new -n local-qwen --bare -- \
  --oss --local-provider ollama -m qwen3-coder:30b
```

`--bare` selects Codex directly instead of a project's `run_codex.sh` or
`run_codex.ps1` ([project wrappers](reference.md#project-wrappers)). The ordinary
Codex alternate screen is supported; keep its default for this example. On first
launch, review the folder-access and hook-trust prompts. Zerus tracking hooks must
be trusted for native state and conversation identity to appear.

The session appears as **Codex**, with **qwen3-coder:30b** as its model. Desktop and
Android can use the same session through their configured connection to this host;
they do not need a local copy of the model or direct access to Ollama's port.

Get the exact session name from `hgs ls`, rather than guessing its folder component.
For example, a project named `project` and the tag above produce
`codex/project/local-qwen`:

```sh
hgs inspect codex/project/local-qwen
hgs pause codex/project/local-qwen
hgs resume codex/project/local-qwen -d
hgs a codex/project/local-qwen
```

Pause is available when the agent is idle. Resume preserves the exact native
conversation, account home, `--oss`, local provider and model; it does not replay
the original prompt. Check that `conversation_id` before and after resume is equal.
Use `--fresh` only when you intentionally want a new conversation.

## Try a real coding task

In a new empty directory, create `calculator.py`:

```python
def total(values):
    return 0
```

And `test_calculator.py`:

```python
import unittest
from calculator import total

class TotalTest(unittest.TestCase):
    def test_total(self):
        self.assertEqual(total([2, 3, 7]), 12)

    def test_empty(self):
        self.assertEqual(total([]), 0)
```

Start the session in that directory and send:

> Run `python3 -m unittest -v`, fix `calculator.py` so `total` returns the sum,
> and run the tests again. Use the shell to edit the file. Do not install anything.
> Remember the marker `orange-otter-47` for the next turn.

After it finishes, pause and resume the session. Then send:

> State the marker from my previous request without reading files for it. Add an
> assertion that `total([-5, 2]) == -3`, actually edit the test file using the shell,
> and run `python3 -m unittest -v` again.

Verify the files and test output, as well as the final reply. This exercises tool
execution, edits, native state transitions and conversation continuity.

## Qualification and limits

On 2026-10-13, Codex 0.162.1 with Ollama 0.40.1 and the existing
`qwen3-coder:30b` Q4_K_M model passed this synthetic workflow on a 24 GB RTX 3090 Ti.
The run used private account/configuration/state, an isolated project and tmux
socket, with no cloud credentials. The speech service remained running.

The loaded context was 32,768 tokens. Ollama reported about 19.9 GB of GPU allocation;
total GPU use, including existing workloads, was about 23,089 MiB (22.6 GiB). One cold task took
about 30 seconds, and a short resumed task about 3 seconds. These are individual
fixture observations, not throughput or long-context benchmarks.

The initial model emitted `apply_patch` as a function call, which Codex rejected.
It recovered with a shell edit and the tests passed. This does not qualify every
Codex tool: Ollama/model support for custom/freeform tools can differ from native
OpenAI models. A stray `<tool_call>` marker also appeared in that first final reply.
The reproducible prompt above asks for shell edits. Review actual tool outcomes and
changed files; do not treat prose claiming success as evidence.

Zerus's empty-composer guard recognizes local-model footers using the exact confirmed
session model, or the explicitly saved launch model before the first native turn.
It retains the native cursor, placeholder, draft and terminal-ownership checks.
Unknown layouts remain blocked and can be completed in Terminal.

No full repository task, maximum-context run, simultaneous speech recognition, remote
client UI or other local model was qualified by this small example. Keep inference
host memory headroom and choose model/context limits for your actual workload.
