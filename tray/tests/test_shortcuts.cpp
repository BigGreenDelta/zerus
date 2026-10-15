#include "Shortcuts.h"
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>

using Shortcuts::Platform;

// Stands in for the embedded terminal: like TerminalScreen, it claims every
// shortcut override so its program receives the key.
class TerminalScreen : public QWidget {
    Q_OBJECT
public:
    explicit TerminalScreen(QWidget *parent = nullptr) : QWidget(parent) { setFocusPolicy(Qt::StrongFocus); }
    QList<QKeyCombination> received;
protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
        return QWidget::event(event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        // QTest presses the modifier keys separately first.
        const int key = event->key();
        if (key != Qt::Key_Control && key != Qt::Key_Alt && key != Qt::Key_Shift && key != Qt::Key_Meta)
            received.append(Shortcuts::combination(event));
    }
};

class TestShortcuts : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup() { QSettings().clear(); }
    void defaultsAreAssignableAndUnambiguous_data();
    void defaultsAreAssignableAndUnambiguous();
    void platformDefaultsKeepSystemAndTerminalKeys();
    void fixedAndEditingKeysCannotBeAssigned();
    void terminalKeepsAgentKeys();
    void textFieldsKeepEditingKeys();
    void overridesPersistAsPortableChords();
    void conflictsIncludeSequencePrefixes();
    void routerAppliesTerminalRule();
    void routerAppliesTextFieldRule();
    void cutStartsASequenceOnlyWithoutSelection();
    void routerIgnoresOtherWindows();
private:
    QTemporaryDir m_settings;
};

void TestShortcuts::init()
{
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
    QSettings().clear();
}

void TestShortcuts::defaultsAreAssignableAndUnambiguous_data()
{
    QTest::addColumn<Platform>("platform");
    QTest::newRow("linux") << Platform::Linux;
    QTest::newRow("windows") << Platform::Windows;
    QTest::newRow("macos") << Platform::MacOS;
}

void TestShortcuts::defaultsAreAssignableAndUnambiguous()
{
    QFETCH(Platform, platform);
    QList<QPair<QString, QKeySequence>> all;
    for (const auto &command : Shortcuts::commands()) {
        const auto keys = Shortcuts::defaults(command.id, platform);
        QVERIFY2(!keys.isEmpty(), command.id);
        QVERIFY2(keys.size() <= 2, command.id);
        for (const auto &key : keys) {
            QString reason;
            QVERIFY2(Shortcuts::assignable(key, platform, &reason), qPrintable(QString(command.id) + ": " + reason));
            for (const auto &[other, existing] : std::as_const(all))
                QVERIFY2(existing.matches(key) == QKeySequence::NoMatch && key.matches(existing) == QKeySequence::NoMatch,
                    qPrintable(QString(command.id) + " vs " + other));
            all.append({command.id, key});
        }
    }
    ShortcutRegistry registry(platform);
    for (const auto &command : Shortcuts::commands())
        for (const auto &key : registry.keys(command.id))
            QVERIFY2(registry.conflict(command.id, key).isEmpty(), command.id);
}

void TestShortcuts::platformDefaultsKeepSystemAndTerminalKeys()
{
    const auto portable = [](const QString &id, Platform platform) {
        QStringList chords;
        for (const auto &key : Shortcuts::defaults(id, platform)) chords << key.toString(QKeySequence::PortableText);
        return chords;
    };
    QCOMPARE(portable("tab.next", Platform::Linux), QStringList{"Ctrl+Tab"});
    // Command-Tab and Command-` are the macOS application and window switchers.
    QCOMPARE(portable("tab.next", Platform::MacOS), QStringList{"Meta+Tab"});
    QCOMPARE(portable("tab.terminal", Platform::MacOS), QStringList{"Meta+`"});
    QCOMPARE(portable("session.next", Platform::Windows), (QStringList{"Alt+Down", "Ctrl+PgDown"}));
    QCOMPARE(portable("session.next", Platform::MacOS), (QStringList{"Alt+Down", "Ctrl+Alt+Down"}));
    QCOMPARE(portable("message.push", Platform::Linux), QStringList{"Ctrl+X, Ctrl+S"});
    QCOMPARE(portable("message.push", Platform::MacOS), QStringList{"Meta+X, Meta+S"});
    QCOMPARE(portable("queue.sendNow", Platform::Linux), QStringList{"Ctrl+Shift+Return"});
    QVERIFY(Shortcuts::command("go.anywhere"));
    QVERIFY(!Shortcuts::command("go.nowhere"));
    QVERIFY(Shortcuts::defaults("go.nowhere", Platform::Linux).isEmpty());
}

