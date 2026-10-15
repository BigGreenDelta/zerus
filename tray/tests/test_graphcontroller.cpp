#include "GraphController.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

class TestGraphController : public QObject {
    Q_OBJECT
    QJsonObject page(const QString &cursor = "cursor", bool more = false, int revision = 1) const {
        const QString id = "node:" + QString(64, 'a');
        return {{"schema", 1},
                {"machine_id", "11111111-1111-4111-8111-111111111111"},
                {"epoch", "22222222-2222-4222-8222-222222222222"},
                {"revision", revision},
                {"reset_required", false},
                {"cursor", cursor},
                {"has_more", more},
                {"coverage",
                 QJsonObject{{"bindings_complete", true}, {"communication", "not_indexed"}}},
                {"items", QJsonArray{QJsonObject{
                              {"id", id},
                              {"revision", revision},
                              {"deleted", false},
                              {"item", QJsonObject{{"kind", "node"},
                                                   {"value", QJsonObject{{"fixture", true}}}}}}}}};
    }
    void reply(GraphController &controller, const QList<QVariant> &request,
               const QJsonObject &value) {
        controller.received(request[0].toULongLong(), request[1].toString(), value);
    }
  private slots:
    void hiddenPageDoesNotPollAndOneReadIsInFlight() {
        GraphController controller;
        QSignalSpy reads(&controller, &GraphController::requestPage);
        controller.setRoutes({"mac", {}});
        QCOMPARE(reads.size(), 0);
        controller.setActive(true);
        QCOMPARE(reads.size(), 1);
        QCOMPARE(reads[0][1].toString(), QString());
        controller.refresh();
        QCOMPARE(reads.size(), 1);
        controller.setActive(false);
        reply(controller, reads[0], page());
        QTRY_VERIFY(!controller.pending());
        QVERIFY(controller.confirmed({}));
        QCOMPARE(reads.size(), 1);
        controller.setActive(true);
        QCOMPARE(reads.size(), 2);
        QCOMPARE(reads[1][1].toString(), QString("mac"));
        controller.setActive(false);
    }
    void removedRouteDropsLateReplyAndItsCache() {
        GraphController controller;
        QSignalSpy reads(&controller, &GraphController::requestPage);
        controller.setRoutes({"old"});
        controller.setActive(true);
        const auto old = reads.takeFirst();
        controller.setRoutes({"new"});
        QCOMPARE(reads.size(), 0); // Retired transport must settle before another read starts.
        reply(controller, old, page());
        QCOMPARE(reads.size(), 1);
        QVERIFY(!controller.store().host("old"));
        controller.setActive(false);
        reply(controller, reads[0], page());
        QTRY_VERIFY(!controller.pending());
        QVERIFY(controller.store().host("new"));
        QVERIFY(!controller.store().host("old"));
    }
    void mismatchedAndMalformedResponsesDoNotReplaceLastGood() {
        GraphController controller;
        QSignalSpy reads(&controller, &GraphController::requestPage);
        controller.setRoutes({"mac"});
        controller.setActive(true);
        controller.received(reads[0][0].toULongLong(), "other", page());
        QVERIFY(controller.pending());
        controller.setActive(false);
        reply(controller, reads.takeFirst(), page());
        QTRY_VERIFY(!controller.pending());
        const auto old = controller.store().cache();
        controller.refresh();
        controller.setActive(true);
        QCOMPARE(reads.size(), 1);
        QVERIFY(reads[0][2].toBool());
        QCOMPARE(reads[0][3].toString(), QString("cursor"));
        controller.setActive(false);
        reply(controller, reads.takeFirst(), QJsonObject{{"schema", 99}});
        QTRY_VERIFY(!controller.pending());
        QCOMPARE(controller.store().cache(), old);
        QVERIFY(!controller.confirmed("mac"));
        QVERIFY(controller.details().contains("invalid graph page"));
    }
    void partialSnapshotContinuesAfterReopeningAndDoesNotPublishHalf() {
        GraphController controller;
        QSignalSpy reads(&controller, &GraphController::requestPage);
        controller.setRoutes({"mac"});
        controller.setActive(true);
        controller.setActive(false);
        reply(controller, reads.takeFirst(), page("next", true, 2));
        QTRY_VERIFY(!controller.pending());
        QVERIFY(!controller.store().host("mac"));
        controller.refresh();
        controller.setActive(true);
        QCOMPARE(reads.size(), 1);
        QCOMPARE(reads[0][2].toBool(), false);
        QCOMPARE(reads[0][3].toString(), QString("next"));
        auto last = page("end", false, 2);
        auto rows = last["items"].toArray();
        auto row = rows[0].toObject();
        row["id"] = "node:" + QString(64, 'b');
        rows[0] = row;
        last["items"] = rows;
        controller.setActive(false);
        reply(controller, reads.takeFirst(), last);
        QTRY_VERIFY(!controller.pending());
        QVERIFY(controller.confirmed("mac"));
        QCOMPARE(controller.store().host("mac")->items.size(), 2);
    }
    void resetKeepsLastGoodAndRequestsFreshSnapshot() {
        GraphController controller;
        QSignalSpy reads(&controller, &GraphController::requestPage);
        controller.setRoutes({"mac"});
        controller.setActive(true);
        controller.setActive(false);
        reply(controller, reads.takeFirst(), page());
        QTRY_VERIFY(!controller.pending());
        controller.refresh();
        controller.setActive(true);
        auto reset = page();
        reset["reset_required"] = true;
        controller.setActive(false);
        reply(controller, reads.takeFirst(), reset);
        QTRY_VERIFY(!controller.pending());
        QVERIFY(controller.store().host("mac"));
        QVERIFY(!controller.confirmed("mac"));
        controller.setActive(true);
        QCOMPARE(reads.size(), 1);
        QVERIFY(!reads[0][2].toBool());
        QVERIFY(reads[0][3].toString().isEmpty());
        controller.setActive(false);
    }
    void cacheLoadsWithoutNetworkAndConfirmationRequiresFreshReply() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath("graph.json");
        SessionGraphStore store;
        QCOMPARE(store.apply("mac", page(), false, {}, 1000), SessionGraphStore::Applied);
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(store.cache()).toJson());
        }
        GraphController controller(path);
        QSignalSpy reads(&controller, &GraphController::requestPage),
            projections(&controller, &GraphController::projectionChanged);
        controller.setRoutes({"mac"});
        QTRY_VERIFY(!controller.pending());
        QCOMPARE(reads.size(), 0);
        QVERIFY(controller.store().host("mac"));
        QVERIFY(!controller.confirmed("mac"));
        const int before = projections.size();
        controller.setActive(true);
        auto fresh = page("delta");
        fresh["items"] = QJsonArray{};
        controller.setActive(false);
        reply(controller, reads.takeFirst(), fresh);
        QTRY_VERIFY(!controller.pending());
        QVERIFY(controller.confirmed("mac"));
        QVERIFY(projections.size() > before);
        controller.setRoutes({});
        QVERIFY(!controller.store().host("mac"));
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly))
                    return false;
                return QJsonDocument::fromJson(file.readAll())
                    .object()["hosts"]
                    .toObject()
                    .isEmpty();
            }(),
            5000);
#ifndef Q_OS_WIN
        QVERIFY(!(QFile::permissions(path) & QFileDevice::ReadGroup));
        QVERIFY(!(QFile::permissions(path) & QFileDevice::ReadOther));
#endif
    }
    void corruptedCacheDoesNotPreventExplicitRefresh() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath("graph.json");
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("invalid");
        }
        GraphController controller(path);
        controller.setRoutes({"mac"});
        QSignalSpy reads(&controller, &GraphController::requestPage);
        QTRY_VERIFY(!controller.pending());
        QVERIFY(controller.details().contains("cache is unavailable"));
        controller.setActive(true);
        QCOMPARE(reads.size(), 1);
        controller.setActive(false);
        reply(controller, reads[0], page());
        QTRY_VERIFY(!controller.pending());
        QVERIFY(controller.confirmed("mac"));
    }
};
QTEST_GUILESS_MAIN(TestGraphController)
#include "test_graphcontroller.moc"
