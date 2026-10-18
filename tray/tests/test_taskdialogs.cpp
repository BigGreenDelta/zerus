#include "FakeTasksCli.h"
#include "HgsClient.h"
#include "TaskDialogs.h"
#include "TasksView.h"
#include <QAbstractButton>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QToolButton>

namespace {
QList<TaskSession> sessions() {
    TaskSession stopped{"", "codex/stopped", "Stopped", "The agent is not running. Resume it before sending."}; stopped.running = false;
    return {{"", "codex/api", "Ready", ""}, {"", "codex/web", "Working", ""}, stopped};
}
QJsonObject withLog(QJsonObject task, const QJsonArray &log) { task["log"] = log; return task; }
QJsonObject entry(const QString &kind, const QString &text = {}, const QJsonObject &extra = {}) {
    QJsonObject e{{"kind", kind}, {"actor", QJsonObject{{"kind", "person"}}}, {"at", 1760000000000.0}, {"text", text}};
    for (auto it = extra.begin(); it != extra.end(); ++it) e[it.key()] = it.value();
    return e;
}
QWidget *row(TasksView &view, int n) { return view.findChild<QWidget *>(QString("taskRow:T-%1").arg(n, 6, 10, QLatin1Char('0'))); }
QAbstractButton *tile(TasksView &view, const QString &stage) { return view.findChild<QAbstractButton *>("taskTile:" + stage); }
void click(QWidget *widget) { QTest::mouseClick(widget, Qt::LeftButton, {}, QPoint(widget->width() / 2, 8)); }
bool called(const QDir &dir, const QStringList &argv) { return FakeTasksCli::calls(dir).contains(argv); }
}

class TestTaskDialogs : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings;
private slots:
    void initTestCase();
    void theTaskDialogEditsAListedTaskAndShowsItsHistory();
    void aReviewTaskIsReadOnlyWithItsActions();
    void theCancelAndProjectSettingsDialogs();
    void theTabOpensTheDialogsAndAppliesThem();
    void manySessionsWrapInTheDialog();
    void aRefusedSaveKeepsTheDialogAndItsEdits();
    void sendNowFollowsTheChosenSession();
    void anEmptyPromptCannotBeApplied();
};

void TestTaskDialogs::initTestCase() {
    QVERIFY(m_settings.isValid());
    QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("task-dialogs");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
}

void TestTaskDialogs::theTaskDialogEditsAListedTaskAndShowsItsHistory() {
    const auto task = withLog(FakeTasksCli::task(7, "todo", {{"target", FakeTasksCli::pinned("codex/gone")}, {"text", "Old prompt"}}),
        {entry("created"), entry("sent", {}, {{"session", QJsonObject{{"session", "codex/api"}, {"host", ""}}}}), entry("comment", "Use the config value."),
         entry("comment", "Seen on the Mac.", {{"actor", QJsonObject{{"kind", "agent"}, {"session", "codex/ios"}, {"host", "orchard-mac"}}}})});
    TaskDialog dialog(task, sessions(), {}, true); dialog.show();
    auto *prompt = dialog.findChild<QPlainTextEdit *>("taskPrompt"); QVERIFY(!prompt->isReadOnly());
    // Any session, the project's sessions, and the pinned session that no longer exists.
    for (const char *name : {"taskSession:any", "taskSession:/codex/api", "taskSession:/codex/web", "taskSession:/codex/gone"})
        QVERIFY2(dialog.findChild<QPushButton *>(name), name);
    QVERIFY(dialog.findChild<QPushButton *>("taskSession:/codex/gone")->isChecked());
    const auto history = dialog.findChild<QTextBrowser *>("taskHistory")->toPlainText();
    QVERIFY2(history.contains("created the task") && history.contains("sent it to codex/api") && history.contains("commented: Use the config value."), qPrintable(history));
    QVERIFY2(history.contains("codex/ios @orchard-mac"), qPrintable(history));   // an agent on another computer
    for (const char *name : {"taskDialogAction:toBacklog", "taskDialogAction:top", "taskDialogAction:sendNow"}) QVERIFY2(dialog.findChild<QPushButton *>(name), name);
    QVERIFY(!dialog.findChild<QPushButton *>("taskDialogAction:edit"));
    QSignalSpy saved(&dialog, &TaskDialog::saveRequested);
    prompt->setPlainText("New prompt"); dialog.findChild<QPushButton *>("taskSession:any")->click();
    dialog.findChild<QPushButton *>("saveTask")->click();
    QCOMPARE(saved.size(), 1); const auto edits = saved[0][0].value<TaskEdits>();
    QCOMPARE(edits.text.value_or(QString()), QString("New prompt")); QVERIFY(edits.target && edits.target->any);
    // The dialog waits for its owner: it closes once hgs has taken the edits.
    QVERIFY(dialog.isVisible()); QVERIFY(!dialog.findChild<QPushButton *>("saveTask")->isEnabled());
    dialog.setError("hgs refused: edit");
    QCOMPARE(dialog.findChild<QLabel *>("taskDialogError")->text(), QString("hgs refused: edit"));
    QVERIFY(dialog.findChild<QPushButton *>("saveTask")->isEnabled()); QCOMPARE(prompt->toPlainText(), QString("New prompt"));
}

