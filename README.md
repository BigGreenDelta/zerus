<h1 align="center">
  <img src="docs/assets/zerus.svg" alt="" width="64" valign="middle" /> Zerus
</h1>

<p align="center">
  <strong>Your agents. Your machines. One workspace.</strong><br />
  One interface for native agent harnesses, with their tools, hooks and workflows.<br />
  Projects, session assignments and shared settings sync directly between peers.
</p>

<p align="center">
  <a href="docs/installation.md">Install</a> &nbsp; | &nbsp;
  <a href="#features">Features</a> &nbsp; | &nbsp;
  <a href="#your-fleet-from-your-phone">Android</a> &nbsp; | &nbsp;
  <a href="docs/reference.md">Reference</a><br />
  <sub>Linux and macOS &nbsp; | &nbsp; Android &nbsp; | &nbsp; Windows &nbsp; | &nbsp; P2P &nbsp; | &nbsp; MIT</sub>
</p>

<p align="center">
  <a href="https://code.claude.com/"><kbd><img src="https://www.google.com/s2/favicons?domain=claude.ai&amp;sz=64" alt="" width="16" valign="middle" /> Claude Code</kbd></a> &nbsp;
  <a href="https://github.com/openai/codex"><kbd><img src="https://www.google.com/s2/favicons?domain=openai.com&amp;sz=64" alt="" width="16" valign="middle" /> Codex</kbd></a> &nbsp;
  <a href="https://www.kimi.com/code/"><kbd><img src="https://www.google.com/s2/favicons?domain=kimi.com&amp;sz=64" alt="" width="16" valign="middle" /> Kimi Code</kbd></a> &nbsp;
  <a href="https://deepseek-harness.github.io/deepseek-harness/"><kbd><img src="https://www.google.com/s2/favicons?domain=deepseek.com&amp;sz=64" alt="" width="16" valign="middle" /> DeepSeek Harness</kbd></a> &nbsp;
  <a href="https://opencode.ai/"><kbd><img src="https://www.google.com/s2/favicons?domain=opencode.ai&amp;sz=64" alt="" width="16" valign="middle" /> OpenCode</kbd></a>
</p>

Zerus is an **Agent Development Environment (ADE)** where your machines are peers
in one shared workspace. A project can include folders and agent sessions on
several hosts. The project catalog, session assignments and shared recovery
settings sync directly between machines, with no permanent main machine.
Work from your laptop or workstation using the SSH access configured there.
Bring your own agents and subscriptions.

**Claude Code, Codex, Kimi Code, DeepSeek Harness and OpenCode** run in their own native
runtimes. Zerus brings their Activity, messages, questions, approvals and session
controls together, while their original tools, hooks, permissions and history
remain with the native harness. Agents from different providers can discover
sessions, read progress and exchange messages through the built-in `hgs` CLI,
locally or over SSH. Available controls depend on the agent and its version.
You can run **any terminal command** through `hgs`; full ADE features need an
adapter.

