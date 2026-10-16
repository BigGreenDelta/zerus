#include "GraphCanvas.h"
#include "SessionCardDelegate.h"
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QWheelEvent>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>

class TestGraphCanvas : public QObject {
    Q_OBJECT
    QTemporaryDir home;
    static GraphCanvas::Node node(const QString &id, const QString &status = "Ready") {
        return {id,
                "project",
                {{SessionRoles::Title, "Review the implementation"},
                 {SessionRoles::Status, status},
                 {SessionRoles::Agent, "claude"},
                 {SessionRoles::Host, "fixture-mac"},
                 {SessionRoles::MachineName, "fixture-mac"},
                 {SessionRoles::MachineColor, QColor("#7da7d1")},
                 {SessionRoles::Meta, "feature/graph"},
                 {SessionRoles::MetaIcon, "git-branch"},
                 {SessionRoles::Detail, "Project context"},
                 {SessionRoles::Working, status == "Working" || status == "Background"},
                 {SessionRoles::Attention, status == "Needs input"}}};
    }
  private slots:
    void initTestCase() {
        QVERIFY(home.isValid());
        QCoreApplication::setOrganizationName("ZerusGraphFixture");
        QCoreApplication::setApplicationName("GraphCanvas");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, home.path());
    }
    void nativePinchUsesTheGestureAnchorAndPreservesPins_data() {
        QTest::addColumn<bool>("viewport");
        QTest::newRow("viewport-route") << true;
        QTest::newRow("widget-route") << false;
    }
    void nativePinchUsesTheGestureAnchorAndPreservesPins() {
        QFETCH(bool, viewport);
        GraphCanvas view;
        view.resize(1000, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QVERIFY(view.setGraph({node("one"), node("two")}, {}));
        view.fitGraph();
        const auto pins = view.viewState().value("positions");
        const QPointF anchor(210, 180);
        const auto before = view.mapToScene(anchor.toPoint());
        const auto old = view.transform().m11();
        QSignalSpy changed(&view, &GraphCanvas::viewStateChanged);
        auto send = [&](Qt::NativeGestureType type, qreal value) {
            QNativeGestureEvent event(type, QPointingDevice::primaryPointingDevice(), 2, anchor,
                                      anchor, view.viewport()->mapToGlobal(anchor.toPoint()), value,
                                      {});
            QApplication::sendEvent(viewport ? view.viewport() : static_cast<QWidget *>(&view),
                                    &event);
            QVERIFY(event.isAccepted());
        };
        send(Qt::BeginNativeGesture, 0);
        send(Qt::ZoomNativeGesture, .25);
        send(Qt::EndNativeGesture, 0);
        QVERIFY(qAbs(view.transform().m11() - old * 1.25) < .00001);
        QVERIFY(QLineF(before, view.mapToScene(anchor.toPoint())).length() < 3);
        QCOMPARE(view.viewState()["positions"], pins);
        QCOMPARE(changed.size(), 1);
        send(Qt::ZoomNativeGesture, -.2);
        QVERIFY(qAbs(view.transform().m11() - old) < .00001);
        send(Qt::ZoomNativeGesture, 100);
        QCOMPARE(view.transform().m11(), 2.5);
        send(Qt::ZoomNativeGesture, -.9999);
        QCOMPARE(view.transform().m11(), .15);
        const auto stable = view.viewState();
        send(Qt::ZoomNativeGesture, -2);
        QCOMPARE(view.viewState(), stable);
        send(Qt::ZoomNativeGesture, std::numeric_limits<qreal>::quiet_NaN());
        QCOMPARE(view.viewState(), stable);
    }
    void boundedLargeGraphKeepsManualPinsAndReportsInteractionCost_data() {
        QTest::addColumn<int>("count");
        QTest::addColumn<int>("edges");
        QTest::newRow("ordinary-250-750") << 250 << 750;
        QTest::newRow("ceiling-2000-10000") << 2000 << 10000;
    }
    void boundedLargeGraphKeepsManualPinsAndReportsInteractionCost() {
        QFETCH(int, count);
        QFETCH(int, edges);
        GraphCanvas view;
        view.resize(1200, 800);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QList<GraphCanvas::Node> nodes;
        QList<GraphCanvas::Relation> relations;
        for (int i = 0; i < count; ++i) {
            auto n = node(QString::number(i));
            n.group = QString("project-%1").arg(i / 50);
            nodes.append(n);
        }
        for (int i = 0; i < edges; ++i)
            relations.append({QString("edge-%1").arg(i), QString::number(i % count),
                              QString::number((i * 37 + 1) % count), "message",
                              "Verified submission"});
        QElapsedTimer timer;
        timer.start();
        QVERIFY(view.setGraph(nodes, relations));
        const auto initial = timer.elapsed();
        const auto position = view.nodePosition("0");
        const auto initialNeighbor = view.nodePosition("1");
        timer.restart();
        QVERIFY(view.setGraph(nodes, relations));
        const auto refresh = timer.elapsed();
        QCOMPARE(view.nodePosition("0"), position);
        view.selectNode("0");
        timer.restart();
        for (int i = 0; i < 10; ++i) {
            view.horizontalScrollBar()->setValue(view.horizontalScrollBar()->value() + 8);
            view.viewport()->repaint();
        }
        qInfo() << "Graph cost" << count << edges << "initial ms" << initial << "refresh ms"
                << refresh << "ten pan frames ms" << timer.elapsed();
        view.selectNode("0");
        const auto press = view.mapFromScene(view.nodeRect("0").center());
        QVERIFY(view.viewport()->rect().contains(press));
        timer.restart();
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, press);
        for (int i = 1; i <= 10; ++i) {
            QTest::mouseMove(view.viewport(), press + QPoint(i * 4, i * 2), 1);
            view.viewport()->repaint();
        }
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, press + QPoint(40, 20));
        qInfo() << "Graph ten physical drag steps ms" << count << timer.elapsed();
        QVERIFY(view.nodePosition("0") != position);
        QCOMPARE(view.nodePosition("1"), initialNeighbor);
        QCOMPARE(view.nodeCount(), count);
        QCOMPARE(view.selectedNode(), QString("0"));
    }
    void exactSessionsRenderer_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QString>("status");
        for (bool dark : {false, true})
            for (const auto &status : {"Ready", "Working", "Background", "Needs input", "Offline"})
                QTest::newRow(qPrintable(QString::number(dark) + status))
                    << dark << QString(status);
    }
    void exactSessionsRenderer() {
        QFETCH(bool, dark);
        QFETCH(QString, status);
        GraphCanvas view;
        view.resize(700, 400);
        view.setTheme(dark);
        QVERIFY(view.setGraph({node("one", status)}, {}));
        // Isolate the card paint from the separate project-region background.
        for (auto *item : view.scene()->items())
            if (item->zValue() < 0)
                item->hide();
        QImage actual(340, 104, QImage::Format_ARGB32_Premultiplied);
        actual.fill(Qt::transparent);
        {
            QPainter p(&actual);
            p.setRenderHint(QPainter::Antialiasing);
            view.scene()->render(&p, QRectF(0, 0, 340, 104), view.nodeRect("one"));
        }
        QImage expected(actual.size(), actual.format());
        expected.fill(Qt::transparent);
        {
            QPainter p(&expected);
            p.setRenderHint(QPainter::Antialiasing);
            QStyleOptionViewItem option;
            option.initFrom(view.viewport());
            option.widget = view.viewport();
            option.rect = QRect(0, 0, 340, 104);
            option.state.setFlag(QStyle::State_Selected, false);
            option.state.setFlag(QStyle::State_MouseOver, false);
            SessionDelegate delegate;
            delegate.paintStandalone(&p, option, view.nodeModel()->index(0, 0));
        }
        QCOMPARE(actual, expected);
    }
    void inspectingSelectionKeepsDraftAndUnreadBadges() {
        GraphCanvas view;
        auto draft = node("draft");
        draft.roles[SessionRoles::Draft] = true;
        draft.roles[SessionRoles::Unread] = true;
        QVERIFY(view.setGraph({draft}, {}));
        view.selectNode("draft");
        QStyleOptionViewItem option;
        option.widget = view.viewport();
        option.state |= QStyle::State_Selected;
        const auto index = view.nodeModel()->index(0, 0);
        const auto graph = SessionDelegate::statusForView(option, index);
        QVERIFY(graph.draft);
        QVERIFY(graph.additionalUnread);
        QCOMPARE(graph.kind, SessionStatusBadge::Draft);
        QWidget ordinary;
        option.widget = &ordinary;
        const auto editor = SessionDelegate::statusForView(option, index);
        QVERIFY(!editor.draft);
        QCOMPARE(editor.kind, SessionStatusBadge::Unread);
    }
    void refreshPreservesPinsSelectionAndCamera() {
        GraphCanvas view;
        view.resize(700, 400);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QVERIFY(view.setGraph({node("one"), node("two")},
                              {{"edge", "one", "two", "message", "Verified delivery"}}));
        view.selectNode("two");
        auto state = view.viewState();
        state["positions"] =
            QJsonObject{{"one", QJsonArray{125.0, 270.0}}, {"two", QJsonArray{700.0, 290.0}}};
        state["scale"] = 1.2;
        QVERIFY(view.restoreViewState(state));
        const auto saved = view.viewState();
        QVERIFY(view.setGraph({node("two", "Background"), node("one")},
                              {{"edge", "one", "two", "message", "Verified delivery"}}));
        QCOMPARE(view.nodePosition("one"), QPointF(125, 270));
        QCOMPARE(view.nodePosition("two"), QPointF(700, 290));
        QCOMPARE(view.selectedNode(), QString("two"));
        QCOMPARE(view.viewState().value("scale"), saved["scale"]);
        QCOMPARE(view.viewState().value("center"), saved["center"]);
    }
    void duplicateAndMissingIdentitiesDoNotReplaceLastGood() {
        GraphCanvas view;
        QVERIFY(view.setGraph({node("one")}, {}));
        QVERIFY(!view.setGraph({node("x"), node("x")}, {}));
        QCOMPARE(view.nodeCount(), 1);
        QVERIFY(!view.setGraph({node("one")}, {{"edge", "one", "missing", "parent", {}}}));
        QCOMPARE(view.nodeCount(), 1);
        auto state = view.viewState();
        state["positions"] = QJsonObject{{"one", QJsonArray{1e100, 0}}};
        QVERIFY(!view.restoreViewState(state));
        QCOMPARE(view.nodePosition("one"), QPointF());
    }
    void selectionOnlyEmitsInspectionAndCyclesAreSupported() {
        GraphCanvas view;
        QSignalSpy selected(&view, &GraphCanvas::nodeSelected);
        QVERIFY(
            view.setGraph({node("a"), node("b")}, {{"ab", "a", "b", "message", "A to B"},
                                                   {"ba", "b", "a", "message", "B to A"},
                                                   {"self", "a", "a", "reference", "Reference"}}));
        view.selectNode("a");
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.first().first().toString(), QString("a"));
        QCOMPARE(view.relationCount(), 3);
        QVERIFY(view.setGraph({node("a")}, {}));
        QCOMPARE(view.relationCount(), 0);
        QCOMPARE(view.selectedNode(), QString("a"));
    }
    void childrenUseExistingCompactGeometry() {
        GraphCanvas view;
        auto child = node("child");
        child.roles[SessionRoles::ChildId] = "worker";
        QVERIFY(view.setGraph({node("parent"), child},
                              {{"parent-child", "parent", "child", "parent", "Native child"}}));
        QCOMPARE(view.nodeRect("parent").height(), 104.0);
        QCOMPARE(view.nodeRect("child").height(), 64.0);
    }
    void dragResizesProjectWithoutChangingMembershipOrCamera() {
        GraphCanvas view;
        view.resize(1100, 800);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto second = node("b");
        second.group = "another-project";
        QVERIFY(view.setGraph({node("a"), node("c"), second}, {}));
        view.fitGraph();
        const auto before = view.groupRect("project");
        const auto other = view.groupRect("another-project");
        const auto camera = view.viewState().value("center");
        const QPoint start = view.mapFromScene(view.nodeRect("a").center());
        const QPoint end = start + QPoint(90, 95);
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, start);
        QMouseEvent move(QEvent::MouseMove, end, view.viewport()->mapToGlobal(end), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &move);
        // The cloud already follows while the button is still held.
        QVERIFY(view.groupRect("project") != before);
        QVERIFY(view.groupRect("project").height() > before.height());
        QVERIFY(view.groupRect("project").contains(view.nodeRect("a")));
        QCOMPARE(view.groupRect("another-project"), other);
        QCOMPARE(view.viewState().value("center"), camera);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, end);
        const auto moved = view.nodePosition("a");
        QVERIFY(view.setGraph({second, node("c"), node("a", "Background")}, {}));
        QCOMPARE(view.nodePosition("a"), moved);
        QVERIFY(view.groupRect("project").contains(view.nodeRect("c")));
        const QPoint backStart = view.mapFromScene(view.nodeRect("a").center());
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, backStart);
        const QPoint backEnd = backStart - QPoint(90, 95);
        QMouseEvent back(QEvent::MouseMove, backEnd, view.viewport()->mapToGlobal(backEnd),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &back);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, backEnd);
        QVERIFY(qAbs(view.groupRect("project").height() - before.height()) < 2);
    }
    void newCardsAvoidManualAndHiddenPositions() {
        GraphCanvas view;
        QVERIFY(view.setGraph({node("a"), node("hidden")}, {}));
        auto state = view.viewState();
        state["positions"] =
            QJsonObject{{"a", QJsonArray{404, 0}}, {"hidden", QJsonArray{404, 152}}};
        QVERIFY(view.restoreViewState(state));
        QVERIFY(view.setGraph({node("a"), node("new")}, {}));
        QCOMPARE(view.nodePosition("a"), QPointF(404, 0));
        QVERIFY(!view.nodeRect("new").intersects(QRectF(404, 152, 340, 104)));
        QVERIFY(!view.nodeRect("new").intersects(view.nodeRect("a")));
        const auto added = view.nodePosition("new");
        QVERIFY(view.setGraph({node("new"), node("hidden"), node("a")}, {}));
        QCOMPARE(view.nodePosition("hidden"), QPointF(404, 152));
        QCOMPARE(view.nodePosition("new"), added);
        const auto camera = view.viewState().value("center");
        QVERIFY(view.setGraph({}, {}));
        QCOMPARE(view.viewState().value("center"), camera);
        QVERIFY(view.setGraph({node("a"), node("new"), node("hidden")}, {}));
        QCOMPARE(view.nodePosition("a"), QPointF(404, 0));
        QCOMPARE(view.nodePosition("new"), added);
    }
    void restoreBeforeDataAndResizeRetainsCameraAndSelection() {
        GraphCanvas view;
        const QJsonObject saved{{"schema", 1},
                                {"positions", QJsonObject{{"a", QJsonArray{1800, -800}}}},
                                {"scale", .8},
                                {"center", QJsonArray{2000, -1000}},
                                {"selected", "a"}};
        QVERIFY(view.restoreViewState(saved));
        view.resize(1200, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QVERIFY(view.setGraph({node("a")}, {}));
        QCOMPARE(view.nodePosition("a"), QPointF(1800, -800));
        QCOMPARE(view.selectedNode(), QString("a"));
        QCOMPARE(view.viewState().value("center"), saved["center"]);
        QCOMPARE(view.viewState().value("scale"), saved["scale"]);
        QVERIFY(QLineF(view.mapToScene(view.viewport()->rect().center()), QPointF(2000, -1000))
                    .length() < 3);
        view.resize(650, 500);
        QCoreApplication::processEvents();
        QCOMPARE(view.viewState().value("center"), saved["center"]);
        QVERIFY(QLineF(view.mapToScene(view.viewport()->rect().center()), QPointF(2000, -1000))
                    .length() < 3);
    }
    void arrangeVisibleKeepsHiddenPinsAndFitOnlyChangesCamera() {
        GraphCanvas view;
        QVERIFY(view.setGraph({node("a"), node("hidden")}, {}));
        auto state = view.viewState();
        state["positions"] = QJsonObject{{"a", QJsonArray{800, 500}}, {"hidden", QJsonArray{0, 0}}};
        QVERIFY(view.restoreViewState(state));
        QVERIFY(view.setGraph({node("a")}, {}));
        view.resetLayout();
        QVERIFY(!view.nodeRect("a").intersects(QRectF(0, 0, 340, 104)));
        const auto arranged = view.nodePosition("a");
        view.fitGraph();
        QCOMPARE(view.nodePosition("a"), arranged);
        QVERIFY(view.setGraph({node("a"), node("hidden")}, {}));
        QCOMPARE(view.nodePosition("hidden"), QPointF());
        QCOMPARE(view.nodePosition("a"), arranged);
    }
    void maximumAdmittedLayoutCanBeRestored() {
        GraphCanvas view;
        QList<GraphCanvas::Node> nodes;
        for (int i = 0; i < 2000; ++i) {
            auto value = node(QString::number(i));
            value.group = QString::number(i);
            nodes.append(value);
        }
        QVERIFY(view.setGraph(nodes, {}));
        const auto state = view.viewState();
        QCOMPARE(state["positions"].toObject().size(), 2000);
        QVERIFY(view.restoreViewState(state));
    }
    void largeGraphUsesPaintItemsAndOptionalRaster() {
        GraphCanvas view;
        view.resize(1200, 800);
        QList<GraphCanvas::Node> nodes;
        QList<GraphCanvas::Relation> edges;
        for (int i = 0; i < 250; ++i) {
            nodes.append(node(QString::number(i), i % 7 == 0 ? "Background" : "Ready"));
            for (int n = 1; n <= 3; ++n)
                edges.append({QString("%1-%2").arg(i).arg(n), QString::number(i),
                              QString::number((i + n) % 250), n == 1 ? "parent" : "message",
                              "Recorded evidence"});
        }
        QVERIFY(view.setGraph(nodes, edges));
        QCOMPARE(view.nodeCount(), 250);
        QCOMPARE(view.relationCount(), 750);
        QVERIFY(view.findChildren<QWidget *>().size() < 15);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        const auto path = qEnvironmentVariable("ZERUS_GRAPH_RASTER");
        if (!path.isEmpty()) {
            view.centerOn(610, 360);
            QVERIFY(view.grab().save(path));
        }
    }
};
QTEST_MAIN(TestGraphCanvas)
#include "test_graphcanvas.moc"
