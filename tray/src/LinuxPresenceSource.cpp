#include "LinuxPresenceSource.h"

#include <KIdleTime>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <memory>

namespace {
std::optional<bool> boolean(const QDBusMessage &reply)
{
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1) return std::nullopt;
    const QVariant value = reply.arguments().constFirst();
    if (value.metaType().id() != QMetaType::Bool) return std::nullopt;
    return value.toBool();
}

std::optional<double> number(const QDBusMessage &reply, double perSecond)
{
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1) return std::nullopt;
    const QVariant value = reply.arguments().constFirst();
    const int type = value.metaType().id();
    if (type != QMetaType::UInt && type != QMetaType::ULongLong && type != QMetaType::Int && type != QMetaType::LongLong)
        return std::nullopt;
    bool ok = false;
    const double result = value.toULongLong(&ok) / perSecond;
    if (!ok) return std::nullopt;
    return result;
}
}

LinuxPresenceSource::LinuxPresenceSource(QDBusConnection bus, std::function<qint64()> clock, QObject *parent)
    : PresenceSource(parent), m_bus(std::move(bus)), m_clock(std::move(clock))
{
    if (!m_clock) { m_monotonic.start(); m_clock = [this] { return m_monotonic.elapsed(); }; }
}

void LinuxPresenceSource::sample()
{
    struct Replies {
        int remaining = 4;
        std::optional<bool> freedesktopLock, gnomeLock;
        std::optional<double> freedesktopIdle, mutterIdle;
    };
    auto replies = std::make_shared<Replies>();
    const auto finished = [this, replies] {
        if (--replies->remaining) return;
        PresenceSample result;
        result.locked = replies->freedesktopLock.value_or(false) || replies->gnomeLock.value_or(false);
        auto idle = replies->freedesktopIdle ? replies->freedesktopIdle : replies->mutterIdle;
        if (!idle) idle = thresholdIdle();
        if (idle) { result.available = true; result.idleSeconds = *idle; }
        emit sampled(result);
    };
    const QString screenSaver = QStringLiteral("org.freedesktop.ScreenSaver");
    const QString screenSaverPath = QStringLiteral("/org/freedesktop/ScreenSaver");
    call(screenSaver, screenSaverPath, screenSaver, QStringLiteral("GetActive"),
         [replies, finished](const QDBusMessage &reply) { replies->freedesktopLock = boolean(reply); finished(); });
    call(screenSaver, screenSaverPath, screenSaver, QStringLiteral("GetSessionIdleTime"),
         [replies, finished](const QDBusMessage &reply) { replies->freedesktopIdle = number(reply, 1); finished(); });
    call(QStringLiteral("org.gnome.Mutter.IdleMonitor"), QStringLiteral("/org/gnome/Mutter/IdleMonitor/Core"),
         QStringLiteral("org.gnome.Mutter.IdleMonitor"), QStringLiteral("GetIdletime"),
         [replies, finished](const QDBusMessage &reply) { replies->mutterIdle = number(reply, 1000); finished(); });
    call(QStringLiteral("org.gnome.ScreenSaver"), QStringLiteral("/org/gnome/ScreenSaver"),
         QStringLiteral("org.gnome.ScreenSaver"), QStringLiteral("GetActive"),
         [replies, finished](const QDBusMessage &reply) { replies->gnomeLock = boolean(reply); finished(); });
}

void LinuxPresenceSource::call(const QString &service, const QString &path, const QString &interface, const QString &method,
                               std::function<void(const QDBusMessage &)> done)
{
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setAutoStartService(false);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, TimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, done = std::move(done)] {
        done(watcher->reply());
        watcher->deleteLater();
    });
}

void LinuxPresenceSource::thresholdReached()
{
    m_thresholdsVerified = true;
    m_idleSince = m_clock() - ThresholdMs;
}

void LinuxPresenceSource::resumed()
{
    m_thresholdsVerified = true;
    m_idleSince = -1;
}

std::optional<double> LinuxPresenceSource::thresholdIdle() const
{
    if (!m_thresholdsVerified) return std::nullopt;
    // Between events the last input is less than ThresholdMs old. Zero delays
    // "away" by at most that much; it never causes a missed phone alert.
    return m_idleSince < 0 ? 0.0 : (m_clock() - m_idleSince) / 1000.0;
}

std::unique_ptr<PresenceSource> makePresenceSource()
{
    auto source = std::make_unique<LinuxPresenceSource>(QDBusConnection::sessionBus());
    auto *idle = KIdleTime::instance();
    const int threshold = idle->addIdleTimeout(LinuxPresenceSource::ThresholdMs);
    auto *raw = source.get();
    QObject::connect(idle, &KIdleTime::timeoutReached, raw, [raw, threshold](int identifier, int) {
        if (identifier != threshold) return;
        raw->thresholdReached();
        KIdleTime::instance()->catchNextResumeEvent();
    });
    QObject::connect(idle, &KIdleTime::resumingFromIdle, raw, [raw] { raw->resumed(); });
    return source;
}
