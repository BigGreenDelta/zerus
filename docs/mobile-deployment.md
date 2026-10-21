# Android and mobile relay setup

The first Android client operates existing Zerus sessions through a self-hostable
HTTPS relay. Read the [architecture and trust model](mobile-architecture.md)
before choosing a server. Source components are under `mobile/android/` and
`services/mobile/`. The repository's MIT license applies except for the
attributed Apache-2.0 MuxPod terminal adaptations bundled with Android.

## Build and install Android

Use JDK 17 or 21 and an Android SDK with platform 36 and build tools. The checked-in
Gradle wrapper verifies its distribution checksum.

```sh
cd mobile/android
export ANDROID_HOME="$HOME/Android/Sdk"
./gradlew testDebugUnitTest lintDebug assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The debug APK is for local development and testing. It is not a signed store release.
Keep a production signing key outside the repository and retain it for upgrades.
For a non-debuggable build with release performance, run
`./gradlew assembleRelease lintRelease` and install
`app/build/outputs/apk/release/app-release.apk`. This variant
uses the same development signing key as published APKs; it is not a store release.
The application ID is `app.zerus.mobile`; the minimum Android version is 8.0
(API 26). The default build requires no Firebase account or private credentials.
See [Android build details](../mobile/android/README.md) for optional FCM setup.

## Windows gateway

Use a portable bundle that includes `zerus-mobile-connector.exe` beside
`hgs.exe` and `hgs-tray.exe`. In Machines, enable local sessions if needed.
In Settings → Mobile connection, enter an HTTPS relay and the **computer**
credential issued by that relay, save and choose **Start connector**. Refresh
Phones below and pair the phone with the generated QR invitation. Windows is
its own outbound gateway; a Linux/macOS computer is not required.

Configuration lives at `%USERPROFILE%\.config\hgs\mobile\connector.json`
(or the explicit `HGS_CONFIG_DIR`), with private receipt directories beside it.
The connector checks actual ownership/DACLs, reparse points and file links.
It refuses unsafe existing entries without changing their ACLs. A changed relay
or credential gets a new receipt directory; old receipts remain untouched.
Keep credentials and `--check` reports private.

Start is explicit after each Windows login. Closing the GUI leaves the connector
running. **Stop connector** stops claiming, completes its in-flight native call
and journals the outcome; reload status to confirm it has stopped before saving
changes. It never kills the independent ConPTY session host or native agents.
There is no automatic login task or Windows service registration in this slice.

Supported scope is existing local Codex, Claude and Kimi sessions and their
native inspection, history, messages, questions, Terminal and allowed session
controls. Mobile session/folder/worktree creation, Git/file operations, one-hop
peers, DeepSeek/OpenCode and speech/voice are unavailable. Capability publication
and request dispatch both enforce this restriction; existing durable UUID and
uncertain-delivery handling still applies. No live gateway migration is implied.

The connector is not the relay server. To self-host the relay on a Windows PC,
use the existing Linux image through Docker Desktop's Linux containers; see
[Windows relay hosting](../services/mobile/README.md#windows-hosts-use-linux-containers)
for WSL setup, persistent volumes, private configuration and HTTPS. A native
Windows relay port is not part of this deployment.

## Start the relay

The [relay README](../services/mobile/README.md) documents Rust and Docker
installation, administrator commands, limits and push configuration. The default SQLite profile supports one relay process for local/private setups.
Production multiple-worker deployments use the documented
[PostgreSQL profile](../services/mobile/README.md#postgresql-production-profile);
existing SQLite relays move as described in [SQLite installations](#sqlite-installations).
A minimal SQLite Docker deployment is:

```sh
cd services/mobile
docker compose build
docker compose run --rm relay provision \
  --name 'Example workspace' --computer-name 'Example computer'
