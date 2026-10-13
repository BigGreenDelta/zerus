#pragma once
#include <QString>
#include <QStringList>

class NativeBrowserFactory;

// The optional embedded native UI (NativeBrowser.h). Linux packages ship it as
// libhgs-tray-native-ui.so beside the GUI; other builds open the native UI in
// the system browser instead.
namespace NativeBrowserPlugin {
// The factory, loaded once per process on first use; nullptr when the plugin
// is absent or unusable. reason then says why (it never contains a URL).
NativeBrowserFactory *factory(QString *reason = nullptr);
// Where factory() looks, in order: fixed paths relative to the executable,
// never configurable through the environment. Empty outside Linux.
QStringList candidates();
// Loads one plugin file; nullptr with reason when it is missing, cannot be
// loaded or implements another interface version.
NativeBrowserFactory *load(const QString &path, QString *reason);
// Tests replace the loaded factory; nullptr restores the plugin search.
void setFactoryForTesting(NativeBrowserFactory *factory);
}