void TestShortcuts::fixedAndEditingKeysCannotBeAssigned()
{
    const auto allowed = [](const QString &chord, Platform platform = Platform::Linux) {
        return Shortcuts::assignable(QKeySequence::fromString(chord, QKeySequence::PortableText), platform);
    };
    for (const auto &chord : {"Return", "Esc", "Tab", "Shift+A", "Space", "Down", "Ctrl+C", "Ctrl+V", "Ctrl+X",
             "Ctrl+Z", "Ctrl+Shift+Z", "Ctrl+A", "Ctrl+Left", "Alt+Backspace", "Ctrl+Shift+Delete", "Shift+F10", "Menu",
             "Ctrl+Shift+C", "Ctrl+Shift+V", "Ctrl+V, Ctrl+S", "Ctrl+K, Ctrl+X", "Ctrl+J, Ctrl+K, Ctrl+L"})
        QVERIFY2(!allowed(chord), chord);
    QString reason;
    QVERIFY(!Shortcuts::assignable(QKeySequence(), Platform::Linux, &reason));
    QVERIFY(!reason.isEmpty());
    for (const auto &chord : {"F2", "Shift+F6", "Ctrl+K", "Alt+Down", "Ctrl+X, Ctrl+S", "Ctrl+C, Ctrl+J", "Ctrl+Shift+K"})
        QVERIFY2(allowed(chord), chord);
    // macOS keeps Command-Shift-C free; Terminal copies with Command-C.
    QVERIFY(allowed("Ctrl+Shift+C", Platform::MacOS));
    QVERIFY(!allowed("Ctrl+C", Platform::MacOS));
}

void TestShortcuts::terminalKeepsAgentKeys()
{
    const auto safe = [](const QString &chord, Platform platform = Platform::Linux) {
        return Shortcuts::terminalSafe(QKeySequence::fromString(chord, QKeySequence::PortableText)[0], platform);
    };
    for (const auto &chord : {"Ctrl+1", "Ctrl+0", "Ctrl+Tab", "Ctrl+Shift+Tab", "Ctrl+PgUp", "Ctrl+PgDown", "Ctrl+`",
             "Ctrl+,", "Ctrl+Shift+Return"})
        QVERIFY2(safe(chord), chord);
    for (const auto &chord : {"Ctrl+C", "Ctrl+R", "Ctrl+K", "Ctrl+X", "Ctrl+S", "Ctrl+Shift+K", "Alt+Up", "Alt+1",
             "Esc", "Return", "Ctrl+Return", "F1", "F6", "Ctrl+/", "Ctrl+-", "Ctrl+Alt+1", "Ctrl+Shift+C"})
        QVERIFY2(!safe(chord), chord);
    QVERIFY(Shortcuts::terminalSafe(QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Backtab), Platform::Linux));
    // macOS: every Command chord but copy and paste; Control only for tabs.
    for (const auto &chord : {"Ctrl+K", "Ctrl+1", "Ctrl+Alt+Down", "Ctrl+Shift+Return", "Meta+Tab", "Meta+Shift+Tab", "Meta+`"})
        QVERIFY2(safe(chord, Platform::MacOS), chord);
    for (const auto &chord : {"Ctrl+C", "Ctrl+V", "Meta+X", "Meta+S", "Meta+R", "Alt+Up", "Esc", "F1"})
        QVERIFY2(!safe(chord, Platform::MacOS), chord);
}

