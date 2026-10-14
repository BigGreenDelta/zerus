#pragma once

#include "HgsClient.h"
#include "MachinePlatform.h"

#include <QJsonObject>
#include <QString>

#include <optional>

// The session host of this computer and the Windows session it runs in (spec D5 of
// the Windows native host design). A host started over SSH runs in another Windows
// session than the desktop, so agents it starts cannot show windows there, such as a
// browser sign-in. The GUI asks before a launch on this computer goes into such a
// host (LocalLaunchWarning.h).
namespace LocalSessionHost {
// The Windows session this GUI runs in (ProcessIdToSessionId); none on Linux and
// macOS, or when Windows does not answer.
std::optional<qint64> ownWindowsSession();
// Tests only: this GUI runs in that Windows session, or in none (std::nullopt),
// until resetOwnWindowsSessionForTests().
void setOwnWindowsSessionForTests(std::optional<qint64> session);
void resetOwnWindowsSessionForTests();

// The Windows session of this computer's session host when a launch on host goes
// into a host that runs in another Windows session than own; none when it does not:
// a remote machine, a local machine that is not Windows (local.metrics.platform),
// no running host (the launch starts one in this session), a host that reports no
// session, no own session (the GUI is not on Windows) or the same session.
inline std::optional<qint64> otherWindowsSession(const BoxState &local, const QString &host,
                                                 std::optional<qint64> own = ownWindowsSession())
{
    if (!host.isEmpty() || !own
        || MachinePlatform::of(local.metrics, true) != MachinePlatform::Style::Windows
        || local.localHost.value(QLatin1String("state")).toString() != QLatin1String("running"))
        return std::nullopt;
    const auto session = local.localHost.value(QLatin1String("windows_session"));
    if (!session.isDouble()) return std::nullopt;
    const qint64 id = session.toInteger(-1);
    if (id < 0 || id == *own) return std::nullopt;
    return id;
}
} // namespace LocalSessionHost
