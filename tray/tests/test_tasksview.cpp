#include "FakeTasksCli.h"
#include "HgsClient.h"
#include "MessageComposer.h"
#include "TaskBoard.h"
#include "TasksView.h"
#include <QAbstractButton>
#include <QDateTime>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

namespace {
const qint64 Hour = 60LL * 60 * 1000;
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
QList<TaskSession> sessions() {
    return {{"", "codex/api", "Ready", ""}, {"", "codex/web", "Working", ""},
            {"orchard-mac", "codex/ios", "Ready", ""}, {"", "codex/stopped", "Stopped", "The agent is not running. Resume it before sending."}};
}
QAbstractButton *tile(TasksView &view, const QString &stage) { return view.findChild<QAbstractButton *>("taskTile:" + stage); }
QWidget *row(TasksView &view, int n) { return view.findChild<QWidget *>(QString("taskRow:T-%1").arg(n, 6, 10, QLatin1Char('0'))); }
QToolButton *action(QWidget *row, const QString &name) { return row ? row->findChild<QToolButton *>("taskAction:" + name) : nullptr; }
QStringList visibleRows(TasksView &view) {
    QStringList out;
    for (auto *w : view.findChildren<QWidget *>()) if (w->objectName().startsWith("taskRow:") && w->isVisibleTo(&view)) out << w->objectName().mid(8);
    std::sort(out.begin(), out.end(), [&](const QString &a, const QString &b) {
        return view.findChild<QWidget *>("taskRow:" + a)->y() < view.findChild<QWidget *>("taskRow:" + b)->y(); });
    return out;
}
}

class TestTasksView : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings;
private slots:
    void initTestCase();
    void init() { QSettings().remove("tasks"); }
    void tilesCountAndChooseTheFirstStage();
    void rowsShowTheQueueAndTheOpenSessionsTasks();
    void actionsRunHgsAndRefresh();
    void sendNowNamesWhyASessionCannotTakeIt();
    void theInputFilesATaskForTheOpenSession();
    void onlyThisSessionFiltersAndIsRemembered();
    void doneShowsTheLastWeekAndOlderOnRequest();
    void aFailedListingIsShownInsteadOfTasks();
    void aSessionOutsideProjectsExplainsWhy();
    void theTabBadgeCountsReviewAndTodo();
};

void TestTasksView::initTestCase() {
    QVERIFY(m_settings.isValid());
    QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("tasks-view");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
}

void TestTasksView::tilesCountAndChooseTheFirstStage() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"), FakeTasksCli::task(2, "todo"),
        FakeTasksCli::task(3, "review", {{"session", FakeTasksCli::pinned("codex/api")}, {"summary", "Fixed."}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    QSignalSpy counts(&view, &TasksView::countsChanged);
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 2);
    QCOMPARE(tile(view, "review")->property("count").toInt(), 1);
    // In review opens first while it has tasks.
    QVERIFY(tile(view, "review")->isChecked()); QCOMPARE(view.selectedStage(), QString("review"));
    QCOMPARE(visibleRows(view), QStringList({"T-000003"}));
    QVERIFY(!counts.isEmpty()); QCOMPARE(counts.last().value(0).toInt(), 1); QCOMPARE(counts.last().value(1).toInt(), 2);
    QCOMPARE(FakeTasksCli::calls(dir.path()).first(), QStringList({"task", "list", "--project", "project-1"}));
    tile(view, "todo")->click(); QCOMPARE(visibleRows(view), QStringList({"T-000001", "T-000002"}));
    QVERIFY(view.findChild<QToolButton *>("taskOnlySession")); QVERIFY(view.findChild<QToolButton *>("taskSettings"));
    const auto preview = qEnvironmentVariable("HGS_TASKS_PREVIEW");
    if (!preview.isEmpty()) for (const bool dark : {true, false}) {
        view.setTheme(dark); QTest::qWait(50); QVERIFY(view.grab().save(preview + (dark ? "-dark.png" : "-light.png")));
    }
}