void TestShortcuts::textFieldsKeepEditingKeys()
{
    const auto kept = [](QKeyCombination key) { return Shortcuts::protectedInTextField(key, Platform::Linux); };
    QVERIFY(kept(QKeyCombination(Qt::Key_A)));
    QVERIFY(kept(QKeyCombination(Qt::ShiftModifier, Qt::Key_A)));
    QVERIFY(kept(QKeyCombination(Qt::Key_Return)));
    QVERIFY(kept(QKeyCombination(Qt::ControlModifier, Qt::Key_C)));
    QVERIFY(kept(QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Left)));
    QVERIFY(kept(QKeyCombination(Qt::AltModifier, Qt::Key_Backspace)));
    QVERIFY(!kept(QKeyCombination(Qt::Key_F2)));
    QVERIFY(!kept(QKeyCombination(Qt::ControlModifier, Qt::Key_K)));
    QVERIFY(!kept(QKeyCombination(Qt::AltModifier, Qt::Key_Up)));
    QVERIFY(!kept(QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Return)));
    QVERIFY(!kept(QKeyCombination(Qt::ControlModifier | Qt::KeypadModifier, Qt::Key_1)));
}

void TestShortcuts::overridesPersistAsPortableChords()
{
    ShortcutRegistry registry(Platform::Linux);
    QSignalSpy changed(&registry, &ShortcutRegistry::changed);
    QVERIFY(!registry.customized("go.anywhere"));
    QCOMPARE(registry.hint("go.anywhere"), QKeySequence("Ctrl+K").toString(QKeySequence::NativeText));
    QCOMPARE(registry.tooltip("Go to", "go.anywhere"), QString("Go to (%1)").arg(registry.hint("go.anywhere")));

    registry.setKeys("go.anywhere", {QKeySequence("Ctrl+J"), QKeySequence("Alt+G")});
    QCOMPARE(changed.count(), 1);
    QVERIFY(registry.customized("go.anywhere"));
    QCOMPARE(QSettings().value("shortcuts/go.anywhere").toStringList(), (QStringList{"Ctrl+J", "Alt+G"}));
    QCOMPARE(ShortcutRegistry(Platform::Linux).keys("go.anywhere"), (QList<QKeySequence>{QKeySequence("Ctrl+J"), QKeySequence("Alt+G")}));

    // No chord is stored as an empty list and differs from the defaults.
    registry.setKeys("go.anywhere", {});
    QVERIFY(registry.customized("go.anywhere"));
    QVERIFY(registry.keys("go.anywhere").isEmpty());
    QVERIFY(registry.hint("go.anywhere").isEmpty());
    QCOMPARE(registry.tooltip("Go to", "go.anywhere"), QString("Go to"));

    // Choosing the defaults again stores nothing.
    registry.setKeys("go.anywhere", Shortcuts::defaults("go.anywhere", Platform::Linux));
    QVERIFY(!registry.customized("go.anywhere"));

    registry.setKeys("session.new", {QKeySequence("Ctrl+J")});
    registry.reset("session.new");
    QVERIFY(!registry.customized("session.new"));
    registry.setKeys("session.new", {QKeySequence("Ctrl+J")});
    registry.setKeys("panel.toggle", {});
    registry.resetAll();
    QVERIFY(!registry.customized("session.new"));
    QVERIFY(!registry.customized("panel.toggle"));
    QCOMPARE(changed.count(), 8);
    registry.setKeys("go.nowhere", {QKeySequence("Ctrl+J")});
    QVERIFY(!QSettings().contains("shortcuts/go.nowhere"));
}