docker compose up -d relay
```

Provisioning prints a workspace UUID, computer UUID, computer credential and a
phone invitation. Save these privately. The invitation is usable once and expires
after ten minutes; generate another with the local `invite --workspace UUID`
command when ready to pair.

Terminate TLS with your existing reverse proxy, forwarding to
`127.0.0.1:8787`, or use the optional Caddy profile. A proxy must allow at least
40 seconds for long polls and 1 MiB ordinary request bodies. File-bearing sends
use a larger envelope only at `/v1/requests`: allow 29 MiB there and 75 seconds
for a mobile upload. Disable API request buffering to disk and request/body/header
logging. Only the HTTPS port should be accessible remotely. Configure
`trusted_proxy_cidrs` to the exact proxy peer IP/CIDR visible to the relay; for a
host-local Nginx this is normally `127.0.0.1/32`. Never trust every private subnet
or caller-supplied forwarding headers. The supplied Caddy profile overwrites
`X-Forwarded-For` and uses its explicitly assigned bridge address; change both
its network and the relay's private trust configuration if the subnet conflicts.
Keep the dynamic IPAM range separate from Caddy's static address; the supplied
`172.30.78.128/25` allocation range excludes its `.2` address.

The relay limits each credential to 2,400 authenticated requests a minute and
admits concurrent work through bounded lanes that all credentials share; a busy
lane answers 429. It does not limit requests globally or per client address, so
it does not guarantee fairness between workspaces. That is enough for a private
relay. Before a shared relay opens to the public, add global and
per-address request limits in the reverse proxy, for example Nginx `limit_req`
or a Caddy build with a rate-limit module: the supplied Caddy profile has none.

`/healthz` is cheap process liveness, independent of database availability.
`/readyz` coalesces a bounded database readiness probe with a one-second cache. Inline attachment ingestion and claim
responses share one large-payload lane per worker until complete; overload
returns 429 with a retry hint. Private response spooling uses disk, with a
64 MiB reservation budget per worker, rather than the container's small tmpfs.
Phone catalogs and receipts enforce the installed 1 MiB total response limit
before sending headers. Event pages fit an encoded prefix and advance only
through returned IDs; oversized legacy data returns an explicit 413.
Keep spool permissions 0700, reserve free disk for all workers and monitor actual
container memory as well as process RSS. A 256 MiB limit is a safety setting,
not demonstrated high-fanout capacity. See the relay README for limits and the
[storage/admission contract](relay-architecture.md) for scale boundaries.

```nginx
location / {
    proxy_pass http://127.0.0.1:8787;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_set_header Forwarded "";
    proxy_set_header Connection "";
    proxy_read_timeout 45s;
    proxy_send_timeout 45s;
    proxy_request_buffering off;
    client_max_body_size 1m;
    access_log off;
}

location = /v1/requests {
    proxy_pass http://127.0.0.1:8787;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_set_header Forwarded "";
    proxy_set_header Connection "";
    proxy_read_timeout 75s;
    proxy_send_timeout 75s;
    client_body_timeout 75s;
    client_max_body_size 29m;
    proxy_request_buffering off;
    access_log off;
}
```

Use a real certificate trusted by Android. The application does not disable
certificate validation or accept arbitrary cleartext endpoints. Verify
`https://relay.example.com/healthz` returns `ok: true` and `protocol_version: 1`.
The reserved domain in these examples must be replaced with your own.

## Connect a computer

Each computer runs the outbound connector beside the `hgs` it already has. It
uses the local CLI, so existing native accounts, hooks, conversations and safety
checks continue to apply. Do not stop or relaunch native sessions for setup.

Install the Rust connector, `zerus-mobile-connector`, with the rest of Zerus:

- **Arch Linux package** (`zerus`, `zerus-git`, `zerus-ade-bin` or
  `zerus-ade-nightly-bin`): `/usr/bin/zerus-mobile-connector` and its user unit
  `zerus-mobile-connector.service`, which the package never enables.
- **Zerus.app with the bundled connector**: use its signed
  `Contents/MacOS/zerus-mobile-connector`. Older bundles do not contain it; use
  the source installation below until upgrading to a bundle that does.
- **Source checkout**: `HGS_MOBILE_CONNECTOR=1 ./install.sh` builds
  `~/.local/bin/zerus-mobile-connector`; later runs keep it current.

