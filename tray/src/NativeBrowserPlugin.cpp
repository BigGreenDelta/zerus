#include "NativeBrowserPlugin.h"
#include "NativeBrowser.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPluginLoader>
#include <QtGlobal>

namespace {
const char *const kFile = "libhgs-tray-native-ui.so";
struct State { bool searched = false; NativeBrowserFactory *factory = nullptr; QString reason; NativeBrowserFactory *testing = nullptr; bool replaced = false; };
State &state() { static State value; return value; }
}

namespace NativeBrowserPlugin {

QStringList candidates()
{
#ifdef Q_OS_LINUX
    const QDir bin(QCoreApplication::applicationDirPath());
    return {QDir::cleanPath(bin.filePath(QStringLiteral("../lib/hgs-tray/plugins/")) + '/' + kFile),
            bin.filePath(QStringLiteral("plugins/")) + kFile};
#else
    return {};
#endif
}

NativeBrowserFactory *load(const QString &path, QString *reason)
{
    if (!QFileInfo(path).isFile()) { if (reason) *reason = QStringLiteral("%1 is not installed").arg(path); return nullptr; }
    // Never unloaded: browsers and their views live as long as the window.
    auto *loader = new QPluginLoader(path);
    QObject *instance = loader->instance();
    if (!instance) {
        if (reason) *reason = loader->errorString();
        delete loader; return nullptr;
    }
    auto *factory = qobject_cast<NativeBrowserFactory *>(instance);
    if (!factory && reason) *reason = QStringLiteral("%1 does not implement %2").arg(path, QLatin1String(NativeBrowserFactory_iid));
    return factory;
}

NativeBrowserFactory *factory(QString *reason)
{
    auto &s = state();
    if (s.replaced) { if (reason) reason->clear(); return s.testing; }
    if (!s.searched) {
        s.searched = true;
        const auto paths = candidates();
        QStringList problems;
        for (const auto &path : paths) {
            QString problem;
            if ((s.factory = load(path, &problem))) break;
            // A missing build-tree copy says nothing about an installed one.
            if (QFileInfo(path).isFile()) problems << problem;
        }
        if (!s.factory) {
            s.reason = problems.isEmpty() ? QStringLiteral("the embedded native UI plugin is not installed") : problems.join(QStringLiteral("; "));
            if (!problems.isEmpty()) qWarning("Native UI opens in the browser: %s", qPrintable(s.reason));
        }
    }
    if (reason) *reason = s.reason;
    return s.factory;
}

void setFactoryForTesting(NativeBrowserFactory *factory)
{
    auto &s = state(); s.testing = factory; s.replaced = factory != nullptr;
}

}
