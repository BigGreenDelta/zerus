// Compiled into Windows test programs that use Qt GUI (tests/CMakeLists.txt). The
// offscreen platform reads the font files in QT_QPA_FONTDIR (set by CTest) but has
// no Windows font configuration: its default "Sans Serif" falls back to an
// arbitrary family. Tests use the Windows UI font, as the windows platform does.
#include <QCoreApplication>
#include <QFont>
#include <QGuiApplication>

namespace {
void useWindowsUiFont()
{
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        QGuiApplication::setFont(QFont(QStringLiteral("Segoe UI"), 9));
}
} // namespace

Q_COREAPP_STARTUP_FUNCTION(useWindowsUiFont)
