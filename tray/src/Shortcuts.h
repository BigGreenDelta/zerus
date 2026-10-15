#pragma once
#include <QKeyCombination>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QKeyEvent;
class QWidget;

// Keyboard commands of the main window: stable ids, default chords for each
// platform, and the rules that decide when a chord may leave Terminal or a text
// field. See docs/superpowers/specs/2026-10-15-keyboard-shortcuts-design.md.
namespace Shortcuts {
enum class Platform { Linux, Windows, MacOS };
// Window: anywhere in the main window. Composer: only in a message field.
enum class Scope { Window, Composer };
struct Command { const char *id; const char *group; const char *title; Scope scope; };

Platform currentPlatform();
const QList<Command> &commands();
const Command *command(const QString &id);
QString title(const Command &command);
QString group(const Command &command);
QList<QKeySequence> defaults(const QString &id, Platform platform = currentPlatform());

// The key of an event without the keypad flag; Backtab becomes Shift+Tab.
QKeyCombination combination(const QKeyEvent *event);
QKeyCombination normalized(QKeyCombination key);
// Fixed and editing keys cannot be assigned; *reason says why.
bool assignable(const QKeySequence &sequence, Platform platform, QString *reason = nullptr);
// While Terminal has focus, only these may reach a window command.
bool terminalSafe(QKeyCombination key, Platform platform);
// Editing keys a text field keeps even when a command uses them.
bool protectedInTextField(QKeyCombination key, Platform platform);
// Cut and Copy do nothing without a selection, so they may start a sequence.
bool selectionKey(QKeyCombination key);
QString text(const QKeySequence &sequence);
// The Terminal rule in one sentence for Settings and the cheat sheet.
QString terminalRule(Platform platform);
}

// Current chords: defaults overridden by QSettings group "shortcuts" (one list
// of portable chord strings per command id; an empty list means no chord).
class ShortcutRegistry : public QObject {
    Q_OBJECT
public:
    explicit ShortcutRegistry(Shortcuts::Platform platform = Shortcuts::currentPlatform(), QObject *parent = nullptr);
    static ShortcutRegistry &instance();
    Shortcuts::Platform platform() const { return m_platform; }
    QList<QKeySequence> keys(const QString &id) const;
    bool customized(const QString &id) const;
    void setKeys(const QString &id, const QList<QKeySequence> &keys);
    void reset(const QString &id);
    void resetAll();
    // The first chord in native text, or empty without one.
    QString hint(const QString &id) const;
    // A tooltip with the first chord appended, as "Caption (Ctrl+K)".
    QString tooltip(const QString &caption, const QString &id) const;
    // The command (other than id) already using the sequence, or one of them
    // that would make it ambiguous as a prefix. Empty when free.
    QString conflict(const QString &id, const QKeySequence &sequence) const;
    // Every such command, in command order.
    QStringList conflicts(const QString &id, const QKeySequence &sequence) const;
signals:
    void changed();
private:
    Shortcuts::Platform m_platform;
};

// Lets bound commands win over Terminal and text fields of one window, under
// the Terminal and text-field rules. A focused combo box keeps Alt+Up, Alt+Down
// and F4, which open its list. Install on qApp.
class ShortcutRouter : public QObject {
public:
    explicit ShortcutRouter(QWidget *window, Shortcuts::Platform platform = Shortcuts::currentPlatform(), QObject *parent = nullptr);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    QPointer<QWidget> m_window;
    Shortcuts::Platform m_platform;
    const QEvent *m_yielded = nullptr;
};
