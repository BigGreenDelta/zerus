#pragma once

#include "DesktopPresence.h"

#include <QDBusConnection>
#include <QElapsedTimer>
#include <functional>
#include <optional>

class QDBusMessage;

// org.freedesktop.ScreenSaver reports the lock everywhere and idle time on X11;
// GNOME reports idle time through Mutter. KDE Plasma on Wayland refuses
// GetSessionIdleTime, so compositor idle thresholds (KIdleTime) fill the gap,
// but only after they delivered an event: a missing plugin must not look like
// constant activity. Calls are asynchronous and never start services.
class LinuxPresenceSource : public PresenceSource {
    Q_OBJECT
public:
    static constexpr int ThresholdMs = 10000;
    static constexpr int TimeoutMs = 2000;
    explicit LinuxPresenceSource(QDBusConnection bus, std::function<qint64()> clock = {}, QObject *parent = nullptr);
    void sample() override;
    // Input stopped for ThresholdMs / input resumed (KIdleTime).
    void thresholdReached();
    void resumed();
private:
    void call(const QString &service, const QString &path, const QString &interface, const QString &method,
              std::function<void(const QDBusMessage &)> done);
    std::optional<double> thresholdIdle() const;
    QDBusConnection m_bus;
    std::function<qint64()> m_clock;
    QElapsedTimer m_monotonic;
    bool m_thresholdsVerified = false;
    qint64 m_idleSince = -1;
};