void TestTasksView::rowsShowTheQueueAndTheOpenSessionsTasks() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo", {{"target", FakeTasksCli::pinned("codex/web")}}),
        FakeTasksCli::task(2, "todo", {{"target", FakeTasksCli::pinned("codex/api")}, {"text", "Add rate limiting to POST /v1/orders"}}),
        FakeTasksCli::task(3, "todo"),
        FakeTasksCli::task(4, "active", {{"session", FakeTasksCli::pinned("codex/web")}, {"author", QJsonObject{{"kind", "agent"}, {"machine", "m"}, {"session", "codex/web"}}}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_VERIFY(row(view, 2)); tile(view, "todo")->click();
    QCOMPARE(row(view, 1)->findChild<QLabel *>("taskGlyph")->text(), QString("1"));
    QCOMPARE(row(view, 2)->findChild<QLabel *>("taskGlyph")->text(), QString("2"));
    // The open session's own task is highlighted; the others are not.
    QVERIFY(row(view, 2)->property("mine").toBool()); QVERIFY(!row(view, 1)->property("mine").toBool());
    QVERIFY(row(view, 2)->findChild<QLabel *>("taskText")->text().startsWith("Add rate limiting"));
    QVERIFY(row(view, 1)->findChild<QLabel *>("taskMeta")->text().contains("codex/web"));
    QVERIFY(row(view, 3)->findChild<QLabel *>("taskMeta")->text().contains("any session"));
    for (const char *name : {"edit", "toBacklog", "top", "sendNow"}) QVERIFY2(action(row(view, 1), name), name);
    tile(view, "active")->click();
    const auto meta = row(view, 4)->findChild<QLabel *>("taskMeta")->text();
    QVERIFY2(meta.contains("Working") && meta.contains("filed by codex/web"), qPrintable(meta));
    QVERIFY(action(row(view, 4), "cancel"));
}

void TestTasksView::actionsRunHgsAndRefresh() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"), FakeTasksCli::task(2, "todo"),
        FakeTasksCli::task(3, "review", {{"session", FakeTasksCli::pinned("codex/api")}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 2); tile(view, "todo")->click();
    action(row(view, 2), "top")->click();
    QTRY_COMPARE(visibleRows(view), QStringList({"T-000002", "T-000001"}));
    QVERIFY(FakeTasksCli::calls(dir.path()).contains(QStringList({"task", "move", "uuid-2", "--top", "--project", "project-1"})));
    action(row(view, 1), "toBacklog")->click();
    QTRY_COMPARE(tile(view, "backlog")->property("count").toInt(), 1);
    // An Any session task goes to the open session.
    action(row(view, 2), "sendNow")->click();
    QTRY_COMPARE(tile(view, "active")->property("count").toInt(), 1);
    QVERIFY(FakeTasksCli::calls(dir.path()).contains(QStringList({"task", "send", "uuid-2", "--session", "codex/api", "--project", "project-1"})));
    tile(view, "review")->click(); action(row(view, 3), "accept")->click();
    QTRY_COMPARE(tile(view, "done")->property("count").toInt(), 1);
    QVERIFY(FakeTasksCli::calls(dir.path()).contains(QStringList({"task", "stage", "uuid-3", "done", "--project", "project-1"})));
    // A refused command is reported and changes nothing.
    QFile fail(dir.filePath("fail")); QVERIFY(fail.open(QIODevice::WriteOnly)); fail.write("stage"); fail.close();
    QSignalSpy notices(&view, &TasksView::notice);
    tile(view, "done")->click(); action(row(view, 3), "backToTodo")->click();
    QTRY_COMPARE(notices.size(), 1); QCOMPARE(notices[0][0].toString(), QString("hgs refused: stage")); QVERIFY(notices[0][1].toBool());
    QCOMPARE(tile(view, "done")->property("count").toInt(), 1);
}

void TestTasksView::sendNowNamesWhyASessionCannotTakeIt() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {
        FakeTasksCli::task(1, "todo", {{"target", FakeTasksCli::pinned("codex/gone")}}),
        FakeTasksCli::task(2, "todo", {{"target", FakeTasksCli::pinned("codex/stopped")}}),
        FakeTasksCli::task(3, "todo", {{"target", FakeTasksCli::pinned("codex/ios", "orchard-mac")}}),
        FakeTasksCli::task(4, "todo", {{"target", FakeTasksCli::pinned("codex/web")}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_VERIFY(row(view, 4)); tile(view, "todo")->click();
    const auto blocked = [&](int n) { auto *b = action(row(view, n), "sendNow"); return b->isEnabled() ? QString() : b->toolTip(); };
    QCOMPARE(blocked(1), QString("codex/gone no longer exists — choose another session"));
    QCOMPARE(blocked(2), QString("The agent is not running. Resume it before sending."));
    QCOMPARE(blocked(3), QString("codex/ios runs on another computer; sending there comes with replication"));
    QCOMPARE(blocked(4), QString());
}

void TestTasksView::theInputFilesATaskForTheOpenSession() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path()));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    auto *composer = view.composer(); auto *send = composer->findChild<QPushButton *>("sendMessage");
    QTRY_VERIFY(!composer->isDraftLoading(composer->sessionKey()));
    QCOMPARE(composer->sessionKey(), QString("task-draft\n\ncodex/api")); QCOMPARE(send->text(), QString("To do"));
    composer->editor()->setFocus(); QTest::keyClicks(composer->editor(), "Add rate limiting");
    QTest::keyClick(composer->editor(), Qt::Key_Return);
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 1);
    QTRY_VERIFY(composer->editor()->toPlainText().isEmpty());
    QVERIFY(FakeTasksCli::calls(dir.path()).contains(QStringList({"task", "add", "--project", "project-1", "--target", "codex/api"})));
    QVERIFY(FakeTasksCli::stdins(dir.path()).contains("Add rate limiting"));
    // While Backlog is open, a new task goes there; a remote session names its computer.
    view.setContext("project-1", "orchard", "orchard-mac", "codex/ios", "orchard-mac\ncodex/ios", sessions());
    tile(view, "backlog")->click(); QCOMPARE(send->text(), QString("Backlog"));
    QTRY_VERIFY(!composer->isDraftLoading(composer->sessionKey()));
    QTest::keyClicks(composer->editor(), "Ship the iOS build"); QTest::keyClick(composer->editor(), Qt::Key_Return);
    QTRY_COMPARE(tile(view, "backlog")->property("count").toInt(), 1);
    QVERIFY(FakeTasksCli::calls(dir.path()).contains(QStringList({"task", "add", "--project", "project-1", "--target", "codex/ios", "--host", "orchard-mac", "--backlog"})));
    // A refused add keeps the prompt and reports why.
    QFile fail(dir.filePath("fail")); QVERIFY(fail.open(QIODevice::WriteOnly)); fail.write("add"); fail.close();
    QSignalSpy notices(&view, &TasksView::notice);
    QTest::keyClicks(composer->editor(), "Kept"); QTest::keyClick(composer->editor(), Qt::Key_Return);
    QTRY_COMPARE(notices.size(), 1); QCOMPARE(notices[0][0].toString(), QString("hgs refused: add"));
    QTRY_COMPARE(composer->editor()->toPlainText(), QString("Kept"));
}

