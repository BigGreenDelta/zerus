Zerus @VERSION@ for Windows x86_64 (portable)
hgs @CLI_VERSION@, Qt @QT_VERSION@, source commit @SOURCE_COMMIT@

This folder is a portable Zerus control console. hgs-tray.exe manages agent
sessions on remote Linux and macOS machines through the Windows OpenSSH client.
It can also run agents on this computer once you enable local sessions: choose
Enable local sessions for this computer in Machines, or run
hgs.exe session-host install. This stages the hgs CLI in
%USERPROFILE%\.local\bin and a session host in
%USERPROFILE%\.local\share\hgs\host; hgs.exe session-host uninstall disables
local sessions again. Claude Code, Codex and Kimi Code then run here once
installed; DeepSeek and OpenCode do not. Install agent... in Accounts installs
Codex with npm (Node.js required) and Claude Code and Kimi Code with their
vendors' PowerShell installers.

Mobile connection in Settings configures this PC's bundled Rust outbound
connector. Supply the computer credential issued by your relay, save, and
choose Start connector. Refresh Phones to pair a phone by QR code. No Linux
or macOS gateway is needed. Start it again after each Windows login; closing
Zerus leaves the connector and native sessions running. Stop connector drains
its current request and preserves native agents and durable request receipts.
This first Windows gateway controls existing local Codex, Claude and Kimi
sessions. Mobile session creation, folder/worktree/Git/file operations, peer
routing, voice/speech, DeepSeek and OpenCode are unavailable.
Documentation: https://github.com/ufna/zerus/blob/main/docs/installation.md#windows

Requirements
- Windows 10 version 1903 or later, or Windows 11, on x86_64.
- The OpenSSH client (ssh.exe), included with current Windows versions.
- Key-based SSH access to each remote machine, with hgs installed there in
  ~/.local/bin/hgs. Host aliases from %USERPROFILE%\.ssh\config work as usual.

Use
- Extract the whole folder and run hgs-tray.exe. Keep the files together:
  hgs-tray.exe uses the hgs.exe next to it.
- hgs-tray.exe opens the Zerus window. Closing it keeps Zerus running in the
  notification area, where Windows 11 first hides a new icon behind the ^
  arrow; run hgs-tray.exe again to bring the window back. To start Zerus at
  sign-in, put a shortcut to hgs-tray.exe in the Startup folder
  (shell:startup) and add a space and --background at the end of its Target
  field: Zerus then starts in the notification area without its window.
- To use the CLI in a terminal, run hgs.exe from this folder or add the folder
  to PATH. In PowerShell, a program in the current folder needs the .\ prefix
  and a machine argument needs quotes:
    .\hgs.exe '@myserver' ls
  With the folder on PATH, PowerShell takes hgs.exe '@myserver' ls, and
  cmd.exe takes hgs.exe @myserver ls without quotes. In Git Bash, set
  MSYS_NO_PATHCONV=1 before passing remote paths such as /workspace/project.
- Configuration lives in %USERPROFILE%\.config\hgs (under HOME instead when
  that variable is set), as on Linux and macOS. HGS_CONFIG_DIR and
  HGS_STATE_DIR move only the CLI's configuration and state; hgs-tray.exe
  always reads tray.conf from .config\hgs under HOME, or else under the
  account's profile folder.

There is no installer and no automatic update. Newer builds come with the
stable and nightly releases at https://github.com/ufna/zerus/releases, and
Settings > Updates reports them. To update, quit hgs-tray from its tray menu, then replace
this folder with a newer one. The executables are not code-signed, so
Windows SmartScreen may ask for confirmation on the first start. Compare the
zip with SHA256SUMS of its GitHub release, for example:
  Get-FileHash zerus-<version>-windows-x86_64.zip -Algorithm SHA256

Licenses
Zerus is licensed under the MIT license in LICENSE; THIRD_PARTY_NOTICES.md
describes the components it uses. The folder also contains:
- Qt @QT_VERSION@ libraries and plugins, used under the GNU LGPL version 3
  (licenses\qt). They are separate DLLs that can be replaced with compatible
  builds. Their exact source code is available from
    https://download.qt.io/archive/qt/@QT_SERIES@/@QT_VERSION@/single/qt-everywhere-src-@QT_VERSION@.tar.xz
  and as Git tags v@QT_VERSION@ in the Qt Project repositories at
  https://code.qt.io.
- SPDX documents supplied with Qt (licenses\qt\sbom) that name the license
  and copyright of each Qt module and of the third-party code inside it.
  licenses\qt holds the text of every standard license and exception they
  name. This software is based in part on the work of the Independent JPEG
  Group and of the FreeType Team (https://freetype.org).
- The Microsoft Visual C++ runtime DLLs, redistributed with the application.
- Microsoft's ConPTY, conpty.dll and OpenConsole.exe from the NuGet package
  Microsoft.Windows.Console.ConPTY @CONPTY_VERSION@, under the MIT license
  (licenses\conpty-LICENSE.txt), with the notices of the third-party code
  that OpenConsole.exe contains (licenses\conpty-NOTICE.md). hgs.exe hosts
  consoles with them once local sessions are enabled.
- License texts of the Rust crates in hgs.exe (licenses\rust) and of libvterm,
  md4c and the QR Code generator, which hgs-tray.exe contains (licenses).
