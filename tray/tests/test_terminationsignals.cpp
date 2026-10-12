#include <QtTest>

#include <csignal>

#include "TerminationSignals.h"

// Signals reach the callback from the event loop, never from the async handler,
// and the process keeps the default action for a repeated signal.

namespace {
struct sigaction actionFor(int signal)
{
    struct sigaction current = {};
    ::sigaction(signal, nullptr, &current);
    return current;
}

int customCalls = 0;
void custom(int) { ++customCalls; }
} // namespace

class TestTerminationSignals : public QObject {
    Q_OBJECT
private slots:
    void quitsTheEventLoopAndRunsAboutToQuit();
    void eachSignalArrivesOnceThenKeepsItsDefaultAction();
    void ignoredSignalsStayIgnored();
    void restoresPreviousActionsWhenDestroyed();
};

void TestTerminationSignals::quitsTheEventLoopAndRunsAboutToQuit()
{
    // The same wiring as main.cpp: the menu's quit, so aboutToQuit flushes drafts.
    bool flushed = false;
    const auto connection = connect(qApp, &QCoreApplication::aboutToQuit, this, [&flushed] { flushed = true; });
    TerminationSignals termination([](int) { QCoreApplication::quit(); });
    QVERIFY(termination.handles(SIGTERM));
    QTimer::singleShot(0, [] { std::raise(SIGTERM); });
    QTimer::singleShot(5000, [] { QCoreApplication::exit(1); });
    QCOMPARE(QCoreApplication::exec(), 0);
    QVERIFY(flushed);
    disconnect(connection);
}

void TestTerminationSignals::eachSignalArrivesOnceThenKeepsItsDefaultAction()
{
    QList<int> received;
    TerminationSignals termination([&received](int signal) { received << signal; });
    for (int signal : TerminationSignals::kSignals)
        QVERIFY(termination.handles(signal));
    std::raise(SIGINT);
    std::raise(SIGHUP);
    // Nothing runs inside the async handler; the event loop delivers both.
    QCOMPARE(received.size(), 0);
    QTRY_COMPARE(received, QList<int>({SIGINT, SIGHUP}));
    QVERIFY(!termination.handles(SIGINT));
    QVERIFY(actionFor(SIGINT).sa_handler == SIG_DFL);
    QVERIFY(actionFor(SIGHUP).sa_handler == SIG_DFL);
    QVERIFY(termination.handles(SIGTERM));
}

void TestTerminationSignals::ignoredSignalsStayIgnored()
{
    const auto previous = std::signal(SIGHUP, SIG_IGN);
    {
        TerminationSignals termination([](int) {});
        QVERIFY(!termination.handles(SIGHUP));
        QVERIFY(actionFor(SIGHUP).sa_handler == SIG_IGN);
        QVERIFY(termination.handles(SIGTERM));
    }
    QVERIFY(actionFor(SIGHUP).sa_handler == SIG_IGN);
    std::signal(SIGHUP, previous);
}

void TestTerminationSignals::restoresPreviousActionsWhenDestroyed()
{
    const auto previous = std::signal(SIGINT, custom);
    {
        TerminationSignals termination([](int) {});
        QVERIFY(actionFor(SIGINT).sa_handler != custom);
    }
    QVERIFY(actionFor(SIGINT).sa_handler == custom);
    std::raise(SIGINT);
    QCOMPARE(customCalls, 1);
    std::signal(SIGINT, previous);
}

QTEST_GUILESS_MAIN(TestTerminationSignals)
#include "test_terminationsignals.moc"
