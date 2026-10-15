#include "KeyboardSettings.h"
#include "ShortcutsDialog.h"
#include <QApplication>
#include <QDialogButtonBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest>

using Shortcuts::Platform;

class TestKeyboardSettings : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup() { QSettings().clear(); }
    void listsEveryCommandOnce();
    void editingPersistsAChord();
    void protectedKeyIsRefusedWithAReason();
    void conflictAsksAndReassigns();
    void conflictDeclinedChangesNothing();
    void doubleClickOpensTheEditorOnce();
    void samePhraseInBothSlotsCollapses();
    void resetCommandAndResetAll();
    void marksChordsThatWorkInTerminal();
    void cheatSheetFiltersAndUpdates();
    void cheatSheetListsFixedKeys();
    void customizeIsRequested();
private:
    static QTreeWidgetItem *commandItem(QTreeWidget *tree, const QString &id);
    // Runs `action` against the next modal dialog or message box shortly after it opens.
    static void whenModal(const std::function<void(QWidget *)> &action);
    QTemporaryDir m_settings;
};

void TestKeyboardSettings::init()
{
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
    QSettings().clear();
}

QTreeWidgetItem *TestKeyboardSettings::commandItem(QTreeWidget *tree, const QString &id)
{
    QTreeWidgetItemIterator it(tree);
    for (; *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toString() == id) return *it;
    return nullptr;
}

void TestKeyboardSettings::whenModal(const std::function<void(QWidget *)> &action)
{
    QTimer::singleShot(0, [action] {
        if (QWidget *modal = QApplication::activeModalWidget()) action(modal);
        else QTimer::singleShot(20, [action] { if (QWidget *again = QApplication::activeModalWidget()) action(again); });
    });
}

void TestKeyboardSettings::listsEveryCommandOnce()
{
    ShortcutRegistry registry(Platform::Linux);
    KeyboardSettings settings(registry);
    auto *tree = settings.findChild<QTreeWidget *>("keyboardCommands");
    QVERIFY(tree);
    QCOMPARE(settings.objectName(), QString("keyboardSettings"));
    QStringList ids;
    QTreeWidgetItemIterator it(tree);
    for (; *it; ++it)
        if (const QString id = (*it)->data(0, Qt::UserRole).toString(); !id.isEmpty()) ids.append(id);
    QCOMPARE(ids.size(), Shortcuts::commands().size());
    QCOMPARE(QSet<QString>(ids.begin(), ids.end()).size(), ids.size());
    for (const auto &command : Shortcuts::commands()) {
        auto *item = commandItem(tree, command.id);
        QVERIFY(item);
        QCOMPARE(item->text(0), Shortcuts::title(command));
        QCOMPARE(item->parent()->text(0), Shortcuts::group(command));
    }
    QVERIFY(settings.findChild<QLabel *>("keyboardTerminalNote"));
}

void TestKeyboardSettings::editingPersistsAChord()
{
    ShortcutRegistry registry(Platform::Linux);
    KeyboardSettings settings(registry);
    QSignalSpy changed(&registry, &ShortcutRegistry::changed);
    whenModal([](QWidget *dialog) {
        dialog->findChild<QKeySequenceEdit *>("keyboardEdit")->setKeySequence(QKeySequence("Ctrl+J"));
        QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
    });
    auto *tree = settings.findChild<QTreeWidget *>("keyboardCommands");
    emit tree->itemActivated(commandItem(tree, "go.anywhere"), 1);
    QCOMPARE(registry.keys("go.anywhere"), QList<QKeySequence>{QKeySequence("Ctrl+J")});
    QVERIFY(changed.count() >= 1);
    QCOMPARE(commandItem(tree, "go.anywhere")->text(1).left(6), QKeySequence("Ctrl+J").toString(QKeySequence::NativeText));
    ShortcutRegistry reopened(Platform::Linux);
    QCOMPARE(reopened.keys("go.anywhere"), QList<QKeySequence>{QKeySequence("Ctrl+J")});
}

void TestKeyboardSettings::protectedKeyIsRefusedWithAReason()
{
    ShortcutRegistry registry(Platform::Linux);
    KeyboardSettings settings(registry);
    QString shown;
    bool visible = false;
    whenModal([&](QWidget *dialog) {
        dialog->findChild<QKeySequenceEdit *>("keyboardEdit")->setKeySequence(QKeySequence(Qt::Key_Return));
        QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
        auto *reason = dialog->findChild<QLabel *>("keyboardEditReason");
        shown = reason->text();
        visible = !reason->isHidden();
        const bool stillOpen = dialog->isVisible();
        QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
        QVERIFY(stillOpen);
    });
    settings.editChord("go.anywhere", 0);
    QVERIFY(visible);
    QVERIFY(!shown.isEmpty());
    QVERIFY(!registry.customized("go.anywhere"));
}

