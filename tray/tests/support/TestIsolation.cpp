// Compiled into every Windows test program (tests/CMakeLists.txt). The native
// QSettings format there is the registry, so tests that use default settings
// would read and write the developer's HKEY_CURRENT_USER. Like hgs-tray on
// Windows they use INI files, here in a directory of their own. Tests that set a
// path themselves still do so later, in initTestCase(). The program also names
// the fixture worker pool its fake executables share (support/fixture-worker.py).
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

namespace {
// QSettings creates the directory on its first write, and it is removed when the
// program ends normally. A test child that is killed on purpose before it writes
// (test_messagecomposer's crash child) leaves nothing behind.
struct SettingsDirectory {
    QString path;
    ~SettingsDirectory() { if (!path.isEmpty()) QDir(path).removeRecursively(); }
};

void isolateSettings()
{
    static SettingsDirectory directory;
    directory.path = QDir::tempPath() + '/' + QCoreApplication::applicationName() + '-'
        + QString::number(QCoreApplication::applicationPid()) + '-' + QUuid::createUuid().toString(QUuid::Id128).left(8);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory.path);
    QStandardPaths::setTestModeEnabled(true);
    // ZERUS_FIXTURE_POOL=0 starts every fixture in a Python process of its own.
    if (qgetenv("ZERUS_FIXTURE_POOL") != "0")
        qputenv("ZERUS_FIXTURE_POOL", QByteArray::number(QCoreApplication::applicationPid()));
}
} // namespace

Q_COREAPP_STARTUP_FUNCTION(isolateSettings)
