#pragma once

#include "DesktopPresence.h"

#include <QDBusConnection>
#include <QElapsedTimer>
#include <QVariantList>
#include <functional>
#include <optional>

class QDBusMessage;

// On X11 the idle time is read in-process (KIdleTime). Otherwise it comes from
// org.freedesktop.ScreenSaver, whose KDE implementation answers in milliseconds
// instead of seconds, or from Mutter on GNOME. The ScreenSaver services report
// the lock everywhere. KDE Plasma on Wayland refuses GetSessionIdleTime, so
// compositor idle thresholds (KIdleTime) fill the gap, but only after they
// delivered an event: a missing plugin must not look like constant activity.
// Thresholds that stop arriving (a compositor restart, an idle inhibitor) are
// not trusted either. Calls are asynchronous and never start services.
class LinuxPresenceSource : public PresenceSource {
    Q_OBJECT
public:
    static constexpr int ThresholdMs = 10000;
    static constexpr int TimeoutMs = 2000;
    // Without any threshold event for this long the thresholds are unavailable
    // until the next one.
    static constexpr qint64 ThresholdStaleMs = 15 * 60 * 1000;
    // Milliseconds since the last input, or nullopt when this process cannot tell.
    using IdleReader = std::function<std::optional<qint64>()>;
    explicit LinuxPresenceSource(QDBusConnection bus, std::function<qint64()> clock = {}, IdleReader localIdle = {},
                                 QObject *parent = nullptr);
    void sample() override;
    // Input stopped for ThresholdMs / input resumed (KIdleTime).
    void thresholdReached();
    void resumed();
private:
    void call(const QString &service, const QString &path, const QString &interface, const QString &method,
              std::function<void(const QDBusMessage &)> done, const QVariantList &arguments = {});
    std::optional<double> thresholdIdle() const;
    QDBusConnection m_bus;
    std::function<qint64()> m_clock;
    IdleReader m_localIdle;
    QElapsedTimer m_monotonic;
    std::optional<qint64> m_lastThresholdEvent; // nullopt: the thresholds are unverified
    std::optional<qint64> m_idleSince; // nullopt: input since the last threshold
};
