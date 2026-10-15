#include "GraphPage.h"
#include "SessionCardDelegate.h"
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPushButton>
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
    void initTestCase() {
        QVERIFY(settings.isValid());
        QCoreApplication::setOrganizationName("ZerusGraphFixture");
        QCoreApplication::setApplicationName("GraphPage");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    }
    void inspectDoesNotNavigateAndOpenCarriesExactIdentity() {
        GraphPage page;
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
        GraphPage page;
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
        QVERIFY(!inspector->isHidden());
        QCOMPARE(open.size(), 0);
    }
    void projectScopeKeepsOutsideNeighborsAndPinsSurviveFiltering() {
        GraphPage page;
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
        GraphPage page;
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
        GraphPage page;
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
        GraphPage page;
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
        GraphPage page;
        QVERIFY(page.setProjection(nodes(), relations()));
        auto *search = page.findChild<QLineEdit *>("graphSearch");
        search->setText("fixture-mac");
        QTRY_COMPARE(page.canvas()->nodeCount(), 1);
        search->setText("feature/graph");
        QTRY_COMPARE(page.canvas()->nodeCount(), 3);
    }
    void narrowInspectorOverlaysWithoutSqueezingCanvasAndRaster() {
        GraphPage page;
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
