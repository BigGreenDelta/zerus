#include <QtTest>
#include "DesktopPresence.h"

class FakeSource : public PresenceSource {
public:
    PresenceSample next{true, 0, false};
    bool immediate = true;
    int requests = 0;
    void sample() override { ++requests; if (immediate) emit sampled(next); }
    void answer() { emit sampled(next); }
};

struct Write { double idle; bool locked; };

class TestDesktopPresence : public QObject {
    Q_OBJECT
private slots:
    void transitionsWriteOnlyLockChangesAndReturns();
    void periodicSamplesAlwaysWriteAndChecksWriteOnlyTransitions();
    void slowSourcesAreNotQueuedTwice();
    void withoutPlatformIdleOnlyZerusInputCounts();
};

void TestDesktopPresence::transitionsWriteOnlyLockChangesAndReturns()
{
    QVERIFY(DesktopPresence::transition(10, false, 0, 10, true));
    QVERIFY(DesktopPresence::transition(10, true, 0, 10, false));
    QVERIFY(DesktopPresence::transition(150, false, 0, 3, false));
    QVERIFY(DesktopPresence::transition(30, false, 100, 2, false));
    QVERIFY(!DesktopPresence::transition(30, false, 20, 2, false));
    QVERIFY(!DesktopPresence::transition(10, false, 200, 210, false));
    QVERIFY(!DesktopPresence::transition(300, true, 0, 1, true));
}

void TestDesktopPresence::periodicSamplesAlwaysWriteAndChecksWriteOnlyTransitions()
{
    qint64 now = 0; QList<Write> writes;
    auto *fake = new FakeSource;
    DesktopPresence presence(std::unique_ptr<PresenceSource>(fake),
        [&writes](double idle, bool locked) { writes.append({idle, locked}); }, [&now] { return now; });
    presence.start(3600000, 3600000);
    QCOMPARE(writes.size(), 1); QCOMPARE(writes[0].idle, 0.0);
    fake->next = {true, 4, false}; now = 5000; presence.poll(false); QCOMPARE(writes.size(), 1);
    fake->next = {true, 4, true}; presence.poll(false); QCOMPARE(writes.size(), 2); QVERIFY(writes[1].locked);
    fake->next = {true, 35, true}; now = 35000; presence.poll(true); QCOMPARE(writes.size(), 3);
    fake->next = {true, 1, false}; presence.poll(false); QCOMPARE(writes.size(), 4); QVERIFY(!writes[3].locked);
    fake->next = {true, 200, false}; now = 300000; presence.poll(false); QCOMPARE(writes.size(), 4);
    fake->next = {true, 2, false}; presence.poll(false); QCOMPARE(writes.size(), 5); QCOMPARE(writes[4].idle, 2.0);
}

void TestDesktopPresence::slowSourcesAreNotQueuedTwice()
{
    qint64 now = 0; QList<Write> writes;
    auto *fake = new FakeSource; fake->immediate = false;
    DesktopPresence presence(std::unique_ptr<PresenceSource>(fake),
        [&writes](double idle, bool locked) { writes.append({idle, locked}); }, [&now] { return now; });
    presence.poll(true); presence.poll(false); presence.poll(true);
    QCOMPARE(fake->requests, 1);
    fake->answer(); QCOMPARE(writes.size(), 1);
    fake->answer(); QCOMPARE(writes.size(), 1); // an unsolicited reply is ignored
    now = 1000; presence.poll(false); presence.poll(false); QCOMPARE(fake->requests, 2);
    now = 12000; presence.poll(false); QCOMPARE(fake->requests, 3); // a hung source is asked again
}

void TestDesktopPresence::withoutPlatformIdleOnlyZerusInputCounts()
{
    qint64 now = 0; QList<Write> writes; QObject target;
    DesktopPresence presence(nullptr,
        [&writes](double idle, bool locked) { writes.append({idle, locked}); }, [&now] { return now; });
    now = 30000; presence.poll(true);
    QCOMPARE(writes.last().idle, 150.0); // no input here yet: away
    now = 40000; presence.noteInput();
    QCOMPARE(writes.size(), 2); QCOMPARE(writes.last().idle, 0.0);
    now = 50000; presence.noteInput(); QCOMPARE(writes.size(), 2);
    now = 60000; QEvent key(QEvent::KeyPress); QCoreApplication::sendEvent(&target, &key);
    now = 400000; presence.poll(true); QCOMPARE(writes.last().idle, 340.0);
}

QTEST_GUILESS_MAIN(TestDesktopPresence)
#include "test_desktoppresence.moc"
