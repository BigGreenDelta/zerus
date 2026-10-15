#include "SessionGraphStore.h"
#include <QJsonArray>
#include <QtTest>

class TestSessionGraphStore : public QObject {
    Q_OBJECT
    const QString machine = "11111111-1111-4111-8111-111111111111",
                  epoch = "22222222-2222-4222-8222-222222222222";
    QString id(char suffix) const { return "node:" + QString(64, QLatin1Char(suffix)); }
    QJsonObject row(char suffix, int rev, bool deleted = false) const {
        return {{"id", id(suffix)},
                {"revision", rev},
                {"deleted", deleted},
                {"item", QJsonObject{{"kind", "node"},
                                     {"value", QJsonObject{{"example", QString(suffix)}}}}}};
    }
    QJsonObject page(QJsonArray items, int revision, const QString &cursor,
                     bool more = false) const {
        return {{"schema", 1},
                {"machine_id", machine},
                {"epoch", epoch},
                {"revision", revision},
                {"cursor", cursor},
                {"has_more", more},
                {"reset_required", false},
                {"coverage", QJsonObject{{"bindings_complete", true}}},
                {"items", items}};
    }
  private slots:
    void snapshotOnlyReplacesAtConsistentLastPage() {
        SessionGraphStore store;
        QCOMPARE(store.apply("mac", page({row('a', 1)}, 1, "old"), false, {}, 1),
                 SessionGraphStore::Applied);
        QCOMPARE(store.apply("mac", page({row('b', 2)}, 3, "next", true), false, {}, 2),
                 SessionGraphStore::More);
        QVERIFY(store.host("mac")->items.contains(id('a')));
        QVERIFY(!store.host("mac")->items.contains(id('b')));
        QCOMPARE(store.apply("mac", page({row('c', 3)}, 4, "end"), false, "next", 3),
                 SessionGraphStore::Rejected);
        QCOMPARE(store.nextCursor("mac"), QString("next"));
        QCOMPARE(store.apply("mac", page({row('c', 3)}, 3, "end"), false, "next", 3),
                 SessionGraphStore::Applied);
        QVERIFY(!store.host("mac")->items.contains(id('a')));
        QCOMPARE(store.host("mac")->items.size(), 2);
    }
    void deltasRetainUnmentionedNodesAndRequireExactCursor() {
        SessionGraphStore store;
        QCOMPARE(store.apply({}, page({row('a', 1), row('b', 2)}, 2, "base"), false, {}, 1),
                 SessionGraphStore::Applied);
        QCOMPARE(store.apply({}, page({row('a', 3, true)}, 4, "mid", true), true, "base", 2),
                 SessionGraphStore::More);
        QVERIFY(!store.host({})->items.contains(id('a')));
        QVERIFY(store.host({})->items.contains(id('b')));
        QCOMPARE(store.apply({}, page({row('c', 4)}, 4, "done"), true, "base", 3),
                 SessionGraphStore::ResetRequired);
        QCOMPARE(store.apply({}, page({row('c', 4)}, 4, "done"), true, "mid", 3),
                 SessionGraphStore::Applied);
        QCOMPARE(store.host({})->items.size(), 2);
    }
    void malformedPageIsAtomic() {
        SessionGraphStore store;
        QCOMPARE(store.apply({}, page({row('a', 1)}, 1, "base"), false, {}, 1),
                 SessionGraphStore::Applied);
        const auto old = store.cache();
        auto bad = row('c', 3);
        bad["deleted"] = "false";
        QCOMPARE(store.apply({}, page({row('a', 2, true), bad}, 3, "next"), true, "base", 2),
                 SessionGraphStore::Rejected);
        QCOMPARE(store.cache(), old);
        QCOMPARE(store.apply({}, page({row('b', 2), row('b', 3)}, 3, "next"), true, "base", 2),
                 SessionGraphStore::Rejected);
        QCOMPARE(store.cache(), old);
        QCOMPARE(store.apply({}, page({row('b', 1)}, 1, "next"), true, "base", 2),
                 SessionGraphStore::Rejected);
        QCOMPARE(store.cache(), old);
    }
    void machineAndEpochChangesRequireNewSnapshot() {
        SessionGraphStore store;
        QCOMPARE(store.apply("alias", page({row('a', 1)}, 1, "old"), false, {}, 1),
                 SessionGraphStore::Applied);
        auto changed = page({row('b', 2)}, 2, "new");
        changed["machine_id"] = "33333333-3333-4333-8333-333333333333";
        QCOMPARE(store.apply("alias", changed, true, "old", 2), SessionGraphStore::ResetRequired);
        QVERIFY(store.host("alias")->items.contains(id('a')));
        QCOMPARE(store.apply("alias", changed, false, {}, 3), SessionGraphStore::Applied);
        QCOMPARE(store.host("alias")->machine, changed["machine_id"].toString());
        QVERIFY(!store.host("alias")->items.contains(id('a')));
        changed["epoch"] = "44444444-4444-4444-8444-444444444444";
        QCOMPARE(store.apply("alias", changed, true, "new", 4), SessionGraphStore::ResetRequired);
    }
    void resetAndFailureKeepLastGood() {
        SessionGraphStore store;
        QCOMPARE(store.apply({}, page({row('a', 1)}, 1, "old"), false, {}, 1),
                 SessionGraphStore::Applied);
        auto reset = page({}, 2, "new");
        reset["reset_required"] = true;
        QCOMPARE(store.apply({}, reset, true, "old", 2), SessionGraphStore::ResetRequired);
        QCOMPARE(store.host({})->cursor, QString("old"));
        QCOMPARE(store.apply({}, page({}, 2, "loop", true), false, {}, 2),
                 SessionGraphStore::Rejected);
        store.discardPending({});
        QVERIFY(store.host({}));
        store.forget({});
        QVERIFY(!store.host({}));
    }
    void cacheRoundTripAndRejectedCorruption() {
        SessionGraphStore store;
        QCOMPARE(store.apply("mac", page({row('a', 1)}, 1, "base"), false, {}, 1),
                 SessionGraphStore::Applied);
        SessionGraphStore restored;
        QVERIFY(restored.restore(store.cache()));
        QCOMPARE(restored.cache(), store.cache());
        auto bad = store.cache();
        bad["schema"] = 2;
        QVERIFY(!restored.restore(bad));
        QCOMPARE(restored.cache(), store.cache());
        auto hosts = bad["hosts"].toObject(), host = hosts["mac"].toObject();
        host["machine"] = "not-native-identity";
        hosts["mac"] = host;
        bad["hosts"] = hosts;
        bad["schema"] = 1;
        QVERIFY(!restored.restore(bad));
        QCOMPARE(restored.cache(), store.cache());
    }
};
QTEST_GUILESS_MAIN(TestSessionGraphStore)
#include "test_sessiongraphstore.moc"