void TestKeyboardSettings::conflictAsksAndReassigns()
{
    ShortcutRegistry registry(Platform::Linux);
    KeyboardSettings settings(registry);
    QString question;
    whenModal([&](QWidget *dialog) {
        dialog->findChild<QKeySequenceEdit *>("keyboardEdit")->setKeySequence(QKeySequence("Ctrl+N"));
        QTimer::singleShot(30, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box) return;
            question = box->text();
            QTest::mouseClick(box->button(QMessageBox::Yes), Qt::LeftButton);
        });
        QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
    });
    settings.editChord("go.anywhere", 0);
    QVERIFY2(question.contains(Shortcuts::title(*Shortcuts::command("session.new"))), qPrintable(question));
    QCOMPARE(registry.keys("go.anywhere").first(), QKeySequence("Ctrl+N"));
    QVERIFY(registry.keys("session.new").isEmpty());
}

void TestKeyboardSettings::conflictDeclinedChangesNothing()
{
    ShortcutRegistry registry(Platform::Linux);
    KeyboardSettings settings(registry);
    whenModal([&](QWidget *dialog) {
        dialog->findChild<QKeySequenceEdit *>("keyboardEdit")->setKeySequence(QKeySequence("Ctrl+N"));
        QTimer::singleShot(30, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) QTest::mouseClick(box->button(QMessageBox::No), Qt::LeftButton);
        });
        QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
        QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    });
    settings.editChord("go.anywhere", 0);
    QVERIFY(!registry.customized("go.anywhere"));
    QVERIFY(!registry.customized("session.new"));
}

void TestKeyboardSettings::doubleClickOpensTheEditorOnce()
{
    ShortcutRegistry registry(Platform::Linux);
    KeyboardSettings settings(registry);
    settings.resize(700, 700);
    settings.show();
    settings.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&settings));
    auto *tree = settings.findChild<QTreeWidget *>("keyboardCommands");
    auto *item = commandItem(tree, "go.anywhere");
    tree->scrollToItem(item);
    int opened = 0;
    std::function<void()> close = [&] {
        if (QWidget *modal = QApplication::activeModalWidget()) {
            ++opened;
            qobject_cast<QDialog *>(modal)->reject();
            QTimer::singleShot(30, close);
        }
    };
    QTimer::singleShot(30, close);
    const QPoint point = tree->visualItemRect(item).center();
    // QTest delivers the double-click event only after a first click.
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(tree->columnViewportPosition(1) + 5, point.y()));
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(tree->columnViewportPosition(1) + 5, point.y()));
    QTest::qWait(150);
    QCOMPARE(opened, 1);
}

void TestKeyboardSettings::samePhraseInBothSlotsCollapses()
{
    ShortcutRegistry registry(Platform::Linux);
    KeyboardSettings settings(registry);
    registry.setKeys("go.anywhere", {QKeySequence("Ctrl+J"), QKeySequence("Ctrl+L")});
    whenModal([](QWidget *dialog) {
        dialog->findChild<QKeySequenceEdit *>("keyboardEdit")->setKeySequence(QKeySequence("Ctrl+J"));
        QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
    });
    settings.editChord("go.anywhere", 1);
    QCOMPARE(registry.keys("go.anywhere"), QList<QKeySequence>{QKeySequence("Ctrl+J")});
}

void TestKeyboardSettings::resetCommandAndResetAll()
{
    ShortcutRegistry registry(Platform::Linux);
    KeyboardSettings settings(registry);
    registry.setKeys("go.anywhere", {QKeySequence("Ctrl+J")});
    registry.setKeys("session.new", {});
    auto *tree = settings.findChild<QTreeWidget *>("keyboardCommands");
    tree->setCurrentItem(commandItem(tree, "go.anywhere"));
    QTest::mouseClick(settings.findChild<QPushButton *>("keyboardResetCommand"), Qt::LeftButton);
    QVERIFY(!registry.customized("go.anywhere"));
    QVERIFY(registry.customized("session.new"));

    // Declining keeps the override.
    whenModal([](QWidget *box) { QTest::mouseClick(qobject_cast<QMessageBox *>(box)->button(QMessageBox::No), Qt::LeftButton); });
    QTest::mouseClick(settings.findChild<QPushButton *>("keyboardResetAll"), Qt::LeftButton);
    QVERIFY(registry.customized("session.new"));
    whenModal([](QWidget *box) { QTest::mouseClick(qobject_cast<QMessageBox *>(box)->button(QMessageBox::Yes), Qt::LeftButton); });
    QTest::mouseClick(settings.findChild<QPushButton *>("keyboardResetAll"), Qt::LeftButton);
    QVERIFY(!registry.customized("session.new"));
    QVERIFY(!registry.keys("session.new").isEmpty());
}