void TestTaskDialogs::aReviewTaskIsReadOnlyWithItsActions() {
    const auto task = withLog(FakeTasksCli::task(8, "review", {{"session", FakeTasksCli::pinned("codex/api")}, {"summary", "Token bucket; 6 tests."}}), {});
    TaskDialog dialog(task, sessions(), {}, false); dialog.show();
    QVERIFY(dialog.findChild<QPlainTextEdit *>("taskPrompt")->isReadOnly());
    QVERIFY(!dialog.findChild<QPushButton *>("saveTask")); QVERIFY(dialog.findChild<QPushButton *>("closeTask"));
    QCOMPARE(dialog.findChild<QLabel *>("taskDialogSession")->text(), QString("codex/api"));
    QVERIFY(dialog.findChild<QLabel *>("taskDialogSummary")->text().contains("Token bucket; 6 tests."));
    QSignalSpy acted(&dialog, &TaskDialog::actionRequested);
    QVERIFY(dialog.findChild<QPushButton *>("taskDialogAction:backToTodo"));
    dialog.findChild<QPushButton *>("taskDialogAction:accept")->click();
    QCOMPARE(acted.size(), 1); QCOMPARE(acted[0][1].value<TaskAction>(), TaskAction::Accept);
    const auto edits = acted[0][0].value<TaskEdits>(); QVERIFY(!edits.text && !edits.target);
}

void TestTaskDialogs::theCancelAndProjectSettingsDialogs() {
    auto item = TaskBoard::item(FakeTasksCli::task(9, "active", {{"session", FakeTasksCli::pinned("codex/web")}}));
    TaskCancelDialog working(item, true, true);
    QCOMPARE(working.findChild<QLabel *>("taskCancelWarning")->text(),
             QString("T-000009 moves to In review, and codex/web gets the cancel message. codex/web is working; it reads the message when its turn ends."));
    QVERIFY(working.findChild<QPushButton *>("keepTaskRunning")->isDefault());
    working.findChild<QLineEdit *>("taskCancelReason")->setText("  Wrong approach  "); QCOMPARE(working.reason(), QString("Wrong approach"));
    TaskCancelDialog stopped(item, false, false);
    TaskCancelDialog idle(item, true, false);
    QCOMPARE(idle.findChild<QLabel *>("taskCancelWarning")->text(), QString("T-000009 moves to In review, and codex/web gets the cancel message."));
    QVERIFY(stopped.findChild<QLabel *>("taskCancelWarning")->text().contains("The agent is not running, so hgs will refuse the message"));
    const QJsonObject settings{{"task_template", "[Zerus task {ref}]\n{prompt}"}, {"cancel_template", "[Zerus task {ref} cancelled]"},
                               {"agents_other", true}, {"agent_limit", 0}};
    TaskSettingsDialog dialog("orchard", settings);
    QCOMPARE(dialog.windowTitle(), QString("Project tasks · orchard")); QVERIFY(dialog.changes().isEmpty());
    QCOMPARE(dialog.findChild<QSpinBox *>("agentLimit")->text(), QString("no limit"));
    dialog.findChild<QSpinBox *>("agentLimit")->setValue(3); dialog.findChild<QCheckBox *>("agentsOther")->setChecked(false);
    QCOMPARE(dialog.changes(), QJsonObject({{"agent_limit", 3}, {"agents_other", false}}));
}

