# Install Zerus

[Product overview](../README.md) | [Detailed reference](reference.md#installation)

Zerus runs on Linux and macOS. Arch Linux x86_64 users can install the complete
CLI and desktop bundle from AUR; Apple Silicon Macs can use the Homebrew cask.
Other installations build from source. The CLI keeps the name `hgs`; the desktop
executable is `hgs-tray`. Windows has a portable [control console](#windows) for
remote Linux and macOS machines.

## Requirements

For the CLI: Rust 1.85+, a C compiler, Python 3, tmux 3.7+ and OpenSSH. Install the
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

On Arch Linux, desktop dependencies include `cmake`, `qt6-base`, `qt6-multimedia`, `qt6-svg`,
`kstatusnotifieritem`, `kwindowsystem`, `kidletime` and C++ build tools; add
`qt6-webengine` for the embedded Native UI.

Add `~/.local/bin` to `PATH`. The CLI installs to `~/.local/bin/hgs`; SSH calls
use that exact path. The desktop installs to `~/.local/bin/hgs-tray` on Linux or
`~/Applications/hgs-tray.app` on macOS. The installer configures autostart through
systemd or launchd.

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

## macOS (Homebrew)

Apple Silicon Macs running macOS 14 or later can install the signed and notarized
application and CLI from the Zerus tap. The casks appear in the tap with the first
signed builds.

```sh
brew install --cask ufna/zerus/zerus
zerus-setup
```

The tap has two channels. `zerus` follows stable releases; `zerus@nightly` follows
the daily build from main that passed the full CI, published at the same time as
the AUR nightly. Both install the same `Zerus.app`, so only one can be installed.
Settings, agents and conversations are shared. To switch channels:

```sh
brew uninstall --cask zerus && brew install --cask ufna/zerus/zerus@nightly
brew uninstall --cask zerus@nightly && brew install --cask ufna/zerus/zerus
```

A Zerus installed from `zerus@nightly` checks the nightly update channel unless you
choose another one in **Settings > Updates**. The Dev channel, built from every main
push, has no cask; use `zerus@nightly` for prebuilt builds from main.

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

`brew upgrade --cask zerus` (or `zerus@nightly`) replaces the application but never
quits it, unloads its LaunchAgents or stops tmux and native agents. When the
installed bundle is another build than the running GUI, Zerus shows **Restart to
finish update**; drafts are kept. Background workers keep the previous binary until
you restart them:

```sh
launchctl kickstart -k "gui/$(id -u)/com.hgdev.hgs-recovery"
launchctl kickstart -k "gui/$(id -u)/com.hgdev.hgs-swarm"
```

To uninstall, remove the per-user integration first:

```sh
zerus-setup --remove
brew uninstall --cask zerus
```

`brew uninstall --zap --cask zerus` (or `zerus@nightly`) additionally removes the Zerus LaunchAgents,
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

## Windows control console

Nightly releases include a portable Windows x86_64 zip with the desktop app
(`hgs-tray.exe`) and the CLI (`hgs.exe`). They manage agent sessions on your Linux
and macOS machines through the Windows OpenSSH client. Agents do not run on the
Windows computer itself: this is the first phase of the
[Windows plan](superpowers/specs/2026-10-10-windows-support-design.md).

Requirements:

- Windows 10 version 1903 or later, or Windows 11, on x86_64.
- The OpenSSH client. `ssh -V` in PowerShell shows whether it is installed;
  otherwise add **OpenSSH Client** in Settings → Optional features.
- At least one Linux or macOS machine with `hgs` installed at `~/.local/bin/hgs`
  (see [local installation](#local-installation)) and key-based SSH access from
  Windows. Status polls run SSH in batch mode, so the key must work without a
  passphrase prompt, for example through the Windows `ssh-agent` service (disabled
  by default; enabling it needs an administrator). Host aliases from
  `%USERPROFILE%\.ssh\config` work as usual.

Install and start:

1. From a [nightly release](https://github.com/ufna/zerus/releases?q=nightly-&expanded=true),
   download `zerus-<version>-windows-x86_64.zip` and `SHA256SUMS`.
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
4. Add your machines in **Machines**. Accept a new host key once with
   **Open SSH terminal**, then use **Test connection**. **Install / update HGS…**
   builds `hgs` on the remote machine from a Zerus source checkout on this computer;
   the remote machine needs the [CLI requirements](#requirements). A checkout whose
   `install.sh` or `hgs_state.py` has CRLF line endings is refused.

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

Limitations of the control console:

- No local sessions, local agent accounts or hooks: `hgs.exe` exits with code 2 for
  local session commands. Commands for another machine (`hgs.exe '@build-01' …`)
  work as on Linux.
- No DeepSeek Native UI from Windows: it needs SSH connection sharing, which
  Windows OpenSSH lacks, so a DeepSeek session cannot be opened externally
  either. Its Activity still works; DeepSeek sessions have no Terminal tab on any
  platform.
- No mobile gateway: pair your phone with a Linux or macOS machine. Settings →
  Mobile connection is hidden, and activity on the Windows computer does not
  delay phone alerts.
- Other machines cannot connect to the Windows computer as a peer.
- No installer, code signing, autostart or self-update, and only nightly
  releases carry the zip. **Settings → Updates** reports nightlies with a newer
  product version; Stable and Dev have no Windows build, so on those channels it
  says so and reports no update. To update, quit Zerus from the tray menu and
  replace the folder with the new one; configuration and state stay in your
  profile. To start Zerus at sign-in, put a shortcut to `hgs-tray.exe` in the
  Startup folder (`shell:startup`).
- Without SSH connection sharing, every poll of every machine is a full SSH
  handshake: about 150 ms per machine on a LAN. The app lists this computer at
  most once every 15 seconds.

[Windows details in the reference](reference.md#windows) |
[Building on Windows](../CONTRIBUTING.md#windows)

## Checking for updates

Open **Settings > Updates** to see the installed and published desktop versions,
the last successful check and any check error. **Update checks** contains the
automatic-check switch, release channel and **Check now** action. Automatic
checks run at most once a day per channel; manual checks remain available when
automatic checks are disabled.

When a newer release is verified, the workspace shows a compact **New version**
button with the Zerus mark. Its tooltip identifies Zerus and the release version;
clicking it opens Updates. The tray menu also offers Updates, and fresh newer
releases can show a system notification.

**View release** opens the published release page. Expand **Show installation
details** for package ownership, copyable Arch commands and installation
instructions. These actions do not install an update automatically. A local
build remains separate from an optional AUR installation.

## Other machines

Add an SSH connection in **Machines**. The agent's machine needs `hgs` installed
at `~/.local/bin/hgs`, the selected agent and the project folder. SSH settings and
keys stay local.

To install on a Mac from another machine:

```sh
./mac-install.sh <ssh-alias>            # CLI and desktop
./mac-install.sh <ssh-alias> --no-tray  # CLI only
```

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
`@deepseek-ai/dsh@0.2.0-rc.2`:

```sh
npm install -g @deepseek-ai/dsh@0.2.0-rc.2
hgs account login native-dsh
```

Then choose **DeepSeek Harness** in **New session**.
[Adapter capabilities and limitations](reference.md#deepseek).

See the [reference](reference.md) for commands and operational details, and
[CONTRIBUTING.md](../CONTRIBUTING.md) for build checks.
