#include "LinuxPresenceSource.h"

#include <KIdleTime>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QGuiApplication>
#include <memory>

namespace {
std::optional<bool> boolean(const QDBusMessage &reply)
{
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1) return std::nullopt;
    const QVariant value = reply.arguments().constFirst();
    if (value.metaType().id() != QMetaType::Bool) return std::nullopt;
    return value.toBool();
}

std::optional<qint64> number(const QDBusMessage &reply)
{
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1) return std::nullopt;
    const QVariant value = reply.arguments().constFirst();
    const int type = value.metaType().id();
    if (type != QMetaType::UInt && type != QMetaType::ULongLong && type != QMetaType::Int && type != QMetaType::LongLong)
        return std::nullopt;
    bool ok = false;
    const qint64 result = value.toLongLong(&ok);
    if (!ok || result < 0) return std::nullopt;
    return result;
}
}

LinuxPresenceSource::LinuxPresenceSource(QDBusConnection bus, std::function<qint64()> clock, IdleReader localIdle,
                                         QObject *parent)
    : PresenceSource(parent), m_bus(std::move(bus)), m_clock(std::move(clock)), m_localIdle(std::move(localIdle))
{
    if (!m_clock) { m_monotonic.start(); m_clock = [this] { return m_monotonic.elapsed(); }; }
}

void LinuxPresenceSource::sample()
{
    struct Replies {
        int remaining = 0;
        std::optional<bool> freedesktopLock, gnomeLock, kdeScreenSaver;
        std::optional<qint64> freedesktopIdle, mutterIdleMs;
    };
    auto replies = std::make_shared<Replies>();
    std::optional<qint64> localIdleMs = m_localIdle ? m_localIdle() : std::nullopt;
    // KIdleTime's X11 idle time wraps negative after about 24.8 days; ask D-Bus instead.
    if (localIdleMs && *localIdleMs < 0) localIdleMs.reset();
    replies->remaining = localIdleMs ? 3 : 5;
    const auto finished = [this, replies, localIdleMs] {
        if (--replies->remaining) return;
        PresenceSample result;
        result.locked = replies->freedesktopLock.value_or(false) || replies->gnomeLock.value_or(false);
        std::optional<double> idle;
        if (localIdleMs) idle = *localIdleMs / 1000.0;
        else if (replies->freedesktopIdle)
            idle = *replies->freedesktopIdle / (replies->kdeScreenSaver.value_or(false) ? 1000.0 : 1.0);
        else if (replies->mutterIdleMs) idle = *replies->mutterIdleMs / 1000.0;
        else idle = thresholdIdle();
        if (idle) { result.available = true; result.idleSeconds = *idle; }
        emit sampled(result);
    };
    const QString screenSaver = QStringLiteral("org.freedesktop.ScreenSaver");
    const QString screenSaverPath = QStringLiteral("/org/freedesktop/ScreenSaver");
    call(screenSaver, screenSaverPath, screenSaver, QStringLiteral("GetActive"),
         [replies, finished](const QDBusMessage &reply) { replies->freedesktopLock = boolean(reply); finished(); });
    call(QStringLiteral("org.gnome.Mutter.IdleMonitor"), QStringLiteral("/org/gnome/Mutter/IdleMonitor/Core"),
         QStringLiteral("org.gnome.Mutter.IdleMonitor"), QStringLiteral("GetIdletime"),
         [replies, finished](const QDBusMessage &reply) { replies->mutterIdleMs = number(reply); finished(); });
    call(QStringLiteral("org.gnome.ScreenSaver"), QStringLiteral("/org/gnome/ScreenSaver"),
         QStringLiteral("org.gnome.ScreenSaver"), QStringLiteral("GetActive"),
         [replies, finished](const QDBusMessage &reply) { replies->gnomeLock = boolean(reply); finished(); });
    if (localIdleMs) return;
    call(screenSaver, screenSaverPath, screenSaver, QStringLiteral("GetSessionIdleTime"),
         [replies, finished](const QDBusMessage &reply) { replies->freedesktopIdle = number(reply); finished(); });
    const QString dbus = QStringLiteral("org.freedesktop.DBus");
    call(dbus, QStringLiteral("/org/freedesktop/DBus"), dbus, QStringLiteral("NameHasOwner"),
         [replies, finished](const QDBusMessage &reply) { replies->kdeScreenSaver = boolean(reply); finished(); },
         {QStringLiteral("org.kde.screensaver")});
}

void LinuxPresenceSource::call(const QString &service, const QString &path, const QString &interface, const QString &method,
                               std::function<void(const QDBusMessage &)> done, const QVariantList &arguments)
{
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(arguments);
    message.setAutoStartService(false);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, TimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, done = std::move(done)] {
        done(watcher->reply());
        watcher->deleteLater();
    });
}

void LinuxPresenceSource::thresholdReached()
{
    m_lastThresholdEvent = m_clock();
    m_idleSince = *m_lastThresholdEvent - ThresholdMs;
}

void LinuxPresenceSource::resumed()
{
    m_lastThresholdEvent = m_clock();
    m_idleSince.reset();
}

std::optional<double> LinuxPresenceSource::thresholdIdle() const
{
    if (!m_lastThresholdEvent || m_clock() - *m_lastThresholdEvent >= ThresholdStaleMs) return std::nullopt;
    // Between events the last input is less than ThresholdMs old. Zero delays
    // "away" by at most that much; it never causes a missed phone alert.
    return m_idleSince ? (m_clock() - *m_idleSince) / 1000.0 : 0.0;
}

std::unique_ptr<PresenceSource> makePresenceSource()
{
    auto *idle = KIdleTime::instance();
    // 0: KIdleTime has no plugin for this platform, and its idleTime() is a constant 0.
    const int threshold = idle->addIdleTimeout(LinuxPresenceSource::ThresholdMs);
    LinuxPresenceSource::IdleReader local;
    // The X server tracks the idle time; Wayland compositors only signal thresholds.
    if (threshold && QGuiApplication::platformName() == QLatin1String("xcb"))
        local = [] { return std::optional<qint64>(KIdleTime::instance()->idleTime()); };
    auto source = std::make_unique<LinuxPresenceSource>(QDBusConnection::sessionBus(), std::function<qint64()>{}, std::move(local));
    if (!threshold) return source;
    auto *raw = source.get();
    QObject::connect(idle, &KIdleTime::timeoutReached, raw, [raw, threshold](int identifier, int) {
        if (identifier != threshold) return;
        raw->thresholdReached();
        KIdleTime::instance()->catchNextResumeEvent();
    });
    QObject::connect(idle, &KIdleTime::resumingFromIdle, raw, [raw] { raw->resumed(); });
    QObject::connect(raw, &QObject::destroyed, idle, [threshold] { KIdleTime::instance()->removeIdleTimeout(threshold); });
    return source;
}