void TestTaskDialogs::theTabOpensTheDialogsAndAppliesThem() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"),
        FakeTasksCli::task(2, "active", {{"session", FakeTasksCli::pinned("codex/web")}}),
        FakeTasksCli::task(3, "review", {{"session", FakeTasksCli::pinned("codex/api")}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 1); tile(view, "todo")->click();
    // A click on the row opens its settings; an action there first applies the edits.
    click(row(view, 1)); TaskDialog *dialog = nullptr; QTRY_VERIFY((dialog = view.findChild<TaskDialog *>("taskDialog")));
    QVERIFY(called(dir.path(), {"task", "get", "uuid-1", "--project", "project-1"}));
    dialog->findChild<QPlainTextEdit *>("taskPrompt")->setPlainText("Edited prompt");
    dialog->findChild<QPushButton *>("taskSession:/codex/web")->click();
    dialog->findChild<QPushButton *>("taskDialogAction:sendNow")->click();
    QTRY_COMPARE(tile(view, "active")->property("count").toInt(), 2);
    const auto calls = FakeTasksCli::calls(dir.path());
    const int edit = calls.indexOf(QStringList({"task", "edit", "uuid-1", "--project", "project-1"}));
    const int target = calls.indexOf(QStringList({"task", "target", "uuid-1", "codex/web", "--project", "project-1"}));
    const int send = calls.indexOf(QStringList({"task", "send", "uuid-1", "--project", "project-1"}));
    QVERIFY(edit >= 0 && edit < target && target < send);
    QVERIFY(FakeTasksCli::stdins(dir.path()).contains("Edited prompt"));
    // Cancel asks first; the reason goes to hgs as a comment, never to the agent.
    tile(view, "active")->click(); row(view, 2)->findChild<QToolButton *>("taskAction:cancel")->click();
    TaskCancelDialog *cancel = nullptr; QTRY_VERIFY((cancel = view.findChild<TaskCancelDialog *>("taskCancelDialog")));
    cancel->findChild<QLineEdit *>("taskCancelReason")->setText("Wrong approach");
    cancel->findChild<QPushButton *>("confirmTaskCancel")->click();
    QTRY_VERIFY(called(dir.path(), {"task", "cancel", "uuid-2", "--reason", "Wrong approach", "--project", "project-1"}));
    QTRY_COMPARE(tile(view, "review")->property("count").toInt(), 2);
    // A comment is added in place and the history follows.
    tile(view, "review")->click(); click(row(view, 3));
    QTRY_VERIFY((dialog = view.findChild<TaskDialog *>("taskDialog")) && dialog->isVisible());
    dialog->findChild<QLineEdit *>("taskComment")->setText("Looks good");
    dialog->findChild<QPushButton *>("addTaskComment")->click();
    QTRY_VERIFY(dialog->findChild<QTextBrowser *>("taskHistory")->toPlainText().contains("commented: Looks good"));
    QVERIFY(dialog->findChild<QLineEdit *>("taskComment")->text().isEmpty());
    QVERIFY(called(dir.path(), {"task", "comment", "uuid-3", "--project", "project-1"}));
    dialog->reject();
    // The gear opens the project's task settings; Save sends only what changed.
    view.findChild<QToolButton *>("taskSettings")->click();
    TaskSettingsDialog *settings = nullptr; QTRY_VERIFY((settings = view.findChild<TaskSettingsDialog *>("taskSettingsDialog")));
    settings->findChild<QSpinBox *>("agentLimit")->setValue(2); settings->findChild<QPushButton *>("saveTaskSettings")->click();
    QTRY_VERIFY(called(dir.path(), {"task", "settings", "--project", "project-1", "--set"}));
    QCOMPARE(QJsonDocument::fromJson(FakeTasksCli::stdins(dir.path()).last().toUtf8()).object(), QJsonObject({{"agent_limit", 2}}));
}

void TestTaskDialogs::manySessionsWrapInTheDialog() {
    QList<TaskSession> many;
    for (int i = 0; i < 40; ++i) many << TaskSession{"", QString("codex/session-%1").arg(i), "Ready", ""};
    TaskDialog dialog(withLog(FakeTasksCli::task(1, "todo"), {}), many, {}, true); dialog.resize(640, 640); dialog.show();
    QVERIFY(dialog.sizeHint().width() < 900);
    auto *first = dialog.findChild<QPushButton *>("taskSession:any"), *last = dialog.findChild<QPushButton *>("taskSession:/codex/session-39");
    QTRY_VERIFY(last->mapTo(&dialog, QPoint()).y() > first->mapTo(&dialog, QPoint()).y());   // wrapped onto later rows
    QVERIFY(dialog.findChild<QWidget *>("taskSessionChoices")->height() <= 104);
}

