# Local models and OpenCode integration

Owner request: 2026-10-13. Tracking: zerus-k9ie.

## Purpose

Support local coding models without conflating the model server with the agent
that owns tools, permissions and durable conversations. First qualify the existing
Codex adapter with Ollama and the model already installed on the development host.
Add OpenCode as a separate native agent adapter, usable through desktop and Android.

## Architecture

The client selects a computer, native agent and model. The host runs the agent,
which calls its configured inference provider. Ollama is an inference provider;
it does not replace the native agent lifecycle. The mobile relay carries the
existing API v1 projections and requests; it does not run model inference.

The Codex example uses its existing OSS provider selection and preserves those
launch options on exact resume. Keep the existing cloud defaults unchanged.
Document installation and host configuration with synthetic paths and commands.

OpenCode uses a dedicated native API adapter. Its private host is independent of
the GUI lifetime. Bindings pin the project, native session, HGS run and native
configuration; catalog/inspection reads never create sessions. Native IDs and
API receipts determine delivery and lifecycle state. Do not infer readiness from
a completed model generation or a loaded Ollama model.

## First supported contract

- Provider identity: `opencode`; initial account identity: `native-opencode`.
- Explicit model selection uses the native `provider/model` identifier.
- New session, recorded history, model discovery, guarded delivery and authoritative
  status use the installed and qualified native API version.
- Stop, permission/question answers and exact pause/resume retain native identity.
- Request identities are durable. An uncertain native request is inspected by its
  exact native message identity and is never automatically submitted again.
- Existing mobile API v1 consumers receive ordinary session/history/question
  projections and confirmed capability fields.
- Terminal and embedded Native UI are unavailable for the initial API-host path.
  Clear/compaction remain unavailable until separately implemented and qualified.
- Managed additional OpenCode accounts are outside this initial native-account
  contract; inference provider configuration remains native and host-owned.

## Boundaries

Use authenticated loopback and private host metadata. Do not publish passwords,
provider credentials, raw local configuration or transcripts in catalogs, fixtures
or source. Unknown/incompatible native hosts stay running and fail with an
explicit reason. Do not replace or restart live agents, tmux servers, speech
services or existing model servers during qualification.

Keep current Claude, Codex, Kimi and DeepSeek behavior and API request guards.
Do not change the optional desktop browser plugin or release policy. Final main
integration/publication remains coordinated with its existing owner.

## Qualification

Qualify the actual installed OpenCode version with private synthetic directories
and a keyless local model fixture. Check authoritative working/idle/error states,
permission/question answers, history, interrupt, exact resume, identity rejection
and uncertain delivery reconciliation. A requested native suite must fail when
its native prerequisites are missing rather than silently skip.

Use the existing local Ollama model for an actual Codex tool/edit/resume example;
record the exact versions, model digest and observed context/resource limitations
in a private receipt. No paid model requests or live owner session input.

Run affected Rust/CLI, desktop and Android gates on the final combined source.
Repository runbooks describe only behavior supported by those final checks.
