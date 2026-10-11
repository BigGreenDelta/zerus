#include "DesktopPresence.h"

#include <QCoreApplication>
#include <QEvent>
#include <cmath>
#include <utility>

DesktopPresence::DesktopPresence(std::unique_ptr<PresenceSource> source, Writer writer, Clock clock, QObject *parent)
    : QObject(parent), m_source(std::move(source)), m_writer(std::move(writer)), m_clock(std::move(clock))
{
    if (!m_clock) { m_monotonic.start(); m_clock = [this] { return m_monotonic.elapsed(); }; }
    m_started = m_clock();
    if (m_source) connect(m_source.get(), &PresenceSource::sampled, this, &DesktopPresence::sampled);
    connect(&m_sampleTimer, &QTimer::timeout, this, [this] { poll(true); });
    connect(&m_checkTimer, &QTimer::timeout, this, [this] { poll(false); });
    if (auto *app = QCoreApplication::instance()) app->installEventFilter(this);
}

void DesktopPresence::start(int sampleMs, int checkMs)
{
    m_sampleTimer.start(sampleMs);
    m_checkTimer.start(checkMs);
    poll(true);
}

void DesktopPresence::poll(bool periodic)
{
    m_periodic = m_periodic || periodic;
    const qint64 now = m_clock();
    // One sample at a time; a source that never answers is asked again after 10 s.
    if (m_requestedAt >= 0 && now - m_requestedAt < 10000) return;
    m_requestedAt = now;
    if (m_source) m_source->sample(); else sampled({});
}

void DesktopPresence::sampled(const PresenceSample &sample)
{
    if (m_requestedAt < 0) return;
    m_requestedAt = -1;
    const qint64 now = m_clock();
    double idle = sample.idleSeconds;
    // A negative or non-finite idle time is no evidence of input.
    if (!sample.available || !std::isfinite(idle) || idle < 0) {
        // Only Zerus windows count. Before any input here the owner counts as
        // away: an extra phone alert is better than a missed one.
        idle = m_lastInput < 0 ? ActiveSeconds + (now - m_started) / 1000.0 : (now - m_lastInput) / 1000.0;
    }
    idle = qMax(0.0, idle);
    const bool periodic = std::exchange(m_periodic, false);
    if (!periodic && m_writtenAt >= 0
        && !transition(m_writtenIdle, m_writtenLocked, (now - m_writtenAt) / 1000.0, idle, sample.locked))
        return;
    m_writtenAt = now; m_writtenIdle = idle; m_writtenLocked = sample.locked;
    m_writer(idle, sample.locked);
}

bool DesktopPresence::transition(double writtenIdle, bool writtenLocked, double elapsedSeconds, double idle, bool locked)
{
    if (locked != writtenLocked) return true;
    return !locked && writtenIdle + elapsedSeconds >= ActiveSeconds && idle < ActiveSeconds;
}

void DesktopPresence::noteInput()
{
    const qint64 now = m_clock();
    const bool returning = m_lastInput < 0 || now - m_lastInput >= qint64(ActiveSeconds * 1000);
    m_lastInput = now;
    if (returning) poll(false);
}

bool DesktopPresence::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress: case QEvent::MouseButtonPress: case QEvent::MouseMove:
    case QEvent::Wheel: case QEvent::TouchBegin:
        noteInput();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}
