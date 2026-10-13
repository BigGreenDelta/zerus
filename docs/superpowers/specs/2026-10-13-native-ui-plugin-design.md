# Optional Native UI Plugin Design

Tracking epic: `zerus-abnk`.

## Goal

Remove Qt WebEngine from the base desktop build on every platform. Qt WebEngine
serves one feature: the embedded **Native UI** tab of DeepSeek Harness sessions.
It adds Chromium (roughly 150-200 MB per bundle), a sandbox that conflicts with
AppArmor in self-contained Linux bundles, and a separately signed helper on macOS.
The base build must not pay that cost for an optional view.

The embedded view stays available as an optional Linux plugin. Without it, Native
UI opens in the system browser through the existing `hgs native-ui` handoff, which
already provides the SSH tunnel and the authenticated launch URL. No DeepSeek
function is lost: native fork, global history search, schedules and account
transfer remain available in the browser.

Non-goals: a downloadable add-on for the Windows zip, the macOS bundle or an
AppImage (each would need a WebEngine runtime matching its exact Qt build and, on
macOS, separate signing); embedding other web content; changing the CLI or the
DeepSeek host.

## Owner decisions (2026-10-13)

| Topic | Decision |
| --- | --- |
| Base build | No Qt WebEngine on any platform |
| Embedded view | Optional Qt plugin, Linux only |
| Arch packages | `qt6-webengine` in `optdepends`; source packages keep it in `makedepends` |
| Windows zip, macOS bundle, AppImage | Browser only, no plugin |
| Plugin boundary | A narrow browser-surface interface; Native UI logic stays in the core |

## Architecture

### Interface (`tray/src/NativeBrowser.h`)

A header-only interface the core and the plugin share. It depends only on Qt Core
and Widgets declarations:

- `NativeBrowser`: one isolated browser for one origin. `widget()` returns the
  view it owns, `load(url)` navigates, and two callbacks report
  `loadFinished(bool)` and `rendererStopped()`. Callbacks, not signals: signals
  would need the core's meta-object inside the plugin.
- `NativeBrowserFactory`: `create(origin, background, parent)` returns a new
  `NativeBrowser`. Declared with `Q_DECLARE_INTERFACE` and the IID
  `dev.zerus.hgs-tray.NativeBrowserFactory/1`.

Any change to either class changes the IID suffix. A plugin with another IID is
rejected and the GUI falls back to the browser, so a stale plugin can never be
called through a different interface.

### Plugin (`tray/plugins/NativeUiPlugin.cpp`)

`libhgs-tray-native-ui.so`, a Qt plugin linking Qt Widgets and Qt WebEngine
Widgets, nothing from hgs-tray. It contains the code that today lives in
`NativeUiView.cpp` behind `QWebEngine*`:

- one off-the-record `QWebEngineProfile` per browser (cookies and cache in memory);
- main-frame navigation limited to the launch origin; user-initiated web links
  elsewhere open in the system browser;
- `LocalContentCanAccessFileUrls` disabled;
- JavaScript console messages dropped (they can contain the launch URL);
- downloads saved only through a save dialog;
- the view is destroyed before its profile.

### Loader (`tray/src/NativeBrowserPlugin.{h,cpp}`)

`NativeBrowserPlugin::factory()` loads the plugin once per process, on first use,
and caches the result. It searches only fixed paths relative to the executable:

1. `<bin>/../lib/hgs-tray/plugins/libhgs-tray-native-ui.so` (Arch:
   `/usr/lib/hgs-tray/plugins`; local install: `~/.local/lib/hgs-tray/plugins`);
2. `<bin>/plugins/libhgs-tray-native-ui.so` (the CMake build tree).

No environment variable or setting can add a path. On non-Linux platforms the
search list is empty. A missing file, a failed `dlopen` (for example when
`qt6-webengine` is not installed) or an incompatible IID yields no factory and one
log line with the reason; the reason never contains a launch URL. Tests can inject
a factory with `setFactoryForTesting()`.

Qt requires `Qt::AA_ShareOpenGLContexts` before `QApplication` when WebEngine is
loaded later from a plugin; `main.cpp` already sets it.

### Core changes

- `NativeUiView` keeps the connection logic: `HgsClient::requestNativeUi`, the
  loopback URL check, per host/origin/launch-token-fingerprint browser keys,
  status text, Reload and **Respond in Activity**. It receives a factory provider
  and creates browsers through it.