Missing your agent or a feature?
[Send a PR](https://github.com/ufna/zerus/pulls) or
[let us know](https://github.com/ufna/zerus/issues).

<p align="center">
  <img src="docs/assets/readme/workspace.webp" alt="Zerus workspace with multiple agents, Activity, subagents and tasks in one window" width="1100" />
</p>

## Features

<table>
  <tr>
    <td width="42%" valign="middle">
      <h3>Your whole fleet at a glance</h3>
      <p>See machines, running sessions and requests for attention in one overview. Search, filters and project groups help you work with dozens or hundreds of agents. Machine load and account limits are right alongside them.</p>
    </td>
    <td width="58%">
      <a href="docs/assets/readme/fleet.webp"><img src="docs/assets/readme/fleet.webp" alt="Overview of 112 synthetic sessions on four machines, with activity and resource use" width="640" /></a>
    </td>
  </tr>
  <tr>
    <td valign="middle">
      <h3>One shared workspace across your machines</h3>
      <p>A project brings together folders and agents on your laptop, workstation and servers. The project catalog, session assignments and shared recovery settings sync peer to peer over SSH. Each machine keeps a copy, with no permanent main machine. Switch to another connected computer and keep the same project organization. Offline peers catch up when they reconnect; SSH access, credentials and appearance stay local.</p>
    </td>
    <td>
      <a href="docs/assets/readme/projects.webp"><img src="docs/assets/readme/projects.webp" alt="The Orbit project connects folders on a workstation, a MacBook and two servers" width="640" /></a>
    </td>
  </tr>
  <tr>
    <td valign="middle">
      <h3>Notifications that lead to the right session</h3>
      <p>Know when an agent finishes, asks a question, requests approval or hits an error. A click takes you to that session's Activity, including remote sessions. Read replies, send messages and images, and follow subagents and tasks alongside the conversation.</p>
    </td>
    <td>
      <a href="docs/assets/readme/attention.webp"><img src="docs/assets/readme/attention.webp" alt="A remote DeepSeek Harness session waiting for a review-scope choice in Activity" width="640" /></a>
    </td>
  </tr>
</table>

### Native harnesses, working together

Use each provider's agent in its own environment, with its native tools and
workflows. Zerus preserves existing hooks while adding session tracking and
connects native events to the shared workspace. Supported integrations also expose
their original Terminal or Native UI for provider-specific features.

**Agent-to-agent communication is built in.** For example, let Codex implement a
change and Claude review it. Agents with shell access can use `hgs ls` to find
sessions, `hgs inspect` to read their progress and `hgs send` to exchange tasks
and results, including on another machine through `hgs @host`. Each recipient
continues in its own native conversation. The same inspection and message
interfaces serve the desktop and other clients.
[CLI protocol and delivery guarantees](docs/reference.md#json-and-external-clients).

**Local coding models** can run on your own computer through Ollama. Keep the
agent harness and inference provider separate: use the existing Codex adapter
with a local model, or the [OpenCode native adapter](docs/opencode.md).
[Setup and qualified local example](docs/local-models.md).

### Voice control across agents and devices

Speak a task to **Claude Code, Codex, Kimi Code, DeepSeek Harness or OpenCode** from the
desktop or Android app. Record, review and send: Zerus transcribes the recording
and delivers the text to the selected agent's native conversation, including on
another connected computer. Your message stays in Activity as a **playable voice
message with a waveform and expandable transcript**, so you can compare what you
said with what the agent received. Camera mode records video circles with the same
hold, cancel and lock controls on desktop and Android; the video stays in history
and its recognized speech goes to the agent.

Choose the processing computer and manage its recognition profiles in
**Settings → Voice input**. Use **local GigaAM v3 for Russian**, including GPU
inference, or an **OpenAI-compatible transcription server** at your own URL.
Your phone reaches the processing computer through the relay; it does not need
to run the model. Agent choice and speech provider choice are independent.

[Set up a speech provider](docs/speech-setup.md) |
[Voice messages and delivery](docs/voice-messages.md)

### Your fleet from your phone

The **Android app** brings your projects and agents to your phone:

- **Conversations and tasks.** Read replies, follow goals, task lists and subagents,
  answer questions and approvals, and send messages with files and images.
- **Voice control.** Send recorded tasks to any supported agent, play your voice
  messages back and view their transcripts. Choose recognition profiles on a
  connected computer, including local models and remote transcription servers.
- **Remote controls.** Create sessions with the computer's native agent and account;
  change models, clear or compact context, inspect processes and open the native
  terminal where supported.
- **Your workspace.** Filter sessions across machines, browse project folders and
  check account limits. Private drafts and cached conversations stay available offline.

Pair your phone with a computer to reach it and its directly connected Zerus peers
through a **self-hostable HTTPS relay**. Notifications use Firebase Cloud Messaging,
with a foreground live connection for phones without it. **iOS is planned.**

[Download the Android APK](https://zerus.dev/#download) |
[Connect your phone](docs/mobile-deployment.md#pair-the-phone) |
[Architecture and trust model](docs/mobile-architecture.md)

<table>
  <tr>
    <th width="33%">Your sessions</th>
    <th width="34%">Talk to your agents</th>
    <th width="33%">Accounts and limits</th>
  </tr>
  <tr>
    <td><a href="docs/assets/readme/android-sessions.webp"><img src="docs/assets/readme/android-sessions.webp" alt="Android session overview with agents on a workstation, MacBook and build server" width="280" /></a></td>
    <td><a href="docs/assets/readme/android-conversation.webp"><img src="docs/assets/readme/android-conversation.webp" alt="A conversation with Codex in the Android client" width="280" /></a></td>
    <td><a href="docs/assets/readme/android-accounts.webp"><img src="docs/assets/readme/android-accounts.webp" alt="Shared Codex, Claude and Kimi accounts with usage limits across machines" width="280" /></a></td>
  </tr>
</table>

### Persistent sessions, your choice of client

Terminal agents run in **tmux**. Closing Zerus, losing SSH or putting your laptop
to sleep leaves sessions on a running host intact. Attach from the built-in
Terminal, a regular terminal over SSH or another client, even one without Zerus.
Supported agents also have pause/resume with an exact conversation binding.

**DeepSeek Harness** uses the official native engine and an embedded
**Native UI** alongside Activity. Its host also keeps working after you close
the window. [Integration details](docs/reference.md#deepseek).

## Yet another ADE

> One project, agents on several machines. I wanted to work from the laptop or
> workstation with the same projects and session assignments available on both.
>
> That is the reason for the P2P workspace. Each machine keeps the shared catalog
> and exchanges changes directly with its peers, with no permanent main machine.
> The folders and agents stay on their hosts; the project organization and shared
> settings follow you between connected computers.
>
> Terminal agents run in ordinary tmux sessions. Work through Zerus or connect to
> the same session from any SSH client, using the SSH access configured on that
> computer.
>
> — [Vladimir / @ufna](https://github.com/ufna)

## Quick start

On Arch Linux x86_64, install the stable bundle with `yay -S zerus-ade-bin`, then
run `zerus-setup` as your normal user.
[Source and development packages](docs/installation.md#arch-linux) are also available.
On Apple Silicon macOS, install with `brew install --cask ufna/zerus/zerus`, then
run `zerus-setup`, or use the [signed DMG](docs/installation.md#macos-dmg).
Other Linux distributions [build from source](docs/installation.md#local-installation);
see [other platforms](#other-platforms).

[Install Zerus and the CLI](docs/installation.md), then use **New session** to
choose an agent, machine, project and folder. Ordinary folders without Git work
too. Add other machines through SSH in **Machines**.

The same sessions are available from your terminal:

```sh
hgs codex /workspace/orbit -n api       # local agent in tmux
hgs @build-01 claude /workspace/orbit   # agent on another machine
hgs ls                                # sessions here and on connected machines
hgs status                            # versions and running services here and there
hgs a codex/orbit/api                  # attach from any terminal
```

**On Android:** [download the APK](https://zerus.dev/#download),
then [pair your phone](docs/mobile-deployment.md#pair-the-phone).
**Try demo** lets you explore the interface before connecting a computer.

## Other platforms

Prebuilt Linux packages currently exist only for Arch Linux x86_64. Other Linux
distributions [build from source](docs/installation.md#local-installation) with
the same installer. Zerus is open source: apt, dnf and other packages are welcome
as pull requests.

**macOS**: Apple Silicon Macs running macOS 14 or later can install a signed and
notarized stable release from the [DMG](docs/installation.md#macos-dmg) or
[Homebrew tap](docs/installation.md#macos-homebrew):
`brew install --cask ufna/zerus/zerus`, then `zerus-setup`.
Daily Nightly and successful main CI Dev builds use `ufna/zerus/zerus@nightly`
and `ufna/zerus/zerus@dev`, respectively.
**Settings > Updates** checks versions and provides the update command or DMG.
Source builds remain available through the same installer.

**Windows**: nightly releases include a portable x86_64
[control console](docs/installation.md#windows) that manages agents on your
Linux and macOS machines over SSH. With local sessions enabled on its Machines
page, it also runs agents on the Windows computer itself, in a native session
host on ConPTY without WSL. Stable releases after 0.42.2 include it too; Dev
builds have none. Contributions that follow the plan are welcome.

[Windows support plan](docs/superpowers/specs/2026-10-10-windows-support-design.md)

[Command and settings reference](docs/reference.md) | [CI and Arch packages](docs/ci-and-aur.md) | [MIT](LICENSE)
