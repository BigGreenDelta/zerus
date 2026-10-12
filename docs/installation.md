# Install Zerus

[Product overview](../README.md) | [Detailed reference](reference.md#installation)

Zerus runs on Linux and macOS. Arch Linux x86_64 users can install the complete
CLI and desktop bundle from AUR. Other installations build from source. The CLI
keeps the name `hgs`; the desktop executable is `hgs-tray`.

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

For the desktop: CMake, a C++17 compiler and Qt 6 with Widgets, WebEngine and Multimedia.
Linux also needs KDE Frameworks 6 KStatusNotifierItem, KWindowSystem and KIdleTime,
a system tray and user systemd for the standard autostart setup. On macOS, the
installer uses Qt from Homebrew, and the desktop app requires macOS 14 or later.

## Local installation

```sh
git clone https://github.com/ufna/zerus.git
cd zerus
./install.sh
./tray/install.sh
```

On macOS, install the dependencies first:

```sh
brew install rust python tmux cmake qt qtwebengine qtmultimedia
```

On Arch Linux, desktop dependencies include `cmake`, `qt6-base`, `qt6-webengine`, `qt6-svg`,
`kstatusnotifieritem`, `kwindowsystem`, `kidletime` and C++ build tools.

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
| [zerus-ade-nightly-bin](https://aur.archlinux.org/packages/zerus-ade-nightly-bin) | `yay -S zerus-ade-nightly-bin` | Prebuilt nightly after full Linux/Arch/macOS checks |

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