void TestShortcuts::conflictsIncludeSequencePrefixes()
{
    ShortcutRegistry registry(Platform::Linux);
    QCOMPARE(registry.conflict("session.new", QKeySequence("Ctrl+K")), QString("go.anywhere"));
    QCOMPARE(registry.conflict("go.anywhere", QKeySequence("Ctrl+K")), QString());
    QCOMPARE(registry.conflict("session.new", QKeySequence("Ctrl+X")), QString("message.push"));
    QCOMPARE(registry.conflict("session.new", QKeySequence("Ctrl+K, Ctrl+J")), QString("go.anywhere"));
    QCOMPARE(registry.conflict("session.new", QKeySequence("Ctrl+J")), QString());
    registry.setKeys("go.anywhere", {});
    QCOMPARE(registry.conflict("session.new", QKeySequence("Ctrl+K")), QString());
}

namespace {
struct Window {
    QWidget window;
    QPlainTextEdit *editor = new QPlainTextEdit;
    QLineEdit *search = new QLineEdit;
    TerminalScreen *terminal = new TerminalScreen;
    QListWidget *list = new QListWidget;
    int tabs = 0, find = 0, previous = 0, rename = 0, word = 0, push = 0;
    ShortcutRouter router{&window, Platform::Linux};
    Window()
    {
        auto *layout = new QVBoxLayout(&window);
        for (QWidget *widget : {static_cast<QWidget *>(editor), static_cast<QWidget *>(search),
                 static_cast<QWidget *>(terminal), static_cast<QWidget *>(list)})
            layout->addWidget(widget);
        list->addItems({"one", "two"});
        const auto add = [this](QWidget *owner, const char *chord, int *count, Qt::ShortcutContext context) {
            auto *action = new QAction(owner);
            action->setShortcut(QKeySequence::fromString(chord, QKeySequence::PortableText));
            action->setShortcutContext(context);
            QObject::connect(action, &QAction::triggered, owner, [count] { ++*count; });
            owner->addAction(action);
        };
        add(&window, "Ctrl+Tab", &tabs, Qt::WindowShortcut);
        add(&window, "Ctrl+K", &find, Qt::WindowShortcut);
        add(&window, "Alt+Up", &previous, Qt::WindowShortcut);
        add(&window, "F2", &rename, Qt::WindowShortcut);
        add(&window, "Ctrl+Left", &word, Qt::WindowShortcut);
        add(editor, "Ctrl+X, Ctrl+S", &push, Qt::WidgetWithChildrenShortcut);
        qApp->installEventFilter(&router);
        window.show();
    }
    ~Window() { qApp->removeEventFilter(&router); }
};
}

void TestShortcuts::routerAppliesTerminalRule()
{
    Window w;
    QVERIFY(QTest::qWaitForWindowActive(&w.window));
    w.terminal->setFocus();
    QTRY_VERIFY(w.terminal->hasFocus());
    QTest::keyClick(w.terminal, Qt::Key_Tab, Qt::ControlModifier);
    QCOMPARE(w.tabs, 1);
    QTest::keyClick(w.terminal, Qt::Key_K, Qt::ControlModifier);
    QTest::keyClick(w.terminal, Qt::Key_Up, Qt::AltModifier);
    QTest::keyClick(w.terminal, Qt::Key_C, Qt::ControlModifier);
    QTest::keyClick(w.terminal, Qt::Key_F2);
    QCOMPARE(w.find, 0);
    QCOMPARE(w.previous, 0);
    QCOMPARE(w.rename, 0);
    QCOMPARE(w.terminal->received, (QList<QKeyCombination>{QKeyCombination(Qt::ControlModifier, Qt::Key_K),
        QKeyCombination(Qt::AltModifier, Qt::Key_Up), QKeyCombination(Qt::ControlModifier, Qt::Key_C), QKeyCombination(Qt::Key_F2)}));
    // Without the router the terminal keeps even Ctrl+Tab.
    qApp->removeEventFilter(&w.router);
    QTest::keyClick(w.terminal, Qt::Key_Tab, Qt::ControlModifier);
    QCOMPARE(w.tabs, 1);
}

