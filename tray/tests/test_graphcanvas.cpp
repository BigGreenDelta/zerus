#include "GraphCanvas.h"
#include "SessionCardDelegate.h"
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

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
        QCOMPARE(view.viewState()["scale"], saved["scale"]);
        QCOMPARE(view.viewState()["center"], saved["center"]);
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
