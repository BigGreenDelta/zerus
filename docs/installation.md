# Install Zerus

[Product overview](../README.md) | [Detailed reference](reference.md#installation)

Zerus runs on Linux and macOS. Arch Linux x86_64 users can install the complete
CLI and desktop bundle from AUR; Apple Silicon Macs can use the Homebrew cask.
Other installations build from source. The CLI keeps the name `hgs`; the desktop
executable is `hgs-tray`. Windows has a portable [control console](#windows) for
remote Linux and macOS machines; it can also run agents on the Windows computer
itself.

## Requirements

For the CLI: the newest stable Rust, a C compiler, Python 3, tmux 3.7+ and OpenSSH. Install the
agents you want to use on the machines where they will run.

The ADE's verified answer/message delivery needs tmux 3.7's input-mode inspection.
Some distributions ship older tmux packages; use a supported upstream release.
Installing a newer binary does not replace an already running tmux server. Check
the server with `tmux display-message -p '#{version}'`; schedule any server
migration deliberately after saving your sessions. Never kill a live server or
native agents as part of a Zerus GUI/package update.
[Official tmux releases](https://github.com/tmux/tmux/releases).

For the desktop: CMake, a C++17 compiler and Qt 6 with Widgets and Multimedia.
Linux also needs KDE Frameworks 6 KStatusNotifierItem, KWindowSystem and KIdleTime,
a system tray and user systemd for the standard autostart setup. On Linux, Qt
WebEngine is optional: when it is installed, the build adds the embedded
DeepSeek [Native UI](reference.md#deepseek); otherwise Native UI opens in your
browser. On macOS, the installer uses Qt from Homebrew, and the desktop app
requires macOS 14 or later.

## Local installation

```sh
git clone https://github.com/ufna/zerus.git
cd zerus
./install.sh
./tray/install.sh
```

On macOS, install the dependencies first:

```sh
brew install rust python tmux cmake qt qtmultimedia
```

The macOS source installer also needs a valid application-signing certificate
and its private key in your keychain. It selects a certificate compatible with
the installed app, including its team and distribution class. For a first install
it prefers Developer ID Application, then Apple Development. If selection is
ambiguous, choose an exact name or SHA-1 fingerprint from
`security find-identity -v -p codesigning` with `HGS_MACOS_SIGN_IDENTITY`:

```sh
HGS_MACOS_SIGN_IDENTITY='Developer ID Application: Example (ABCDE12345)' ./tray/install.sh
```

Compatible certificate signatures keep the privacy identity across local
rebuilds. The installer verifies both designated requirements before replacing
an existing certificate-signed app and stops if they are incompatible. It never
falls back to ad hoc signing. Moving from an older ad hoc installation can require
fresh consent once; no permissions are reset automatically. Source builds link
Homebrew Qt and are signed locally without the hardened runtime or notarization;
use the published app for distribution. See [macOS signing](macos-distribution.md#local-source-builds).

On Arch Linux, desktop dependencies include `cmake`, `qt6-base`, `qt6-tools`, `qt6-multimedia`, `qt6-svg`,
`kstatusnotifieritem`, `kwindowsystem`, `kidletime` and C++ build tools; add
`qt6-webengine` for the embedded Native UI.

The desktop starts in English on every operating system. Settings > Appearance >
Interface language offers an optional Russian preview; restart only Zerus to
apply it. Agent sessions keep running, and untranslated text stays in English.
See [translation coverage and maintenance](../tray/translations/README.md).

Add `~/.local/bin` to `PATH`. The CLI installs to `~/.local/bin/hgs`; SSH calls
use that exact path. The desktop installs to `~/.local/bin/hgs-tray` on Linux or
`~/Applications/hgs-tray.app` on macOS. The installer configures autostart through
systemd or launchd.

`HGS_MOBILE_CONNECTOR=1 ./install.sh` also builds the Rust mobile connector
(`services/mobile/connector`) into `~/.local/bin/zerus-mobile-connector`. Later
runs update that copy when it changed; `HGS_MOBILE_CONNECTOR=0` always skips it.
The installer never enables, starts or restarts the connector's service: when the
running service uses the replaced binary, it prints the restart command to run
after phone actions have drained. `mac-install.sh` passes the variable to the
Mac's build. [Connect a computer](mobile-deployment.md#connect-a-computer) sets up
its service, and [Move a computer to the Rust connector](mobile-deployment.md#move-a-computer-to-the-rust-connector)
switches one that runs the Python connector.

Open the workspace from the command line:

```sh
hgs-tray --sessions                         # Linux
~/Applications/hgs-tray.app/Contents/MacOS/hgs-tray --sessions  # macOS
```

The repository's `tmux.conf` is a suggested configuration. If you already have
your own, merge the settings you need manually: `hgs` uses the machine's shared
tmux server.

## Arch Linux

Choose one package; all include the CLI and desktop and conflict with one another:

| Package | Installation | Contents |
| --- | --- | --- |
| [zerus-ade-bin](https://aur.archlinux.org/packages/zerus-ade-bin) | `yay -S zerus-ade-bin` | Stable prebuilt Arch x86_64 bundle |
| [zerus](https://aur.archlinux.org/packages/zerus) | `yay -S zerus` | Stable release compiled locally |
| [zerus-git](https://aur.archlinux.org/packages/zerus-git) | `yay -S zerus-git` | Current upstream main compiled locally |
| [zerus-ade-nightly-bin](https://aur.archlinux.org/packages/zerus-ade-nightly-bin) | `yay -S zerus-ade-nightly-bin` | Prebuilt nightly after full Linux/Arch/macOS/Windows checks |

Keep Arch fully updated before installing. Binary library version requirements
come from the verified release builder; source packages compile against your
installed libraries. The [0.42.0 release](https://github.com/ufna/zerus/releases/tag/v0.42.0)
includes immutable source/binary archives, checked pacman packages, AUR recipes
and SHA-256 checksums. Native agents are installed separately.

Nightly packages update through ordinary `yay -Syu`; source-based `zerus-git`
updates need `yay -Syu --devel`. Nightly releases use immutable build URLs and
checksums and leave the latest stable release unchanged. See
[automatic nightly publication](ci-and-aur.md#automatic-nightly-publication).

After installation, run this once as your normal user, **without sudo**:

```sh
zerus-setup
```

It creates the `~/.local/bin/hgs` compatibility links used by SSH. Existing
source-install files are preserved: inspect and move them aside deliberately if
you want to migrate to the system package. Keep your configuration and agent state.

Services and autostart are opt-in. Inspect old user unit files before enabling
packaged services; local definitions can shadow the system package. Package
installation and upgrades do not restart native agents, tmux or the GUI.
[Service setup and migration](ci-and-aur.md#package-contents-and-compatibility).

## macOS (DMG)

For Apple Silicon Macs running macOS 14 or later, download
`zerus-VERSION-macos-arm64.dmg` from the [latest stable release](https://github.com/ufna/zerus/releases/latest)
or a selected [nightly release](https://github.com/ufna/zerus/releases?q=nightly-&expanded=true).
Open the image, drag **Zerus** into **Applications**, then eject the image and
open Zerus from Applications. The application and disk image are signed with
Developer ID and notarized by Apple. Signed stable macOS downloads begin with
[0.43.0](https://github.com/ufna/zerus/releases/tag/v0.43.0), and nightly downloads with
[nightly 0.42.2.r1291.g5d2e5d0.n17](https://github.com/ufna/zerus/releases/tag/nightly-38794972859);
older releases may not contain a macOS download.

To use the bundled CLI and SSH session bridge, run the per-user setup once,
without sudo:

```sh
/Applications/Zerus.app/Contents/Resources/bin/zerus-setup
```

Local tmux sessions require `tmux` (`brew install tmux`). Agent providers are
installed separately. The Homebrew cask below installs tmux and links the CLI
automatically. For a manual upgrade, quit Zerus from its menu, replace the app
with the new DMG's copy and open Zerus again; native agents keep running.
**Settings > Updates** checks the selected channel and offers **Download DMG**
with replacement instructions. Dev DMGs come from the verified
[desktop Dev catalog](https://zerus.dev/updates/v2/desktop/dev.json) on zerus.dev;
stable and nightly DMGs come from GitHub Releases. Updates are installed manually.

## macOS (Homebrew)

Apple Silicon Macs running macOS 14 or later can install the signed and notarized
application and CLI from the Zerus tap:

```sh
brew install --cask ufna/zerus/zerus
zerus-setup
```

The tap supports three channels. `zerus` follows stable releases; `zerus@nightly` follows
the daily build from main that passed the full CI, published at the same time as
the AUR nightly. `zerus@dev` follows successful main CI builds, with the same
Apple signature and notarization. All install the same `Zerus.app`, so only one can be installed.
Settings, agents and conversations are shared. Switch between Stable and Nightly with:

```sh
brew uninstall --cask zerus && brew install --cask ufna/zerus/zerus@nightly
brew uninstall --cask zerus@nightly && brew install --cask ufna/zerus/zerus
```

For Dev, install `brew install --cask ufna/zerus/zerus@dev`. To switch from nightly:

```sh
brew uninstall --cask zerus@nightly && brew install --cask ufna/zerus/zerus@dev
```

Zerus checks its installed channel unless you choose another one in
**Settings > Updates**. That page shows the complete installed version, a copyable
Homebrew update/channel-switch command and a separate DMG download.

The cask installs `/Applications/Zerus.app`, links `hgs` and `zerus-setup` into
Homebrew's `bin` directory and installs `tmux` as a dependency. It starts nothing
and does not write to your home directory. Run `zerus-setup` once as your normal
user, **without sudo**. It links `~/.local/bin/hgs` for SSH protocols and installs
the on-demand session bridge that lets agents started over SSH use your login
Keychain. Options:

```sh
zerus-setup --autostart   # start Zerus at login
zerus-setup --workers     # recovery and swarm background workers
```

Through 0.42.2, `--autostart` wrote a LaunchAgent that launchd runs as a background
daemon: whenever other programs keep every core busy, the Zerus window stops
responding and is restarted after 30 seconds. A Zerus that its login item started at
background priority shows **Fix login item** in the Sessions window's status bar.
Nothing changes until you choose: **Update Only** (the default) runs
`zerus-setup --autostart` and applies at your next login while Zerus keeps running;
**Update and Restart** also quits and reopens Zerus now. Saved drafts, sessions and
agents stay; a voice or video message that is still recording stops and is not
kept. You can also run `zerus-setup --autostart` again yourself. It applies at your
next login; to apply it now, quit Zerus from its menu and reload the agent:

```sh
launchctl bootout "gui/$(id -u)/com.hgdev.hgs-tray"
launchctl bootstrap "gui/$(id -u)" ~/Library/LaunchAgents/com.hgdev.hgs-tray.plist
```

`brew upgrade --cask zerus` (or the installed nightly/dev cask) replaces the application but never
quits it, unloads its LaunchAgents or stops tmux and native agents. When the
installed bundle is another build than the running GUI, Zerus shows **Restart to
finish update**; drafts are kept. Background workers finish their current round
and exit when an upgrade replaces their executable, and launchd starts the new
version. A worker that `launchctl print "gui/$(id -u)/com.hgdev.hgs-swarm"` shows
without a `pid` and with `last exit code = 78: EX_CONFIG` cannot be started: the
system log reports `Unable to get updated LWCR`, a launch constraint from its Login
Items record that names an earlier build, which launchd's retries do not renew. Running
`zerus-setup --workers` (or `install.sh` for a source installation) again loads such
a worker again, as does:

```sh
launchctl bootout "gui/$(id -u)/com.hgdev.hgs-swarm"
launchctl bootstrap "gui/$(id -u)" ~/Library/LaunchAgents/com.hgdev.hgs-swarm.plist
```

Use `com.hgdev.hgs-recovery` the same way.

To uninstall, remove the per-user integration first:

```sh
zerus-setup --remove
brew uninstall --cask zerus   # use zerus@nightly for Nightly, or zerus@dev for Dev
```

`brew uninstall --zap --cask zerus` (or `zerus@nightly` / `zerus@dev`) additionally removes the Zerus LaunchAgents,
including ones created by a source installation, the logs and saved window state.
`~/.config/hgs`, agent state and conversations are never removed.

### Migrating from a source installation

`zerus-setup` changes nothing while it finds a source installation:
`~/Applications/hgs-tray.app`, a regular file at `~/.local/bin/hgs`, or Zerus
LaunchAgents that run a program outside `/Applications/Zerus.app`. Keep your
configuration and agent state, then migrate deliberately:

1. Quit Zerus from its menu bar icon. Native agents and tmux keep running.
2. Unload the old agents, ignoring ones you never installed:

   ```sh
   for label in com.hgdev.hgs-tray com.hgdev.hgs-recovery com.hgdev.hgs-swarm com.hgdev.hgs.user-session; do
     launchctl bootout "gui/$(id -u)/$label" 2>/dev/null || true
   done
   ```

3. Move `~/Applications/hgs-tray.app`, `~/.local/bin/hgs`, `~/.local/bin/hgs_state.py`
   and the matching `~/Library/LaunchAgents/com.hgdev.hgs*.plist` files into a
   backup folder.
4. Run `zerus-setup` with the options you used before.

To return to the source installation, run `zerus-setup --remove` and restore the
backup.

<a id="windows"></a>

## Windows

Stable releases after 0.42.2 and nightly releases include a portable Windows
x86_64 zip with the desktop app (`hgs-tray.exe`) and the CLI (`hgs.exe`). It starts
as a control console: it manages agent sessions on your Linux and macOS machines
through the Windows OpenSSH client. You can also
[enable local sessions](#local-sessions-on-windows), and agents then run on the
Windows computer itself, in a session host of your user account on ConPTY, without
WSL ([Windows plan](superpowers/specs/2026-10-10-windows-support-design.md),
[session host](superpowers/specs/2026-10-13-windows-native-host-design.md)).

Requirements:

- Windows 10 version 1903 or later, or Windows 11, on x86_64. Local sessions are
  tested on Windows 11 and on the Windows Server 2025 CI runners.
- For remote machines, the OpenSSH client. `ssh -V` in PowerShell shows whether it
  is installed; otherwise add **OpenSSH Client** in Settings → Optional features.
- For remote machines, at least one Linux or macOS machine with `hgs` installed at
  `~/.local/bin/hgs` (see [local installation](#local-installation)) and key-based
  SSH access from Windows. Status polls run SSH in batch mode, so the key must work
  without a passphrase prompt, for example through the Windows `ssh-agent` service
  (disabled by default; enabling it needs an administrator). Host aliases from
  `%USERPROFILE%\.ssh\config` work as usual.
- For local sessions, the agents you want to run, installed on the Windows
  computer ([below](#local-sessions-on-windows)).

Install and start:

1. From the [latest stable release](https://github.com/ufna/zerus/releases/latest)
   or a [nightly release](https://github.com/ufna/zerus/releases?q=nightly-&expanded=true),
   download `zerus-<version>-windows-x86_64.zip` and `SHA256SUMS`. Each release
   carries the zip its own CI run tested; stable 0.42.2 and earlier have none.
2. Check the zip in PowerShell. Both commands must show the same hash
   (`Get-FileHash` prints it in upper case):

   ```powershell
   Get-FileHash .\zerus-<version>-windows-x86_64.zip -Algorithm SHA256
   Select-String -Path .\SHA256SUMS -Pattern windows-x86_64.zip
   ```

3. Extract the whole zip and run `zerus-windows-x86_64\hgs-tray.exe`. Keep the
   files together: the app uses the `hgs.exe` next to it. The executables are not
   code-signed, so SmartScreen may show **Windows protected your PC** on the first
   start; choose **More info → Run anyway** only for a zip whose hash matched.
   The Zerus window opens. Closing it keeps Zerus running in the notification
   area, where Windows 11 first hides a new icon behind the **^** arrow; run
   `hgs-tray.exe` again to bring the window back.
4. To manage remote machines, add them in **Machines**. Accept a new host key once
   with **Open SSH terminal**, then use **Test connection**.
   **Install / update HGS…** builds `hgs` on the remote machine from a Zerus source
   checkout on this computer; the remote machine needs the
   [CLI requirements](#requirements). A checkout whose `install.sh` or
   `hgs_state.py` has CRLF line endings is refused.

In PowerShell, run the CLI from the folder with `.\hgs.exe` and quote a machine
argument, because `@` is special there: `.\hgs.exe '@build-01' ls`. Commands
copied from the desktop are quoted for PowerShell the same way. From Git Bash, set
`MSYS_NO_PATHCONV=1` before passing remote paths such as `/workspace/orbit`;
otherwise Git Bash rewrites them into Windows paths.

Configuration and state use the same layout as on Linux and macOS under
`%USERPROFILE%`, or under `HOME` when it is set: `.config\hgs` holds `config`,
`machines.local.json` and `tray.conf`, and `.local\state\hgs` holds the CLI state.
`HGS_CONFIG_DIR` and `HGS_STATE_DIR` move only the CLI's configuration and state;
the desktop app always reads `tray.conf` from `.config\hgs` under `HOME`, or
else under the account's profile folder, whatever `USERPROFILE` says. Desktop
settings are in `%APPDATA%\hgdev\hgs-tray.ini`, and drafts in the
`hgs-tray.ini.drafts` folder next to it. The computer's name in the fleet is
`HGS_SELF` when set, otherwise its lowercase `COMPUTERNAME`.

### Local sessions on Windows

Local sessions stay off until you enable them. Open **Machines** (**Open
Machines** in New session while no computer is available), select this computer
(*This computer: control console*) and choose **Enable local sessions**, or run
`.\hgs.exe session-host install` in the extracted folder. Either way, `hgs`
stages these files in your profile (under `HOME` instead when it is set):

- `%USERPROFILE%\.local\bin\hgs.exe`, the stable CLI that sessions, agent hooks and
  the recovery worker run, so they never depend on the extracted folder;
- `%USERPROFILE%\.local\share\hgs\host\<hash>\` with `hgs.exe` and the zip's ConPTY
  pair (`conpty.dll`, `OpenConsole.exe`), the image the session host starts from;
- the marker `%USERPROFILE%\.config\hgs\local-sessions`, which alone decides that
  local sessions are on (`HGS_CONFIG_DIR` moves it with the configuration).

Zerus then lists this computer's sessions, offers it in New session and Accounts,
and runs the stable CLI from its next start. When Zerus starts from another zip of
the same or a newer version, it first stages that zip's `hgs.exe` again (a small
*Starting Zerus…* note may show meanwhile); running sessions stay in the host that
runs them, and the next host starts from the new image. **Machines** shows the
session host's state for this computer, and `hgs.exe session-host status` reports
the marker, the staged files and the host.

The session host starts with the first launch, resume or fork and exits a few
seconds after its last session has ended; listings never start it. Sessions keep
running when Zerus closes or updates and when the terminal or SSH connection that
started them ends. They end when you sign out of Windows or restart it; tracked
sessions stay saved and can be resumed, as after a reboot on Linux.

Install the agents from **Accounts** with **Install agent…**, or yourself. On
Windows, Codex installs with npm (`npm.cmd`, so Node.js must be installed), and
Claude Code and Kimi Code with their vendors' PowerShell installers (`install.ps1`),
which verify the binaries they download. Zerus looks the agents up on `PATH` (with
`PATHEXT`), adds `%USERPROFILE%\.local\bin`, npm's `%APPDATA%\npm` and Kimi Code's
`%USERPROFILE%\.kimi-code\bin`, and runs the script of an npm command shim with
Node.js directly; other batch files are refused. A project's `run_claude.ps1`,
`run_codex.ps1` or `run_kimi.ps1` runs with PowerShell in place of the agent
([project wrappers](reference.md#project-wrappers)); a `run_*.sh` is not used on
Windows.

- **Claude Code** and **Codex** run tracked, with Zerus hooks in their
  configuration. Every automatic CI run tests their
  [reviewed](ci-and-aur.md#native-agent-compatibility-and-upstream-review) Windows
  builds through the session host, and both were also checked on a Windows 11 PC.
  Claude Code before 2.1.296 needs Git for Windows for its hooks: without Git Bash
  Zerus refuses to track it, and `HGS_TRACKING=0` runs it untracked.
- **Kimi Code** runs tracked as well. It was checked on a Windows 11 PC and has no
  Windows CI suite yet.
- **Run PowerShell** in New session (`hgs sh`) opens an untracked PowerShell 7
  (`pwsh`), or Windows PowerShell when PowerShell 7 is missing.
- DeepSeek and OpenCode are not offered for this computer.

Start Zerus and `hgs.exe` without elevation: a session host started from an
elevated terminal refuses every client that is not elevated, Zerus included, with
*the session host on this computer runs elevated*. A host started over SSH runs in
another Windows session, where agents cannot open windows on your desktop, such as
a browser sign-in; Zerus asks before it starts or resumes a session there
(**Launch anyway**).

A session host must not run in a job that ends it early, such as an agent's
sandbox or a terminal that starts it, so `hgs` refuses one that cannot leave such
a job (*the session host runs inside a job that does not let it break away*).
Some desktops run every process in a job, Explorer included; there the host shares
Explorer's job and ends with the desktop at sign-out, as sessions do anyway, and
`hgs.exe session-host status` reports it *in the desktop's job*.

To disable local sessions again, end the sessions on this computer and run
`hgs.exe session-host uninstall`, which refuses while one runs. It removes the
marker, so the computer is a control console again; the staged CLI and host images
stay in place. **Machines** has no button for this. `hgs.exe session-host stop`
stops an idle session host without disabling anything.

### Windows limitations

- Until local sessions are enabled, the computer has no local sessions, agent
  accounts or hooks, and `hgs.exe` exits with code 2 for local session commands.
  Commands for another machine (`hgs.exe '@build-01' …`) work as on Linux.
- No DeepSeek or OpenCode sessions on the Windows computer, and no DeepSeek Native
  UI from Windows: it needs SSH connection sharing, which Windows OpenSSH lacks, so
  a DeepSeek session cannot be opened externally either. Its Activity still works;
  DeepSeek sessions have no Terminal tab on any platform.
- Voice messages cannot be sent to sessions on the Windows computer.
- Mobile connection runs the bundled Rust connector on this PC. Configure its
  HTTPS relay and computer credential, choose Start connector, then refresh
  Phones and pair by QR code. No other gateway is needed. Start the connector
  again after login; closing the GUI keeps it and native agents running.
  The first Windows gateway controls existing local Codex/Claude/Kimi sessions;
  mobile creation, folder/worktree/Git/file operations, peer routing and
  voice/speech are unavailable. Stop drains the current request and keeps
  the independent session host and native agents running.
- Other machines cannot connect to the Windows computer as a peer: its local
  sessions appear only in Zerus on that computer, and its folders in Projects are
  not shared with other machines.
- No installer, code signing, autostart or self-update. **Settings → Updates**
  starts on the Stable channel; choose Nightly to hear about nightly builds too.
  It reports a release with a newer product version than the running zip, with
  a link to its release page. Stable 0.42.2 and earlier have no
  Windows build, so they are never reported; Dev has none either, so on that
  channel it says so and reports no update. To update, quit Zerus from the
  tray menu and replace the folder with the new one; configuration and state
  stay in your profile. To start Zerus at sign-in, put a shortcut to
  `hgs-tray.exe` in the Startup folder (`shell:startup`) and add a space and
  `--background` at the end of its **Target** field, so Zerus starts in the
  notification area without opening its window.
- Without SSH connection sharing, every poll of every machine is a full SSH
  handshake: about 150 ms per machine on a LAN. The app lists this computer at
  most once every 15 seconds, or every 5 seconds with local sessions enabled.

[Windows details in the reference](reference.md#windows) |
[Building on Windows](../CONTRIBUTING.md#windows)

## Checking for updates

Open **Settings > Updates** to see the installed and published desktop versions,
the last successful check and any check error. **Update checks** contains the
automatic-check switch, release channel and **Check now** action. Automatic
checks run at most once a day for Stable/Nightly and every 15 minutes for Dev; manual checks remain available when
automatic checks are disabled.

When a newer release is verified, the workspace shows a compact **New version**
button with the Zerus mark. Its tooltip identifies Zerus and the release version;
clicking it opens Updates. The tray menu also offers Updates, and fresh newer
releases can show a system notification.

**View release** opens the published release page. Expand **Show installation
details** for package ownership and copyable Arch or Homebrew commands. Mac
downloads also offer a DMG button and manual replacement instructions. There, **Installation
instructions** opens this guide at the section for your system:
[Arch Linux](#arch-linux) on Arch-based Linux, [local installation](#local-installation)
on other Linux systems, [Homebrew](#macos-homebrew) on macOS and [Windows](#windows)
on Windows. These actions do not install an update automatically. A local build
remains separate from an optional AUR installation.

## Other machines

Add an SSH connection in **Machines**. The agent's machine needs `hgs` installed
at `~/.local/bin/hgs`, the selected agent and the project folder. SSH settings and
keys stay local.

To install on a Mac from another machine:

```sh
./mac-install.sh <ssh-alias>            # CLI and desktop
./mac-install.sh <ssh-alias> --no-tray  # CLI only
```

Desktop signing uses the Mac's existing keychain. An optional
`HGS_MACOS_SIGN_IDENTITY` selector is forwarded to the remote installer; private
keys and signing credentials are never copied from the calling machine.

The CLI configuration lives at `~/.config/hgs/config`. For terminal use, you can
set SSH aliases directly:

```sh
HGS_SELF="workstation"
HGS_PEERS="macbook build-01"
```

Use **Projects** to collect folders from different machines into one project.
The shared project catalog syncs between connected members; local SSH access is
managed in **Machines**.

## Agents

Use **Accounts** to configure accounts and install supported agents.
On Arch, installing Codex or DeepSeek from Accounts requires both the optional
`nodejs` and `npm` packages. For a fresh npm setup, choose a writable user prefix
before installing agents; Zerus already recognizes `~/.local/bin`:

```sh
sudo pacman -S --needed nodejs npm
npm config set prefix "$HOME/.local"
```

Keep an existing working npm prefix if you already manage native agents there.
The current official DeepSeek Harness adapter is pinned to
`@deepseek-ai/dsh@0.2.1-rc.2`:

```sh
npm install -g @deepseek-ai/dsh@0.2.1-rc.2
hgs account login native-dsh
```

Then choose **DeepSeek Harness** in **New session**.
[Adapter capabilities and limitations](reference.md#deepseek).

See the [reference](reference.md) for commands and operational details, and
[CONTRIBUTING.md](../CONTRIBUTING.md) for build checks.