void TestShortcuts::routerAppliesTextFieldRule()
{
    Window w;
    QVERIFY(QTest::qWaitForWindowActive(&w.window));
    w.editor->setPlainText("hello world");
    w.editor->setFocus();
    QTRY_VERIFY(w.editor->hasFocus());
    auto cursor = w.editor->textCursor(); cursor.movePosition(QTextCursor::End); w.editor->setTextCursor(cursor);
    QTest::keyClick(w.editor, Qt::Key_K, Qt::ControlModifier);
    QTest::keyClick(w.editor, Qt::Key_Up, Qt::AltModifier);
    QTest::keyClick(w.editor, Qt::Key_F2);
    QCOMPARE(w.find, 1);
    QCOMPARE(w.previous, 1);
    QCOMPARE(w.rename, 1);
    // Word movement stays with the field even though a command uses it.
    QTest::keyClick(w.editor, Qt::Key_Left, Qt::ControlModifier);
    QCOMPARE(w.word, 0);
    QCOMPARE(w.editor->textCursor().position(), 6);
    QCOMPARE(w.editor->toPlainText(), QString("hello world"));
    w.search->setFocus();
    QTRY_VERIFY(w.search->hasFocus());
    QTest::keyClick(w.search, Qt::Key_K, Qt::ControlModifier);
    QCOMPARE(w.find, 2);
    w.list->setFocus();
    QTRY_VERIFY(w.list->hasFocus());
    QTest::keyClick(w.list, Qt::Key_Left, Qt::ControlModifier);
    QCOMPARE(w.word, 1);
}

void TestShortcuts::cutStartsASequenceOnlyWithoutSelection()
{
    Window w;
    QVERIFY(QTest::qWaitForWindowActive(&w.window));
    w.editor->setPlainText("draft");
    w.editor->setFocus();
    QTRY_VERIFY(w.editor->hasFocus());
    auto cursor = w.editor->textCursor(); cursor.movePosition(QTextCursor::End); w.editor->setTextCursor(cursor);
    QTest::keyClick(w.editor, Qt::Key_X, Qt::ControlModifier);
    QTest::keyClick(w.editor, Qt::Key_S, Qt::ControlModifier);
    QCOMPARE(w.push, 1);
    QCOMPARE(w.editor->toPlainText(), QString("draft"));
    // Like every Qt key sequence, a key that breaks it is consumed; the next is typed.
    QTest::keyClick(w.editor, Qt::Key_X, Qt::ControlModifier);
    QTest::keyClick(w.editor, Qt::Key_A);
    QTest::keyClick(w.editor, Qt::Key_B);
    QCOMPARE(w.push, 1);
    QCOMPARE(w.editor->toPlainText(), QString("draftb"));
    w.editor->selectAll();
    QTest::keyClick(w.editor, Qt::Key_X, Qt::ControlModifier);
    QCOMPARE(w.editor->toPlainText(), QString());
    QTest::keyClick(w.editor, Qt::Key_S, Qt::ControlModifier);
    QCOMPARE(w.push, 1);
}

void TestShortcuts::routerIgnoresOtherWindows()
{
    Window w;
    QWidget other;
    auto *field = new QLineEdit(&other);
    field->setText("keep");
    other.show();
    other.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&other));
    field->setFocus();
    QTRY_VERIFY(field->hasFocus());
    QTest::keyClick(field, Qt::Key_Up, Qt::AltModifier);
    QTest::keyClick(field, Qt::Key_K, Qt::ControlModifier);
    QCOMPARE(w.previous, 0);
    QCOMPARE(w.find, 0);
}

QTEST_MAIN(TestShortcuts)
#include "test_shortcuts.moc"
