Zerus @VERSION@ for Windows x86_64 (portable control console)
hgs @CLI_VERSION@, Qt @QT_VERSION@, source commit @SOURCE_COMMIT@

This folder is a portable Zerus control console. hgs-tray.exe manages agent
sessions on remote Linux and macOS machines through the Windows OpenSSH client.
Agents do not run on this Windows computer yet: local sessions, local accounts,
the DeepSeek Native UI and the mobile gateway are not available on Windows.
Documentation: https://github.com/ufna/zerus/blob/main/docs/installation.md#windows

Requirements
- Windows 10 version 1903 or later, or Windows 11, on x86_64.
- The OpenSSH client (ssh.exe), included with current Windows versions.
- Key-based SSH access to each remote machine, with hgs installed there in
  ~/.local/bin/hgs. Host aliases from %USERPROFILE%\.ssh\config work as usual.

Use
- Extract the whole folder and run hgs-tray.exe. Keep the files together:
  hgs-tray.exe uses the hgs.exe next to it.
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
nightly releases at https://github.com/ufna/zerus/releases, and Settings >
Updates reports them. To update, quit hgs-tray from its tray menu, then replace
this folder with a newer one. The executables are not code-signed, so
Windows SmartScreen may ask for confirmation on the first start. Compare the
zip with SHA256SUMS of its GitHub release, for example:
  Get-FileHash zerus-<version>-windows-x86_64.zip -Algorithm SHA256

Licenses
Zerus is licensed under the MIT license in LICENSE; THIRD_PARTY_NOTICES.md
describes the components it uses. The folder also contains:
- Qt @QT_VERSION@ libraries and plugins, including Qt WebEngine, used under the
  GNU LGPL version 3 (licenses\qt). They are separate DLLs that can be replaced
  with compatible builds. Their source code is available from
  https://download.qt.io/official_releases/qt/ for this exact version.
- SPDX documents supplied with Qt (licenses\qt\sbom) that name the license
  and copyright of each Qt module and of the third-party code inside it.
  licenses\qt holds the text of every standard license and exception they
  name; the documents for the Chromium components in Qt WebEngine carry their
  own license texts. This software is based in part on the work of the
  Independent JPEG Group and of the FreeType Team (https://freetype.org).
- The Microsoft Visual C++ runtime DLLs, redistributed with the application.
- License texts of the Rust crates in hgs.exe (licenses\rust) and of libvterm
  and md4c, which hgs-tray.exe contains (licenses).