void TestTasksView::onlyThisSessionFiltersAndIsRemembered() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo", {{"target", FakeTasksCli::pinned("codex/web")}}),
        FakeTasksCli::task(2, "todo", {{"target", FakeTasksCli::pinned("codex/api")}}), FakeTasksCli::task(3, "todo")}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 3);
    auto *only = view.findChild<QToolButton *>("taskOnlySession"); only->click();
    QCOMPARE(tile(view, "todo")->property("count").toInt(), 2); QCOMPARE(visibleRows(view), QStringList({"T-000002", "T-000003"}));
    // Queue numbers stay the project's.
    QCOMPARE(row(view, 3)->findChild<QLabel *>("taskGlyph")->text(), QString("3"));
    QVERIFY(QSettings().value("tasks/onlySession").toBool());
    TasksView again(&client); again.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QVERIFY(again.findChild<QToolButton *>("taskOnlySession")->isChecked());
}

void TestTasksView::doneShowsTheLastWeekAndOlderOnRequest() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "done", {{"stage_at", now() - 2 * Hour}}),
        FakeTasksCli::task(2, "done", {{"stage_at", now() - 10 * 24 * Hour}}), FakeTasksCli::task(3, "done", {{"stage_at", now() - Hour}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(tile(view, "done")->property("count").toInt(), 2);
    tile(view, "done")->click(); QCOMPARE(visibleRows(view), QStringList({"T-000003", "T-000001"}));
    auto *older = view.findChild<QPushButton *>("showOlderTasks"); QVERIFY(older->isVisible()); QCOMPARE(older->text(), QString("Show older (1)"));
    older->click(); QCOMPARE(visibleRows(view), QStringList({"T-000003", "T-000001", "T-000002"})); QCOMPARE(older->text(), QString("Hide older"));
}

void TestTasksView::aFailedListingIsShownInsteadOfTasks() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo")}));
    QFile fail(dir.filePath("fail")); QVERIFY(fail.open(QIODevice::WriteOnly)); fail.write("list"); fail.close();
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    auto *error = view.findChild<QLabel *>("tasksError");
    QTRY_VERIFY(error->isVisible()); QVERIFY(error->text().contains("hgs refused: list"));
    QVERIFY(visibleRows(view).isEmpty());
}

void TestTasksView::aSessionOutsideProjectsExplainsWhy() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path()));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext({}, {}, "", "codex/api", "\ncodex/api", sessions());
    auto *empty = view.findChild<QLabel *>("taskEmpty");
    QVERIFY(empty->isVisible()); QVERIFY(empty->text().startsWith("Tasks belong to a project"));
    QVERIFY(!view.composer()->findChild<QPushButton *>("sendMessage")->isEnabled());
    QTest::qWait(100); QVERIFY(FakeTasksCli::calls(dir.path()).isEmpty());
}

void TestTasksView::theTabBadgeCountsReviewAndTodo() {
    QWidget host; TaskTabBadge badge(&host); host.show();
    QVERIFY(!badge.isVisible());
    badge.setCounts(1, 12); QVERIFY(badge.isVisible());
    QCOMPARE(badge.property("review").toInt(), 1); QCOMPARE(badge.property("todo").toInt(), 12);
    QCOMPARE(badge.accessibleName(), QString("1 to review, 12 to do"));
    QVERIFY(badge.sizeHint().width() > badge.sizeHint().height());
    badge.setCounts(0, 0); QVERIFY(!badge.isVisible());
}

QTEST_MAIN(TestTasksView)
#include "test_tasksview.moc"