Run `zerus-mobile-connector --version` to confirm it (use the complete bundled
path when it is not on `PATH`). Configure it in Zerus under
Settings → [Mobile connection](#configure-the-connector-from-the-desktop), or
create the file by hand:

```sh
install -d -m 700 "$HOME/.config/hgs/mobile"
```

Create `$HOME/.config/hgs/mobile/connector.json` privately with mode 0600:

```json
{
  "server_url": "https://relay.example.com",
  "node_token": "REPLACE_WITH_PRIVATE_COMPUTER_CREDENTIAL",
  "hgs_path": "/home/user/.local/bin/hgs",
  "poll_interval": 5,
  "state_dir": "/home/user/.local/state/hgs/mobile-connector"
}
```

Use absolute paths appropriate for the computer. Do not paste credentials into
shell command arguments, issues or version control. The state directory belongs
to one relay and computer credential; use a new directory if those change and
retain the old one for uncertain-delivery review.

`zerus-mobile-connector --check --config "$HOME/.config/hgs/mobile/connector.json"`
reports the configuration, the journal and their permissions without
connecting or changing anything; fix what it refuses first. Then start the
service from the user service manager, so it does not inherit a running
agent's environment:

- **Arch package:** `systemctl --user enable --now zerus-mobile-connector.service`.
- **Source install on Linux:** `python3 scripts/install-mobile-connector-service.py install`
  writes `~/.config/systemd/user/zerus-mobile-connector.service` for
  `~/.local/bin/zerus-mobile-connector`, then enable it as above.
- **macOS:** from a source checkout,
  `python3 scripts/install-mobile-connector-service.py install` writes the
  `com.hgdev.zerus-mobile-connector` LaunchAgent. Bundles with the connector
  setup action also expose `zerus-setup --mobile-connector install`. Load the
  agent with the printed `launchctl bootstrap` command.

The installation helpers only write definitions and refuse to replace a
definition they did not write. They never enable, load, start or restart the
connector. The separate `systemctl --user enable --now` and
`launchctl bootstrap` commands above explicitly start it.

### Python connector (legacy)

Computers set up before the Rust connector run the Python package from a venv.
It stays supported until each one has [moved](#move-a-computer-to-the-rust-connector):

```sh
python3 -m venv "$HOME/.local/share/zerus-mobile/venv"
"$HOME/.local/share/zerus-mobile/venv/bin/pip" install ./services/mobile
"$HOME/.local/share/zerus-mobile/venv/bin/zerus-mobile-connector" \
  --config "$HOME/.config/hgs/mobile/connector.json"
```

Its systemd user service:

```ini
[Unit]
Description=Zerus mobile computer connector
After=network-online.target

[Service]
Type=simple
ExecStart=%h/.local/share/zerus-mobile/venv/bin/zerus-mobile-connector --config %h/.config/hgs/mobile/connector.json
Restart=on-failure
RestartSec=5
UMask=0077
NoNewPrivileges=true
KillMode=process

[Install]
WantedBy=default.target
```

It lives in `~/.config/systemd/user/zerus-mobile-connector.service`, where it
takes precedence over the packaged unit of the same name.

Agents launched from the phone run in tmux. A launch that finds no tmux server
running starts one in a transient scope of its own (see
[Terminal, SSH and clipboard](reference.md#terminal-ssh-and-clipboard)); a server
that an earlier `hgs` started, or one started where systemd could not make that
scope, runs inside this service. `KillMode=process` limits systemd to the
connector process itself when the service stops, restarts or exits; the default
`control-group` mode also kills a tmux server inside it and every agent in it.
Restart the connector with `systemctl --user restart zerus-mobile-connector.service`;
native agents keep running. Do not stop it while agents run; see
[Protect running agents](#protect-running-agents).

### Protect running agents

A connector unit made from an earlier version of this guide sets no `KillMode`,
so it uses the `control-group` default. Protect it before its next restart:

```sh
mkdir -p ~/.config/systemd/user/zerus-mobile-connector.service.d
printf '[Service]\nKillMode=process\n' \
  > ~/.config/systemd/user/zerus-mobile-connector.service.d/50-zerus-killmode.conf
systemctl --user daemon-reload
systemctl --user show -p KillMode zerus-mobile-connector.service
```

`daemon-reload` restarts nothing, although it also loads any other unit file
changes made since the last reload. The connector keeps running, and the new mode
applies from its next stop or restart. The last command reports the effective
value and must print `KillMode=process`. A drop-in that already sets it is enough;
one that sorts later, such as `override.conf` from `systemctl --user edit`, takes
precedence over this file.

`zerus-setup --mobile-connector install` (see [Connect a computer](#connect-a-computer))
writes a user unit for a `~/.local/bin` connector with its absolute path, the
session's `PATH` and `KillMode=process`, relies on the packaged unit for the
Arch package's binary, and on macOS writes the LaunchAgent with
`AbandonProcessGroup`, `KeepAlive` on failure only and logs in
`~/Library/Logs/hgs`.

With the Linux package installed, `zerus-setup --mobile-connector protect` adds
the protection above: it writes that drop-in only when the unit runs a
`zerus-mobile-connector` whose effective kill mode is not already `process`,
reloads the user manager and checks the result, and removes its file again if a
later drop-in wins. Its `migrate` and `rollback` actions
[move a computer to the Rust connector](#move-a-computer-to-the-rust-connector)
and back with a `60-zerus-rust-connector.conf` drop-in. `migrate` refuses while a
connector holds the journal; `rollback` only removes its own drop-in and must
follow the drain and stop steps below. Neither action enables, starts, stops or
restarts the unit.

Prefer `restart` to `stop`, whatever the kill mode. tmux builds with systemd
support, such as Arch's, put every pane in a `tmux-spawn-*.scope` unit that is
`PartOf=` the unit holding the tmux server, whichever client created the pane, so
`systemctl --user stop` ends those panes. Stop a protected connector only when
`systemctl --user show -p ConsistsOf zerus-mobile-connector.service` lists no
`tmux-spawn-*.scope` units.

### Move a computer to the Rust connector

Move one computer at a time, deliberately. `connector.json` and `state_dir` are
used in place and never copied, reset or rebound: the journal keeps the
no-replay record of every phone request. A repeated request returns its stored
receipt; interrupted mutations remain uncertain rather than executing again.

1. **Install** the Rust connector as above and keep the venv. Run
   `zerus-mobile-connector --version`.
2. **Protect agents:** `zerus-setup --mobile-connector protect` (Arch package) or
   `python3 scripts/install-mobile-connector-service.py protect` (source), and
   check `systemctl --user show -p KillMode zerus-mobile-connector.service`
   prints `KillMode=process`. Never stop the unit before this passes.
3. **Record, read-only:** `systemctl --user cat zerus-mobile-connector.service`,
   `systemctl --user show -p ExecStart,KillMode,FragmentPath,DropInPaths
   zerus-mobile-connector.service`, `hgs status`,
   `zerus-mobile-connector --check --json --config <file>` and the venv's version
   (`pip show zerus-mobile`). Note the tmux server and agent process IDs.
4. **Drain:** pause phone actions for this computer and repeat `--check` until no
   request is `running`. Undelivered results need not drain.
5. **Stop only the connector:** first check that
   `systemctl --user show -p ConsistsOf zerus-mobile-connector.service` lists no
   `tmux-spawn-*.scope` units, since stopping would also end those panes (see
   [Protect running agents](#protect-running-agents)); then
   `systemctl --user stop zerus-mobile-connector.service`. `hgs status` shows it
   stopped; agents, tmux and DeepSeek hosts keep running.
6. **Switch:** `zerus-setup --mobile-connector migrate` (add
   `--binary PATH` when both `/usr/bin` and `~/.local/bin` copies exist). It
   writes only `60-zerus-rust-connector.conf` beside the unit and reloads the user
   manager. It refuses while the journal is still held, for a unit that does not
   run `<venv>/bin/zerus-mobile-connector --config FILE`, and when a later drop-in
   such as `override.conf` would keep the old program.
7. **Start:** `systemctl --user start zerus-mobile-connector.service`.
8. **Verify:** `systemctl --user show -p ExecStart,KillMode` names the Rust binary
   and `process`; `hgs status` shows the connector running; `--check` reports the
   same binding and counts apart from pruning; the unit's journal shows no startup
   error; the phone shows the computer online with the same capabilities and
   peers; one inspect and one disposable send, never to an unrelated live agent,
   succeed; the tmux and agent process IDs match step 3. Reopen phone actions.
   Early `429` answers while the old poll lease expires and a short offline blip
   are expected.

On macOS no Python LaunchAgent was documented. Record the actual Python command,
the read-only `--check` report and native process IDs, drain phone actions, and
stop only the foreground Python connector. Install the new agent as described
under [Connect a computer](#connect-a-computer), then run its printed
`launchctl bootstrap` command. Verify the binding, counts, capabilities, peers
and native process IDs as in step 8, using launchd rather than systemd for the
connector status. A manually created LaunchAgent is not adopted or overwritten
by the installer; inspect its configuration before planning a separate cutover.

### Roll back to the Python connector

On Linux, drain and stop the unit as in steps 4 and 5 (agents survive under
`KillMode=process`), run `zerus-setup --mobile-connector rollback`, which removes
only `60-zerus-rust-connector.conf` and reloads while keeping the kill-mode
protection, then start the unit and verify as in step 8. The Python connector
opens the same journal in place. Never copy, restore, clear or rebind
`journal.sqlite3`: an older copy loses the no-replay record. Never resend a
request whose outcome is uncertain, and never kill native agents, tmux servers or
DeepSeek hosts.

On macOS, drain and stop only the Rust connector, then restore the recorded
Python command against the same configuration and journal. Keep
`AbandonProcessGroup` when stopping a launchd agent and verify that native process
IDs survive. Do not replace an existing custom LaunchAgent automatically.

## Configure the connector from the desktop

Settings → Mobile connection edits the computer's existing connector file at
`$XDG_CONFIG_HOME/hgs/mobile/connector.json`, or
`$HOME/.config/hgs/mobile/connector.json` when `XDG_CONFIG_HOME` is unset.
The default official URL is `https://relay.zerus.dev`. The configured computer
credential stays hidden; leaving Replacement computer credential blank retains
it. Saving writes private configuration only. Changes take effect after the
connector restarts. On Linux the panel offers its restart command only once the
service reports `KillMode=process`, so native agents and their hosts keep running;
otherwise it offers the [protection](#protect-running-agents) command instead.
Copied commands require deliberate execution. Wait for active connector requests
and receipts to drain before restarting it.

While the connector file is missing, the status starts with *not configured* and
no start or restart command is offered. The panel names the program the service
runs: the legacy Python connector (a venv's console script, with the migration
hint), the Rust connector (noting a `zerus-setup --mobile-connector migrate`
drop-in) or another program, and says when the unit is not enabled at login. On
macOS it reads the `com.hgdev.zerus-mobile-connector` LaunchAgent with
`launchctl print`, within the same 3 s and 16 KiB bounds, and offers
`launchctl kickstart -k` once a configuration exists.

The managed official-relay migration preserves the original binding identity and
receipt directory. A different relay or replacement computer credential requires
a new, unused private receipt directory. The panel never copies or deletes old
receipts; retain them for uncertain-delivery review. Connector service status is
read-only and does not prove relay connectivity or that the saved configuration
is currently active.

## Pair the phone

On a desktop computer with Zerus, open **Machines → Phones → Pair a phone…** (or
Settings → Mobile connection). It works on the gateway itself and on any computer
that has the gateway as an enabled peer. On the phone, open Zerus, choose
**Machines → Pair another workspace** and scan the QR code, or enter the HTTPS
server URL and the code shown, and confirm. Without a desktop, the relay
administrator's `invite --workspace UUID` command prints a code as before. A `zerus://pair?server=...&code=...` link can
prefill these fields, but the app still requires explicit confirmation. Treat
such a link as a credential until it is consumed or expires.

The app opens on Sessions, grouped by projects containing matching sessions.
Use the desktop-style All sessions, Needs attention, Active, Saved sessions,
and Archive filters, or select one or more computers. Projects shows the logical
project catalog, including empty projects; project details offer a View sessions
action. Open a session to read messages, send a message or answer a supported
question. Attach documents using the composer's file picker: up to
eight files, ten MiB per file and twenty MiB total. The app keeps private copies
with the draft. Sending immediately adds an outgoing message with its delivery
status and frees the composer for the next text.

Previously opened conversations show retained private cached history, when
available for the exact target, while the computer refreshes it. The header
distinguishes last known state from verified current state. Typing remains
available during refresh; sending and other native actions
wait for a verified session. The composer expands for long text, keeps keyboard
selection during updates, and brings your own outgoing message into view.
Reading older messages pauses automatic following; use Jump to latest to resume.

The context row above the composer shows native context usage and provider cache
state. Open it for the reported window, token breakdown, last-request cache use,
native cost data and compaction state. Unknown values stay unknown. A reported
warm-cache deadline is an estimate; cached or archived inspections are marked
as last known.

The header also opens session details and Terminal. Details includes native model
and effort settings, task lists, goals, child activity and process output. Native
capabilities determine available lifecycle actions. Terminal uses the existing
pane and a separate saved input buffer; its key toolbar can interact with native
prompts. A result marked unconfirmed must be checked through its original
receipt before preparing another attempt.

Upgrade the connector's `hgs` executable together with the connector when using
new native operations. A private executable path in `connector.json` can keep
the mobile connector on a reviewed build without replacing the desktop CLI.
Before restarting a connector, wait for its active mutation attempts to finish.
Restart only the connector service, once it is [protected](#protect-running-agents);
leave native agents, tmux and owning DeepSeek hosts running. Older resident
adapters may advertise fewer supported operations.

### Upgrade gateway sharing

An enrolled connector publishes its own computer and enabled direct Zerus peers.
Peer requests travel through that connector and its existing SSH configuration;
neighbors do not receive relay credentials. Update `hgs` on both the gateway and
each peer before enabling this feature. A phone already paired to the workspace
can read the additional computer rows without pairing each peer separately.

Do not roll this change out across mixed old and new relay workers. An old
worker does not understand the immutable route stored with a peer request.

1. Validate the new native binaries on the gateway and peers, then replace only
   their CLI executables. Preserve native sessions and running agent processes.
2. Temporarily block new phone request submissions while existing connector
   claims and result uploads drain. Check both relay queues and the connector's
   durable journal for running work and undelivered receipts. Then block claim
   polling and stop only the connector service, once it holds no agent panes
   (see [Protect running agents](#protect-running-agents)).
3. Take and verify a PostgreSQL backup. Stop every old relay worker before
   starting the new version; apply its additive registry schema and start all
   workers from the same reviewed image. Keep the existing database, credentials,
   request identities and accounting. Never activate a historical SQLite copy.
4. Install the new connector package while preserving its original configuration
   and journal. Start it against the updated native CLI and relay. Verify gateway
   and peer heartbeats, then permit claim polling by the upgraded connector for
   a synthetic route/receipt check before reopening ordinary phone submissions.
   Existing user conversations need no test messages.
5. Check that a peer's displayed data belongs to its native machine UUID and that
   removing its configured edge makes that route unavailable. A disconnected
   gateway must not leave its neighbors online through that gateway.

After guarded requests have been stored, rollback must use a version that
understands their route metadata and per-poll capability opt-in. Keep admission
closed if no such version is available. An older relay must never consume this
queue: preserving the database alone does not make its old claim logic safe.
An older connector cannot claim guarded requests; do not erase route metadata
or clear its journal to bypass this restriction. Uncertain mutations stay
uncertain and are never automatically sent again through another route.

Upgrade both relay and connector before using context controls. Each advertises
its supported operations, and the agent's live inspection must also permit the
action. Compact keeps the draft. Clear requires confirmation and does not send
it. When continuing with a cold cache, choose full context, compact and continue,
clear, or cancel. Compact and continue sends only after the exact native request
completes and the original draft remains unchanged. Editing, navigating away,
restarting or cancelling the pending send keeps it from being sent automatically.
An uncertain operation offers a read-only receipt check rather than a retry.

The Drafts tab preserves unsent text, files and uncertain attempts. An unknown
delivery must be checked against its
receipt and the conversation before deliberately sending again. Archive entries
open their exact saved conversation in read-only mode. Upgrade both the relay
and computer connector before using archive inspection. Session creation and
restoration are available when the gateway, connector and native adapter
advertise the corresponding operations. Archive content stays read-only;
restore, rename, fork and forget operate on its exact saved identity.

Disconnecting a workspace revokes that phone's credential and push registration.
Saved drafts remain private on the phone. An administrator can also revoke a
phone or computer through the relay CLI.

## Enable notifications

Grant Android's notification permission when prompted. Machines shows each
workspace's push status and the optional live connection.

- For FCM, configure the Android build and the relay using your Firebase project.
  The relay needs its own private service-account file; never put that file in the
  APK. Both sides must use the same Firebase project. Without those credentials,
  FCM delivery is unavailable even though the integration code builds. The app
  registers automatically after pairing and on startup.
- The live connection needs no Firebase project. Enable it while
  the app is open; its persistent notification exposes a Stop action. It uses
  an ongoing outbound connection. Android Doze, vendor battery management and
  force-stop can affect delivery, so it is a fallback rather than a guarantee of
  background wake availability.

Push payloads contain a generic wake signal. The app retrieves current events
through the authenticated relay. A provider's successful HTTP response only
acknowledges acceptance; verify delivery on the actual target phone.

## Verify and recover

Use a disposable session to test pairing, reading, sending, answering and an
attention notification. Close the phone UI for the notification check. Then
temporarily disconnect the computer network: the app should report the computer
offline after refreshing the computer catalog. An open conversation retains its
cached history and draft, labels its native status Last known after an inspection
fails, and blocks native actions until a new inspection succeeds. Never test
destructive lifecycle operations on an unrelated live agent.

The source includes a fully synthetic `fixture_hgs.py` used by the HTTP
integration tests. It does not invoke native agents or tmux. Run local checks:

```sh
PYTHONPATH=services/mobile python3 -m unittest discover -s services/mobile/tests -v
python3 scripts/ci/check-source.py
```

Run the Python tests in an environment with the relay dependencies installed.
Android unit tests and lint are separate from these tests. No command in this
runbook dispatches hosted CI.

Back up PostgreSQL with a reviewed backup/restore procedure that includes the
permanent mutation ledger. Never restore old command claims to the queued state.
A claimed request after a relay or connector crash remains uncertain until
reviewed. Keep API access logs off, preserve the connector journal, and revoke
exposed credentials individually.

### SQLite installations

The relay runs on PostgreSQL. The SQLite backend remains only for
single-process local setups until it is removed. Back up a SQLite relay through
its online backup API or with the relay stopped.

The `migrate-sqlite` command was removed after
[v0.42.2](https://github.com/ufna/zerus/releases/tag/v0.42.2), the last stable
release that contains it. To move a SQLite relay to PostgreSQL, install that
tag's `services/mobile` Python package, which pins asyncpg, in a private virtual
environment:

```sh
python3 -m venv /private/zerus-migrate
/private/zerus-migrate/bin/pip install \
  'git+https://github.com/ufna/zerus@v0.42.2#subdirectory=services/mobile'
```

Then follow that release's
[offline SQLite migration](https://github.com/ufna/zerus/blob/v0.42.2/services/mobile/README.md#offline-sqlite-migration)
procedure. The cutover is an offline owner-approved operation: keep the old
relay writers stopped, preserve outstanding claims and identities, import into
an empty target with `migrate-sqlite --source-offline`, and verify before
starting new admission. After new PostgreSQL writes, an old SQLite snapshot is
not a safe rollback because it loses new mutation receipts.
