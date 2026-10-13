# Third-party components

Zerus original source and artwork are licensed under the MIT license in `LICENSE`.
Dependency licenses and copyright notices remain in effect.

## Vendored code

`src/state/data/codex-0.160.1-greetings.txt` retains the finite native empty-state
greeting vocabulary from OpenAI Codex `rust-v0.160.1` for exact reset recognition.
`src/state/data/codex-empty-state-60x21.txt` retains its released settled idle-logo
glyphs, verified against 0.160.1, 0.162.0 and 0.162.1. Source provenance is recorded
in each file; the original Apache-2.0 license is
retained in `docs/licenses/codex-LICENSE.txt`, which is also shipped with packages.
No native Codex executable is bundled.

`tray/vendor/libvterm` contains libvterm 0.3.3 under MIT. Its original copyright
notice and complete license are retained in `tray/vendor/libvterm/LICENSE`;
source provenance and archive checksum are in `UPSTREAM.md` beside it.

`tray/vendor/md4c` contains the md4c 0.5.2 Markdown parser and entity table
under MIT. Its original license is retained in `tray/vendor/md4c/LICENSE.md`;
source provenance and archive checksum are in `UPSTREAM.md` beside it.

## GitHub logo

The About page uses the [GitHub mark from Primer Octicons](
https://github.com/primer/octicons/blob/97825f832c98f817867f770d084c08e3edc6f78c/icons/mark-github-16.svg). Its complete MIT license and GitHub copyright notice are retained
in `tray/resources/icons/github-mark.svg`, including the embedded Qt resource.

## Dependencies obtained during build

Rust dependencies and versions are locked in `Cargo.lock`. Their license
expressions and source repositories are recorded in `docs/rust-dependencies.json`.
This inventory is metadata, not a replacement for dependency license texts.
Cargo downloads the corresponding sources and license files from crates.io.

The GUI links Qt 6 (Widgets, Network and Multimedia), and on Linux the KDE
Frameworks KStatusNotifierItem, KWindowSystem and KIdleTime. These retain their
own licenses. On Linux, the optional embedded DeepSeek native UI plugin
(`libhgs-tray-native-ui.so`) links Qt WebEngine, which includes Chromium and
additional third-party components. See the
[Qt WebEngine licensing documentation](https://doc.qt.io/qt-6.8/qtwebengine-licensing.html)
and the notices supplied by the exact Qt distribution used to build the application.

The Arch package links Qt/KDE dynamically and requires their system packages; it
does not bundle Qt, KDE or Chromium. Qt WebEngine is an optional dependency used
only by the native UI plugin. Their distribution supplies their notices
and corresponding source. `scripts/collect-licenses.py` collects the exact
host-target Cargo dependency license texts and inventory into the package,
alongside the Zerus and vendored libvterm and md4c licenses. A missing dependency notice
fails packaging. Review notices and any source/relinking obligations again if
the linkage or bundled components change. MIT licensing of Zerus itself does
not relicense its dependencies.

The Windows portable zip is different: it bundles Qt 6 for MSVC, including Qt
Multimedia with its audio runtime, next to the application. It contains no Qt
WebEngine or Chromium. These are separate, replaceable DLLs. Qt is used under the GNU LGPL
version 3; its third-party components retain their own licenses. The zip carries
the SPDX documents supplied
with the exact Qt installation, which name the license and copyright of each Qt
module and of the third-party code inside it (libjpeg-turbo, FreeType, HarfBuzz,
libpng, zlib and others), and `packaging/windows/licenses` with the text of every
standard license and exception those documents name. Based in part on the work of the Independent JPEG Group
and of the FreeType Team. The zip also carries the Rust, libvterm, md4c and Codex
notices and the Microsoft Visual C++ runtime DLLs, redistributed with the
application from Visual Studio's redistributable folder. It contains no Windows
system library, such as ICU or the Direct3D shader compiler, no Windows SDK shader
compiler and no software OpenGL fallback. `scripts/package-windows.py` fails when
the Qt documents are missing or name a license without a shipped text, and when
the bundle contains one of those libraries.

The macOS application distributed through the Homebrew cask is different: it
bundles the official Qt 6.11.2 frameworks and plugins it needs (including Qt SVG
and Qt Multimedia) as unmodified, dynamically linked and replaceable Qt components
under LGPL-3.0-only. It contains no Qt WebEngine or Chromium. Third-party audio components retain their own license notices.
`Zerus.app/Contents/Resources/licenses/qt` contains the LGPL-3.0 and GPL-3.0
texts, Qt's official SPDX documents for every bundled module, including its
third-party components, and the location of the exact Qt sources. The same folder also carries the Zerus, libvterm, md4c, Codex and
Cargo dependency notices; `scripts/check-macos-bundle.py` rejects a bundle when
any of them is missing.

The CLI's `rusqlite` build includes SQLite through `libsqlite3-sys`. The collected
notices also preserve its upstream public-domain dedication, separate from the
Rust binding's MIT notice. [SQLite copyright](https://www.sqlite.org/copyright.html).

Native agent CLIs are installed separately by the user and are not included here.

## Android Markdown dependencies

The Android application bundles CommonMark Java 0.30.0 and its GFM tables,
strikethrough, task-list and autolink extensions under BSD-2-Clause, plus
autolink-java 0.12.0 under MIT. Both projects are maintained by Robin Stocker
and contributors. Their exact upstream license texts are retained in
`mobile/android/app/src/main/assets/licenses/commonmark-java.txt` and
`mobile/android/app/src/main/assets/licenses/autolink-java.txt` and shown in the
application's third-party notices. Sources: [CommonMark Java](
https://github.com/commonmark/commonmark-java/tree/commonmark-parent-0.30.0) and
[autolink-java](https://github.com/robinst/autolink-java/tree/autolink-0.12.0).
