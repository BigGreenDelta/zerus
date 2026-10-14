#include "LocalSessionHost.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace LocalSessionHost {
namespace {
bool g_testOverride = false;
std::optional<qint64> g_testSession;
}

std::optional<qint64> ownWindowsSession()
{
    if (g_testOverride) return g_testSession;
#ifdef Q_OS_WIN
    // A process never changes its Windows session.
    static const std::optional<qint64> session = []() -> std::optional<qint64> {
        DWORD id = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &id)) return std::nullopt;
        return qint64(id);
    }();
    return session;
#else
    return std::nullopt;
#endif
}

void setOwnWindowsSessionForTests(std::optional<qint64> session)
{
    g_testOverride = true;
    g_testSession = session;
}

void resetOwnWindowsSessionForTests()
{
    g_testOverride = false;
    g_testSession.reset();
}
} // namespace LocalSessionHost
