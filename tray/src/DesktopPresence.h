#pragma once

#include <QElapsedTimer>
#include <QMetaType>
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>

struct PresenceSample {
    bool available = false; // false: the platform reports no idle time
    double idleSeconds = 0;
    bool locked = false;
};
Q_DECLARE_METATYPE(PresenceSample)

// Reads OS input idle time and screen lock. Every sample() emits sampled()
// once, possibly later: D-Bus replies are asynchronous.
class PresenceSource : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void sample() = 0;
signals:
    void sampled(const PresenceSample &sample);
};

// Defined per platform (LinuxPresenceSource.cpp, MacPresenceSource.cpp).
std::unique_ptr<PresenceSource> makePresenceSource();

// Records whether the owner uses this desktop, for phone alert decisions. The
// tray agent runs it with the window closed. It writes every 30 s and at once
// when the screen locks or unlocks or the owner returns after being away;
// listings age the last value, so going idle needs no write.
class DesktopPresence : public QObject {
    Q_OBJECT
public:
    static constexpr int SampleMs = 30000;
    static constexpr int CheckMs = 5000;
    static constexpr double ActiveSeconds = 120;
    using Writer = std::function<void(double idleSeconds, bool locked)>;
    using Clock = std::function<qint64()>;
    DesktopPresence(std::unique_ptr<PresenceSource> source, Writer writer, Clock clock = {}, QObject *parent = nullptr);
    void start(int sampleMs = SampleMs, int checkMs = CheckMs);
    // Timers call this; tests drive it directly. Periodic samples always write.
    void poll(bool periodic);
    // Input in a Zerus window: the only activity signal when the platform has none.
    void noteInput();
    static bool transition(double writtenIdle, bool writtenLocked, double elapsedSeconds, double idle, bool locked);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void sampled(const PresenceSample &sample);
    std::unique_ptr<PresenceSource> m_source;
    Writer m_writer;
    Clock m_clock;
    QElapsedTimer m_monotonic;
    QTimer m_sampleTimer, m_checkTimer;
    qint64 m_started = 0, m_lastInput = -1, m_requestedAt = -1, m_writtenAt = -1;
    double m_writtenIdle = 0;
    bool m_writtenLocked = false, m_periodic = false;
};