void TestKeyboardSettings::marksChordsThatWorkInTerminal()
{
    ShortcutRegistry registry(Platform::Linux);
    KeyboardSettings settings(registry);
    auto *tree = settings.findChild<QTreeWidget *>("keyboardCommands");
    auto *tab = commandItem(tree, "tab.next");
    auto *search = commandItem(tree, "go.anywhere");
    QVERIFY(tab->text(1).contains(QStringLiteral("✓")));
    QCOMPARE(tab->toolTip(1), QString("Works in Terminal"));
    QVERIFY(!search->text(1).contains(QStringLiteral("✓")));
    QVERIFY(search->toolTip(1).isEmpty());
}

void TestKeyboardSettings::cheatSheetFiltersAndUpdates()
{
    ShortcutRegistry registry(Platform::Linux);
    ShortcutsDialog dialog(registry);
    QCOMPARE(dialog.objectName(), QString("shortcutsDialog"));
    QVERIFY(!dialog.isModal());
    auto *list = dialog.findChild<QTreeWidget *>("shortcutsList");
    auto count = [&] {
        int n = 0;
        QTreeWidgetItemIterator it(list);
        for (; *it; ++it) if ((*it)->parent()) ++n;
        return n;
    };
    QVERIFY(count() >= Shortcuts::commands().size());
    dialog.findChild<QLineEdit *>("shortcutsFilter")->setText("TAB");
    QTreeWidgetItemIterator it(list);
    QStringList titles;
    for (; *it; ++it)
        if ((*it)->parent()) {
            titles.append((*it)->text(0));
            QVERIFY2((*it)->text(0).contains("tab", Qt::CaseInsensitive) || (*it)->text(1).contains("tab", Qt::CaseInsensitive), qPrintable((*it)->text(0)));
        }
    QVERIFY(titles.contains(Shortcuts::title(*Shortcuts::command("tab.next"))));
    QVERIFY(titles.contains(Shortcuts::title(*Shortcuts::command("tab.previous"))));
    QVERIFY(!titles.contains(Shortcuts::title(*Shortcuts::command("go.overview"))));

    dialog.findChild<QLineEdit *>("shortcutsFilter")->setText("Go to Overview");
    registry.setKeys("go.overview", {QKeySequence("Ctrl+J")});
    bool found = false;
    QTreeWidgetItemIterator again(list);
    for (; *again; ++again)
        if ((*again)->text(0) == "Go to Overview") {
            found = true;
            QCOMPARE((*again)->text(1), QKeySequence("Ctrl+J").toString(QKeySequence::NativeText));
        }
    QVERIFY(found);
}

void TestKeyboardSettings::cheatSheetListsFixedKeys()
{
    ShortcutRegistry registry(Platform::Linux);
    ShortcutsDialog dialog(registry);
    auto *list = dialog.findChild<QTreeWidget *>("shortcutsList");
    bool fixedGroup = false;
    for (int i = 0; i < list->topLevelItemCount(); ++i)
        if (list->topLevelItem(i)->text(0) == "Fixed keys" && list->topLevelItem(i)->childCount() > 5) fixedGroup = true;
    QVERIFY(fixedGroup);
}

void TestKeyboardSettings::customizeIsRequested()
{
    ShortcutRegistry registry(Platform::Linux);
    ShortcutsDialog dialog(registry);
    QSignalSpy requested(&dialog, &ShortcutsDialog::customizeRequested);
    QTest::mouseClick(dialog.findChild<QPushButton *>("shortcutsCustomize"), Qt::LeftButton);
    QCOMPARE(requested.count(), 1);
    QVERIFY(dialog.findChild<QLineEdit *>("shortcutsFilter"));
}

QTEST_MAIN(TestKeyboardSettings)
#include "test_keyboardsettings.moc"