void TestTaskDialogs::aRefusedSaveKeepsTheDialogAndItsEdits() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo")}));
    QFile fail(dir.filePath("fail")); QVERIFY(fail.open(QIODevice::WriteOnly)); fail.write("edit"); fail.close();
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 1); tile(view, "todo")->click();
    click(row(view, 1)); TaskDialog *dialog = nullptr; QTRY_VERIFY((dialog = view.findChild<TaskDialog *>("taskDialog")));
    dialog->findChild<QPlainTextEdit *>("taskPrompt")->setPlainText("A prompt worth keeping");
    dialog->findChild<QPushButton *>("saveTask")->click();
    QTRY_COMPARE(dialog->findChild<QLabel *>("taskDialogError")->text(), QString("hgs refused: edit"));
    QVERIFY(dialog->isVisible()); QCOMPARE(dialog->findChild<QPlainTextEdit *>("taskPrompt")->toPlainText(), QString("A prompt worth keeping"));
    QVERIFY(QFile::remove(dir.filePath("fail")));
    dialog->findChild<QPushButton *>("saveTask")->click();
    QTRY_VERIFY(!view.findChild<TaskDialog *>("taskDialog"));
    QVERIFY(FakeTasksCli::stdins(dir.path()).contains("A prompt worth keeping"));
    // The project settings keep a refused change open too.
    QVERIFY(fail.open(QIODevice::WriteOnly)); fail.write("settings --set"); fail.close();
    view.findChild<QToolButton *>("taskSettings")->click();
    TaskSettingsDialog *settings = nullptr; QTRY_VERIFY((settings = view.findChild<TaskSettingsDialog *>("taskSettingsDialog")));
    settings->findChild<QSpinBox *>("agentLimit")->setValue(4); settings->findChild<QPushButton *>("saveTaskSettings")->click();
    QTRY_COMPARE(settings->findChild<QLabel *>("taskSettingsError")->text(), QString("hgs refused: settings"));
    QVERIFY(settings->isVisible()); QCOMPARE(settings->findChild<QSpinBox *>("agentLimit")->value(), 4);
}

void TestTaskDialogs::sendNowFollowsTheChosenSession() {
    const auto blocker = [](const TaskTarget &target) {
        return target.session == "codex/gone" ? QString("codex/gone no longer exists — choose another session")
             : target.session == "codex/stopped" ? QString("The agent is not running. Resume it before sending.") : QString();
    };
    TaskDialog dialog(withLog(FakeTasksCli::task(1, "todo", {{"target", FakeTasksCli::pinned("codex/gone")}}), {}), sessions(), blocker, true);
    dialog.show(); auto *send = dialog.findChild<QPushButton *>("taskDialogAction:sendNow");
    QVERIFY(!send->isEnabled()); QCOMPARE(send->toolTip(), QString("codex/gone no longer exists — choose another session"));
    dialog.findChild<QPushButton *>("taskSession:/codex/api")->click(); QVERIFY(send->isEnabled());
    dialog.findChild<QPushButton *>("taskSession:/codex/stopped")->click(); QVERIFY(!send->isEnabled());
    QCOMPARE(send->toolTip(), QString("The agent is not running. Resume it before sending."));
}

void TestTaskDialogs::anEmptyPromptCannotBeApplied() {
    TaskDialog dialog(withLog(FakeTasksCli::task(1, "todo"), {}), sessions(), {}, true); dialog.show();
    auto *prompt = dialog.findChild<QPlainTextEdit *>("taskPrompt"); auto *save = dialog.findChild<QPushButton *>("saveTask");
    prompt->setPlainText("   \n "); QVERIFY(!save->isEnabled()); QCOMPARE(save->toolTip(), QString("The prompt is empty."));
    QVERIFY(!dialog.findChild<QPushButton *>("taskDialogAction:sendNow")->isEnabled());
    prompt->setPlainText("Back again"); QVERIFY(save->isEnabled()); QVERIFY(dialog.findChild<QPushButton *>("taskDialogAction:sendNow")->isEnabled());
}

QTEST_MAIN(TestTaskDialogs)
#include "test_taskdialogs.moc"