- `SessionsWindow` shows the Native UI tab for a DeepSeek session only when a
  factory is available. The four routes that currently switch to the tab (DeepSeek
  sign-in, the question card's terminal action, recovery's terminal action and
  inspection of a failed message) go through one `showNativeUi()` helper: switch
  to the tab when the plugin is available, otherwise call
  `HgsClient::openNativeUi` to open the system browser.
- `hgs-tray` and its static libraries no longer link `Qt6::WebEngineWidgets`.

### Build option

`HGS_NATIVE_UI_PLUGIN` (`AUTO`, `ON`, `OFF`; default `AUTO`). On Linux, `AUTO`
builds the plugin when `Qt6WebEngineWidgets` is found and `ON` requires it. On
macOS and Windows the plugin is never built and `ON` is a configuration error. The
plugin is written to `<build>/plugins/` and installed by CMake to
`${CMAKE_INSTALL_LIBDIR}/hgs-tray/plugins`.

## Packaging

### Arch Linux

- `zerus`, `zerus-git`: `qt6-webengine` moves from `depends` to `makedepends` and
  `optdepends` (`qt6-webengine: embedded DeepSeek Native UI`).
- `zerus-ade-bin`, `zerus-ade-nightly-bin`: `qt6-webengine` leaves the
  build-library floors and appears in `optdepends`.
- `scripts/package-linux.py --native-ui-plugin` stages the plugin at
  `usr/lib/hgs-tray/plugins/`.
- `scripts/check-package.py` requires the plugin and rejects an `hgs-tray` whose
  dynamic section needs `libQt6WebEngine*`.
- Existing installations keep `qt6-webengine`: an optional dependency is not an
  orphan for `pacman -Qdt`, so the embedded view keeps working after an upgrade.
- Candidates sealed before this change recorded `qt6-webengine` as a required
  build library. Release and nightly validation accept that legacy set (and
  render the legacy nightly recipe exactly), so retrying their publication still
  works without rebuilding.
- namcap must report the plugin's WebEngine dependency only as optional; CI fails
  on errors.

### Local installation (`tray/install.sh`, Linux)

The build uses `AUTO`. When it produced the plugin, the installer copies it to
`~/.local/lib/hgs-tray/plugins/` through a temporary name and an atomic rename
before replacing the GUI; a running GUI keeps its loaded copy. When the build has
no plugin, a previously installed plugin is removed so an old plugin never runs
with a newer GUI. `update-installed.py` is unchanged.

### Windows portable zip

Qt is installed without `qtwebengine`, `qtwebchannel` and `qtpositioning`.
`windows_portable.REQUIRED` drops the WebEngine runtime and `FORBIDDEN` rejects
`Qt6WebEngine*`, `QtWebEngineProcess.exe` and WebEngine resources. Qt Quick, which
only WebEngine pulled in, leaves the bundle with it. Native UI was already
unavailable from Windows.

### macOS

The pinned official Qt (`packaging/macos/qt.json`) drops `qtwebengine`,
`qtwebchannel`, `qtpositioning` and `qtdeclarative`. Signing no longer handles
`QtWebEngineProcess.app`, and its entitlements file is removed. The bundle notices
list only shipped modules. Homebrew dependency lists drop `qtwebengine`. This lands
before the first signed macOS release.

## Testing

- `test_nativeuiview`: no WebEngine. A fake factory checks browser keys and reuse,
  restart of a reused port, Reload, invalid and late connections, offline state
  and the renderer-stopped message. Runs on every platform.
- `test_nativeuiplugin` (Linux, plugin built): loads the real plugin through the
  loader, checks the IID, authentication through the launch URL, the preserved
  native draft, origin restriction, disabled file access and the optional released
  harness check (`HGS_NATIVE_UI_URL_FILE`).
- Loader: missing file, wrong IID and an unloadable file yield no factory with a
  reason; only the fixed paths are searched.
- `test_sessionswindow`: with a fake factory the routes switch to the tab; without
  one the tab is hidden and the routes request the browser handoff.
- Packaging suites: release and nightly validation (current and legacy library
  sets), Windows required/forbidden files, CI Qt modules and cache key, macOS Qt
  pin, bundle notices, signing and the bundle check that rejects any Qt WebEngine
  file, and `tray/install.sh` placing, replacing and removing the local plugin.
- Local verification: Linux builds with and without the plugin and the Arch package
  with namcap; on a Mac, an isolated build and suites with Homebrew Qt and the
  unsigned bundle with the pinned Qt and `check-macos-bundle.py --selftest`.
  Windows is verified by hosted CI.
