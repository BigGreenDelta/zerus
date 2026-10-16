#include "GraphPage.h"
#include "GraphViewStateStore.h"
#include "SessionCardDelegate.h"
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QtTest>

class TestGraphPage : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    GraphPage::Node node(const QString &id, const QString &project,
                         const QString &host = "fixture-arch") const {
        GraphPage::Node value;
        value.card = {id,
                      project,
                      {{SessionRoles::Title, "Review the implementation"},
                       {SessionRoles::Status, "Ready"},
                       {SessionRoles::Agent, "claude"},
                       {SessionRoles::Host, host},
                       {SessionRoles::MachineName, host},
                       {SessionRoles::Meta, "feature/graph"},
                       {SessionRoles::MetaIcon, "git-branch"},
                       {SessionRoles::Detail, project}}};
        value.project = project;
        value.identity = "Native conversation: " + id;
        value.detail = "Confirmed native binding";
        value.targets.append(
            {host, "claude/example/review", "run-" + id, "conversation-" + id, {}, {}, true});
        return value;
    }
    QList<GraphPage::Node> nodes() const {
        return {node("review", "Zerus"), node("release", "Infrastructure", "fixture-mac"),
                node("other", "Unassigned")};
    }
    QList<GraphPage::Relation> relations() const {
        return {{{"handoff", "review", "release", "message", "Review → Release"},
                 "Verified delivery receipt · submitted, consumption unknown"}};
    }
  private slots:
    void deliveryEvidenceIsPagedAndExplicitlyOpensOneExactReceipt() {
        GraphPage page(nullptr, {});
        page.resize(1400, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto edges = relations();
        for (int i = 0; i < 101; ++i)
            edges[0].events.append(QJsonObject{{"id", QString("event-%1").arg(i)},
                                               {"request_id", QString("request-%1").arg(i)},
                                               {"at", 1000 + i}});
        QVERIFY(page.setProjection(nodes(), edges));
        QSignalSpy open(&page, &GraphPage::evidenceRequested);
        page.focusNode("review");
        auto *connections = page.findChild<QListWidget *>("graphConnections");
        connections->setCurrentRow(0);
        connections->setFocus();
        QTest::keyClick(connections, Qt::Key_Return);
        auto *evidence = page.findChild<QListWidget *>("graphEvidence");
        QTRY_VERIFY(evidence->isVisible());
        QCOMPARE(evidence->count(), 50);
        QCOMPARE(open.size(), 0);
        auto *older = page.findChild<QPushButton *>("graphEvidenceOlder");
        QTest::mouseClick(older, Qt::LeftButton);
        QCOMPARE(evidence->count(), 50);
        QCOMPARE(evidence->item(0)->data(Qt::UserRole).toString(), QString("request-50"));
        auto *button = page.findChild<QPushButton *>("graphEvidenceOpen");
        QVERIFY(button->isEnabled());
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(open.size(), 1);
        QCOMPARE(open[0][5].toString(), QString("request-50"));
        QVERIFY(page.setProjection(nodes(), edges));
        QCOMPARE(evidence->item(0)->data(Qt::UserRole).toString(), QString("request-50"));
        QTest::mouseClick(older, Qt::LeftButton);
        QCOMPARE(evidence->count(), 1);
        QVERIFY(!older->isEnabled());
        auto unavailable = nodes();
        unavailable[1].targets[0].available = false;
        QVERIFY(page.setProjection(unavailable, edges));
        QVERIFY(!button->isEnabled());
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(open.size(), 1);
    }
    void initTestCase() {
        QVERIFY(settings.isValid());
        QCoreApplication::setOrganizationName("ZerusGraphFixture");
        QCoreApplication::setApplicationName("GraphPage");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           qEnvironmentVariable("ZERUS_GRAPH_SETTINGS_ROOT", settings.path()));
    }
    void nativeIdentityUpgradePreservesPinsCameraAndSelectionAcrossReopen() {
        GraphPage page(nullptr, {});
        auto provisional = nodes();
        provisional[0].card.id = "provisional";
        QVERIFY(page.setProjection(provisional, {}));
        page.focusNode("provisional");
        auto state = page.viewState();
        auto canvas = state["canvas"].toObject();
        auto positions = canvas["positions"].toObject();
        positions["provisional"] = QJsonArray{1500, 900};
        canvas["positions"] = positions;
        canvas["scale"] = .75;
        canvas["center"] = QJsonArray{1300, 800};
        state["canvas"] = canvas;
        QVERIFY(page.restoreViewState(state));
        const auto camera = page.canvas()->viewState().value("center");
        QVERIFY(page.setProjection(nodes(), {}, {{"provisional", "review"}}));
        QCOMPARE(page.canvas()->nodeRect("review").topLeft(), QPointF(1500, 900));
        QCOMPARE(page.canvas()->viewState()["center"], camera);
        QCOMPARE(page.viewState()["selectedNode"].toString(), QString("review"));
        QVERIFY(
            !page.viewState()["canvas"].toObject()["positions"].toObject().contains("provisional"));
        GraphPage reopened(nullptr, {});
        QVERIFY(reopened.setProjection(nodes(), {}, {{"provisional", "review"}}));
        QVERIFY(reopened.restoreViewState(state));
        QCOMPARE(reopened.canvas()->nodeRect("review").topLeft(), QPointF(1500, 900));
        QCOMPARE(reopened.viewState()["selectedNode"].toString(), QString("review"));
    }
    void inspectDoesNotNavigateAndOpenCarriesExactIdentity() {
        GraphPage page(nullptr, {});
        page.resize(1400, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QVERIFY(page.setProjection(nodes(), relations()));
        QSignalSpy open(&page, &GraphPage::openRequested);
        page.focusNode("review");
        QCOMPARE(open.size(), 0);
        auto *button = page.findChild<QPushButton *>("graphOpenActivity");
        QVERIFY(button->isVisible());
        QVERIFY(button->isEnabled());
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(open.size(), 1);
        QCOMPARE(open[0], QList<QVariant>({"fixture-arch", "claude/example/review", "run-review",
                                           "conversation-review", "", ""}));
        auto unavailable = nodes();
        unavailable[0].targets[0].available = false;
        QVERIFY(page.setProjection(unavailable, relations()));
        QVERIFY(!button->isEnabled());
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(open.size(), 1);
    }
    void selectedCardCanReopenItsClosedInspectorWithoutNavigation() {
        GraphPage page(nullptr, {});
        page.resize(1400, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QVERIFY(page.setProjection(nodes(), relations()));
        page.focusNode("review");
        QSignalSpy open(&page, &GraphPage::openRequested);
        QTest::mouseClick(page.findChild<QPushButton *>("graphInspectorClose"), Qt::LeftButton);
        auto *inspector = page.findChild<QWidget *>("graphInspector");
        QVERIFY(inspector->isHidden());
        page.canvas()->fitGraph();
        QCoreApplication::processEvents();
        QTest::mouseClick(page.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          page.canvas()->mapFromScene(page.canvas()->nodeRect("review").center()));
        QTRY_VERIFY(!inspector->isHidden());
        QCOMPARE(open.size(), 0);
    }
    void openingInspectorRevealsCardAfterReleaseWithoutMovingLayout() {
        GraphPage page(nullptr, {});
        page.resize(1100, 760);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QVERIFY(page.setProjection({node("review", "Zerus")}, {}));
        auto *canvas = page.canvas();
        auto state = canvas->viewState();
        const auto bounds = canvas->nodeRect("review");
        state["center"] = QJsonArray{bounds.center().x() - 220, bounds.center().y()};
        QVERIFY(canvas->restoreViewState(state));
        QCoreApplication::processEvents();
        const auto positions = canvas->viewState().value("positions");
        const auto press = canvas->mapFromScene(bounds.center());
        QVERIFY(canvas->viewport()->rect().contains(press));
        QSignalSpy open(&page, &GraphPage::openRequested);
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, press);
        QCoreApplication::processEvents();
        QVERIFY(page.findChild<QWidget *>("graphInspector")->isHidden());
        const auto heldCamera = canvas->viewState().value("center");
        QTest::qWait(40);
        QCOMPARE(canvas->viewState().value("center"), heldCamera);
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, press);
        QTRY_VERIFY(page.findChild<QWidget *>("graphInspector")->isVisible());
        QCoreApplication::processEvents(); // The reveal follows the splitter
                                           // layout.
        QTRY_VERIFY(
            canvas->viewport()->rect().contains(canvas->mapFromScene(bounds).boundingRect()));
        QCOMPARE(canvas->viewState().value("positions"), positions);
        QCOMPARE(open.size(), 0);
        canvas->horizontalScrollBar()->setValue(canvas->horizontalScrollBar()->value() + 60);
        const auto manualCamera = canvas->viewState().value("center");
        QVERIFY(page.setProjection({node("review", "Zerus")}, {}));
        QCoreApplication::processEvents();
        QCOMPARE(canvas->viewState().value("center"), manualCamera);
    }
    void projectScopeKeepsOutsideNeighborsAndPinsSurviveFiltering() {
        GraphPage page(nullptr, {});
        QVERIFY(page.setProjection(nodes(), relations()));
        auto state = page.canvas()->viewState();
        state["positions"] = QJsonObject{{"other", QJsonArray{980, 400}}};
        QVERIFY(page.canvas()->restoreViewState(state));
        page.focusProject("Zerus");
        QCOMPARE(page.canvas()->nodeCount(), 2);
        QCOMPARE(page.canvas()->relationCount(), 1);
        page.focusProject({});
        QCOMPARE(page.canvas()->nodeCount(), 3);
        QCOMPARE(page.canvas()->nodePosition("other"), QPointF(980, 400));
    }
    void listSelectionAndCanvasUseSameModelAndNoReadAction() {
        GraphPage page(nullptr, {});
        page.resize(1400, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QVERIFY(page.setProjection(nodes(), relations()));
        QSignalSpy open(&page, &GraphPage::openRequested);
        QTest::mouseClick(page.findChild<QPushButton *>("graphListToggle"), Qt::LeftButton);
        auto *list = page.findChild<QListView *>("graphAccessibleList");
        QVERIFY(list->isVisible());
        QCOMPARE(list->model(), page.canvas()->nodeModel());
        list->setCurrentIndex(list->model()->index(1, 0));
        QCOMPARE(page.canvas()->selectedNode(), QString("release"));
        QCOMPARE(open.size(), 0);
        QCOMPARE(page.findChild<QListWidget *>("graphBindings")->count(), 1);
    }
    void repeatedProjectionPreservesChosenExactBinding() {
        GraphPage page(nullptr, {});
        page.resize(1400, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto values = nodes();
        values[0].targets.append({"fixture-arch",
                                  "claude/example/resumed",
                                  "resumed-run",
                                  "conversation-review",
                                  {},
                                  {},
                                  true});
        QVERIFY(page.setProjection(values, relations()));
        page.focusNode("review");
        auto *bindings = page.findChild<QListWidget *>("graphBindings");
        QCOMPARE(bindings->currentRow(), -1);
        QVERIFY(!page.findChild<QPushButton *>("graphOpenActivity")->isEnabled());
        bindings->setCurrentRow(1);
        QVERIFY(page.setProjection(values, relations()));
        QCOMPARE(bindings->currentRow(), 1);
        QSignalSpy open(&page, &GraphPage::openRequested);
        QTest::mouseClick(page.findChild<QPushButton *>("graphOpenActivity"), Qt::LeftButton);
        QCOMPARE(open.size(), 1);
        QCOMPARE(open[0][2].toString(), QString("resumed-run"));
    }
    void unchangedRefreshKeepsEvidenceSelection() {
        GraphPage page(nullptr, {});
        page.resize(1400, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QVERIFY(page.setProjection(nodes(), relations()));
        page.focusNode("review");
        auto *connections = page.findChild<QListWidget *>("graphConnections");
        connections->setCurrentRow(0);
        QVERIFY(page.setProjection(nodes(), relations()));
        QCOMPARE(connections->currentRow(), 0);
    }
    void searchMatchesMachineAndBranch() {
        GraphPage page(nullptr, {});
        QVERIFY(page.setProjection(nodes(), relations()));
        auto *search = page.findChild<QLineEdit *>("graphSearch");
        search->setText("fixture-mac");
        QTRY_COMPARE(page.canvas()->nodeCount(), 1);
        search->setText("feature/graph");
        QTRY_COMPARE(page.canvas()->nodeCount(), 3);
    }
    void arrangeIsExplicitUndoableAndRefreshDoesNotMovePins() {
        GraphPage page(nullptr, {});
        page.resize(1400, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QVERIFY(page.setProjection(nodes(), relations()));
        auto original = page.canvas()->viewState();
        original["positions"] = QJsonObject{{"review", QJsonArray{500, 600}},
                                            {"release", QJsonArray{-500, 400}},
                                            {"other", QJsonArray{980, 400}}};
        original["scale"] = .75;
        original["center"] = QJsonArray{100, 200};
        QVERIFY(page.canvas()->restoreViewState(original));
        QSignalSpy open(&page, &GraphPage::openRequested);
        QTest::mouseClick(page.findChild<QPushButton *>("graphArrange"), Qt::LeftButton);
        QVERIFY(page.canvas()->nodePosition("review") != QPointF(500, 600));
        const auto arranged = page.canvas()->viewState().value("positions");
        QVERIFY(page.setProjection(nodes(), relations()));
        QCOMPARE(page.canvas()->viewState().value("positions"), arranged);
        auto *undo = page.findChild<QPushButton *>("graphUndoArrange");
        QVERIFY(undo->isVisible());
        QTest::mouseClick(undo, Qt::LeftButton);
        QCOMPARE(page.canvas()->viewState().value("positions"), original["positions"]);
        QCOMPARE(page.canvas()->viewState().value("scale"), original["scale"]);
        QCOMPARE(page.canvas()->viewState().value("center"), original["center"]);
        QVERIFY(undo->isHidden());
        QCOMPARE(open.size(), 0);
    }
    void positionsCameraFiltersAndInspectionSurviveReopen() {
        const QString key = "fixtures/reopen";
        QSettings().remove(key);
        QJsonObject saved;
        {
            GraphPage page(nullptr, key);
            QSignalSpy loaded(page.findChild<GraphViewStateStore *>(),
                              &GraphViewStateStore::loaded);
            QTRY_COMPARE(loaded.size(), 1);
            page.resize(1400, 900);
            page.show();
            QVERIFY(QTest::qWaitForWindowExposed(&page));
            QVERIFY(page.setProjection(nodes(), relations()));
            page.focusNode("review");
            auto state = page.canvas()->viewState();
            state["positions"] = QJsonObject{{"review", QJsonArray{500, 600}},
                                             {"release", QJsonArray{-500, 400}},
                                             {"other", QJsonArray{980, 400}}};
            QVERIFY(page.canvas()->restoreViewState(state));
            page.canvas()->zoomBy(.8);
            page.canvas()->horizontalScrollBar()->setValue(
                page.canvas()->horizontalScrollBar()->value() + 40);
            page.focusProject("Zerus");
            saved = page.viewState();
            page.hide(); // Navigation flushes the coalesced write, even before its
                         // timer.
        }
        // Reopen immediately, without waiting for the pending background write.
        GraphPage reopened(nullptr, key);
        QSignalSpy open(&reopened, &GraphPage::openRequested);
        QSignalSpy loaded(reopened.findChild<GraphViewStateStore *>(),
                          &GraphViewStateStore::loaded);
        reopened.resize(1000, 700);
        reopened.show();
        QVERIFY(QTest::qWaitForWindowExposed(&reopened));
        QTRY_COMPARE(loaded.size(), 1);
        // Host data arrives after local view state and at a different window size.
        QVERIFY(reopened.setProjection(nodes(), relations()));
        QCOMPARE(reopened.viewState().value("project"), saved["project"]);
        QCOMPARE(reopened.canvas()->viewState().value("positions"),
                 saved["canvas"].toObject()["positions"]);
        QCOMPARE(reopened.canvas()->viewState().value("scale"),
                 saved["canvas"].toObject()["scale"]);
        QCOMPARE(reopened.canvas()->viewState().value("center"),
                 saved["canvas"].toObject()["center"]);
        QCOMPARE(reopened.canvas()->selectedNode(), QString("review"));
        QVERIFY(reopened.findChild<QWidget *>("graphInspector")->isVisible());
        QCOMPARE(open.size(), 0);
        const auto bytes = QSettings().value(key).toByteArray();
        QCOMPARE(QJsonDocument::fromJson(bytes).object(), saved);
    }
    void layoutSurvivesProcessExitBeforeSaveTimer() {
        const QString key = "fixtures/process-restart";
        if (qEnvironmentVariable("ZERUS_GRAPH_PROCESS_FIXTURE") == "write") {
            GraphPage page(nullptr, key);
            QSignalSpy loaded(page.findChild<GraphViewStateStore *>(),
                              &GraphViewStateStore::loaded);
            QTRY_COMPARE(loaded.size(), 1);
            QVERIFY(page.setProjection(nodes(), relations()));
            auto state = page.canvas()->viewState();
            state["positions"] = QJsonObject{{"review", QJsonArray{765, -432}}};
            QVERIFY(page.canvas()->restoreViewState(state));
            page.canvas()->zoomBy(.75);
            return; // Destruction and application exit flush the pending change.
        }
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("ZERUS_GRAPH_PROCESS_FIXTURE", "write");
        environment.insert("ZERUS_GRAPH_SETTINGS_ROOT", settings.path());
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(), {QTest::currentTestFunction()});
        QVERIFY(child.waitForStarted(5000));
        QVERIFY(child.waitForFinished(15000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, child.readAllStandardOutput().constData());
        QSettings().sync();
        GraphPage reopened(nullptr, key);
        QSignalSpy loaded(reopened.findChild<GraphViewStateStore *>(),
                          &GraphViewStateStore::loaded);
        QTRY_COMPARE(loaded.size(), 1);
        QVERIFY(reopened.setProjection(nodes(), relations()));
        QCOMPARE(reopened.canvas()->nodePosition("review"), QPointF(765, -432));
        QCOMPARE(reopened.canvas()->viewState().value("scale"), QJsonValue(.75));
    }
    void undoWithNewArrivalDoesNotOverlapRestoredPins() {
        GraphPage page(nullptr, {});
        QVERIFY(page.setProjection({node("a", "Project")}, {}));
        auto old = page.canvas()->viewState();
        // This is the slot the new child will prefer after arrangement.
        old["positions"] = QJsonObject{{"a", QJsonArray{0, 152}}};
        QVERIFY(page.canvas()->restoreViewState(old));
        page.findChild<QPushButton *>("graphArrange")->click();
        QVERIFY(page.setProjection({node("a", "Project"), node("new", "Project")}, {}));
        QCOMPARE(page.canvas()->nodePosition("new"), QPointF(0, 152));
        page.findChild<QPushButton *>("graphUndoArrange")->click();
        QCOMPARE(page.canvas()->nodePosition("a"), QPointF(0, 152));
        QVERIFY(!page.canvas()->nodeRect("a").intersects(page.canvas()->nodeRect("new")));
        QCOMPARE(page.canvas()->nodeCount(), 2);
    }
    void corruptSavedStateIsVisibleAndDoesNotReplaceCurrentGraph_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("syntax") << QByteArray("{broken");
        QTest::newRow("invalid-layout")
            << QByteArray("{\"schema\":1,\"canvas\":{\"schema\":1,\"scale\":0}}");
        QTest::newRow("oversized") << QByteArray(1024 * 1024 + 1, 'x');
    }
    void corruptSavedStateIsVisibleAndDoesNotReplaceCurrentGraph() {
        QFETCH(QByteArray, bytes);
        const QString key = "fixtures/corrupt";
        QSettings().setValue(key, bytes);
        GraphPage page(nullptr, key);
        QSignalSpy loaded(page.findChild<GraphViewStateStore *>(), &GraphViewStateStore::loaded);
        QVERIFY(page.setProjection(nodes(), relations()));
        const auto before = page.canvas()->viewState().value("positions");
        QTRY_COMPARE(loaded.size(), 1);
        QCOMPARE(page.canvas()->viewState().value("positions"), before);
        QVERIFY(page.findChild<QLabel *>("graphFreshness")->text().contains("could not be loaded"));
        QCOMPARE(QSettings().value(key).toByteArray(), bytes);
        QSignalSpy saved(page.findChild<GraphViewStateStore *>(), &GraphViewStateStore::saved);
        page.canvas()->zoomBy(.8);
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(saved[0][0].toString().isEmpty());
        QVERIFY(!page.findChild<QLabel *>("graphFreshness")->text().contains("could not"));
        QVERIFY(QJsonDocument::fromJson(QSettings().value(key).toByteArray()).isObject());
    }
    void lateSavedViewDoesNotOverrideNewInteraction() {
        const QString key = "fixtures/late";
        GraphPage fixture(nullptr, {});
        QVERIFY(fixture.setProjection(nodes(), relations()));
        auto saved = fixture.viewState();
        auto canvas = saved["canvas"].toObject();
        canvas["positions"] = QJsonObject{{"review", QJsonArray{8000, 6000}}};
        saved["canvas"] = canvas;
        QSettings().setValue(key, QJsonDocument(saved).toJson(QJsonDocument::Compact));
        GraphPage page(nullptr, key);
        QSignalSpy loaded(page.findChild<GraphViewStateStore *>(), &GraphViewStateStore::loaded);
        QVERIFY(page.setProjection(nodes(), relations()));
        page.canvas()->zoomBy(.8); // Before the queued settings callback.
        const auto newest = page.canvas()->viewState();
        QTRY_COMPARE(loaded.size(), 1);
        QCOMPARE(page.canvas()->viewState(), newest);
    }
    void narrowInspectorOverlaysWithoutSqueezingCanvasAndRaster() {
        GraphPage page(nullptr, {});
        page.resize(880, 720);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QVERIFY(page.setProjection(nodes(), relations()));
        const int before = page.canvas()->width();
        page.focusNode("review");
        QCoreApplication::processEvents();
        auto *inspector = page.findChild<QWidget *>("graphInspector");
        QVERIFY(inspector->isVisible());
        QCOMPARE(inspector->parentWidget(), &page);
        QCOMPARE(page.canvas()->width(), before);
        QVERIFY(page.rect().contains(inspector->geometry()));
        const auto path = qEnvironmentVariable("ZERUS_GRAPH_PAGE_RASTER");
        if (!path.isEmpty()) {
            page.resize(1500, 900);
            QCoreApplication::processEvents();
            page.canvas()->fitGraph();
            QVERIFY(page.grab().save(path));
        }
    }
};
QTEST_MAIN(TestGraphPage)
#include "test_graphpage.moc"
