#pragma once
#include <QtPlugin>
#include <functional>

class QColor;
class QString;
class QUrl;
class QWidget;

// An isolated browser for the official DeepSeek web workspace. The optional
// Linux plugin (plugins/NativeUiPlugin.cpp) implements it with Qt WebEngine,
// so hgs-tray itself never links Chromium. Callbacks instead of signals: the
// plugin has no access to the meta-objects compiled into the GUI.
class NativeBrowser {
public:
    virtual ~NativeBrowser() = default;
    // The view, owned by this browser. Destroying the browser destroys it.
    virtual QWidget *widget() = 0;
    virtual void load(const QUrl &url) = 0;
    std::function<void(bool ok)> loadFinished;
    std::function<void()> rendererStopped;
};

class NativeBrowserFactory {
public:
    virtual ~NativeBrowserFactory() = default;
    // A browser with its own in-memory profile whose main frame stays on origin.
    virtual NativeBrowser *create(const QString &origin, const QColor &background, QWidget *parent) = 0;
};

// Any change to the two classes above changes this IID: a plugin built for
// another interface is then rejected instead of called.
#define NativeBrowserFactory_iid "dev.zerus.hgs-tray.NativeBrowserFactory/1"
Q_DECLARE_INTERFACE(NativeBrowserFactory, NativeBrowserFactory_iid)
