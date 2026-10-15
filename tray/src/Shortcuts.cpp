#include "Shortcuts.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTextEdit>
#include <QWidget>

namespace Shortcuts {
namespace {
constexpr Scope Window = Scope::Window, Composer = Scope::Composer;
const QList<Command> table{
    {"go.overview", QT_TRANSLATE_NOOP("Shortcuts", "Go to"), QT_TRANSLATE_NOOP("Shortcuts", "Go to Overview"), Window},
    {"go.sessions", QT_TRANSLATE_NOOP("Shortcuts", "Go to"), QT_TRANSLATE_NOOP("Shortcuts", "Go to Sessions"), Window},
    {"go.projects", QT_TRANSLATE_NOOP("Shortcuts", "Go to"), QT_TRANSLATE_NOOP("Shortcuts", "Go to Projects"), Window},
    {"go.machines", QT_TRANSLATE_NOOP("Shortcuts", "Go to"), QT_TRANSLATE_NOOP("Shortcuts", "Go to Machines"), Window},
    {"go.accounts", QT_TRANSLATE_NOOP("Shortcuts", "Go to"), QT_TRANSLATE_NOOP("Shortcuts", "Go to Accounts"), Window},
    {"go.settings", QT_TRANSLATE_NOOP("Shortcuts", "Go to"), QT_TRANSLATE_NOOP("Shortcuts", "Open Settings"), Window},
    {"go.anywhere", QT_TRANSLATE_NOOP("Shortcuts", "Go to"), QT_TRANSLATE_NOOP("Shortcuts", "Go to…"), Window},
    {"help.shortcuts", QT_TRANSLATE_NOOP("Shortcuts", "Go to"), QT_TRANSLATE_NOOP("Shortcuts", "Keyboard shortcuts"), Window},
    {"session.next", QT_TRANSLATE_NOOP("Shortcuts", "Sessions"), QT_TRANSLATE_NOOP("Shortcuts", "Next session"), Window},
    {"session.previous", QT_TRANSLATE_NOOP("Shortcuts", "Sessions"), QT_TRANSLATE_NOOP("Shortcuts", "Previous session"), Window},
    {"session.nextAttention", QT_TRANSLATE_NOOP("Shortcuts", "Sessions"), QT_TRANSLATE_NOOP("Shortcuts", "Next session that needs attention"), Window},
    {"session.previousAttention", QT_TRANSLATE_NOOP("Shortcuts", "Sessions"), QT_TRANSLATE_NOOP("Shortcuts", "Previous session that needs attention"), Window},
    {"session.new", QT_TRANSLATE_NOOP("Shortcuts", "Sessions"), QT_TRANSLATE_NOOP("Shortcuts", "New session"), Window},
    {"session.rename", QT_TRANSLATE_NOOP("Shortcuts", "Sessions"), QT_TRANSLATE_NOOP("Shortcuts", "Rename session"), Window},
    {"session.search", QT_TRANSLATE_NOOP("Shortcuts", "Sessions"), QT_TRANSLATE_NOOP("Shortcuts", "Search sessions"), Window},
    {"view.refresh", QT_TRANSLATE_NOOP("Shortcuts", "Sessions"), QT_TRANSLATE_NOOP("Shortcuts", "Refresh"), Window},
    {"tab.next", QT_TRANSLATE_NOOP("Shortcuts", "Session view"), QT_TRANSLATE_NOOP("Shortcuts", "Next tab"), Window},
    {"tab.previous", QT_TRANSLATE_NOOP("Shortcuts", "Session view"), QT_TRANSLATE_NOOP("Shortcuts", "Previous tab"), Window},
    {"tab.terminal", QT_TRANSLATE_NOOP("Shortcuts", "Session view"), QT_TRANSLATE_NOOP("Shortcuts", "Switch between Activity and Terminal"), Window},
    {"panel.toggle", QT_TRANSLATE_NOOP("Shortcuts", "Session view"), QT_TRANSLATE_NOOP("Shortcuts", "Show or hide the side panel"), Window},
    {"focus.next", QT_TRANSLATE_NOOP("Shortcuts", "Session view"), QT_TRANSLATE_NOOP("Shortcuts", "Next area"), Window},
    {"focus.previous", QT_TRANSLATE_NOOP("Shortcuts", "Session view"), QT_TRANSLATE_NOOP("Shortcuts", "Previous area"), Window},
    {"message.send", QT_TRANSLATE_NOOP("Shortcuts", "Messages"), QT_TRANSLATE_NOOP("Shortcuts", "Send message"), Composer},
    {"queue.sendNow", QT_TRANSLATE_NOOP("Shortcuts", "Messages"), QT_TRANSLATE_NOOP("Shortcuts", "Send queued messages now"), Window},
    {"message.push", QT_TRANSLATE_NOOP("Shortcuts", "Messages"), QT_TRANSLATE_NOOP("Shortcuts", "Send and push now"), Composer},
    {"zoom.in", QT_TRANSLATE_NOOP("Shortcuts", "Content scale"), QT_TRANSLATE_NOOP("Shortcuts", "Increase content scale"), Window},
    {"zoom.out", QT_TRANSLATE_NOOP("Shortcuts", "Content scale"), QT_TRANSLATE_NOOP("Shortcuts", "Decrease content scale"), Window},
    {"zoom.reset", QT_TRANSLATE_NOOP("Shortcuts", "Content scale"), QT_TRANSLATE_NOOP("Shortcuts", "Reset content scale"), Window},
};

// Portable notation: "Ctrl" is Command and "Meta" the Control key on macOS.
// macOS keeps physical Control for tabs: Command-Tab and Command-` belong to
// the system application and window switchers.
QStringList portable(const QString &id, Platform platform)
{
    const bool mac = platform == Platform::MacOS;
    static const QHash<QString, QStringList> common{
        {"go.overview", {"Ctrl+1"}}, {"go.sessions", {"Ctrl+2"}}, {"go.projects", {"Ctrl+3"}},
        {"go.machines", {"Ctrl+4"}}, {"go.accounts", {"Ctrl+5"}}, {"go.settings", {"Ctrl+,"}},
        {"go.anywhere", {"Ctrl+K"}}, {"help.shortcuts", {"F1", "Ctrl+/"}},
        {"session.nextAttention", {"Alt+Shift+Down"}}, {"session.previousAttention", {"Alt+Shift+Up"}},
        {"session.new", {"Ctrl+N"}}, {"session.rename", {"F2"}}, {"session.search", {"Ctrl+F"}},
        {"view.refresh", {"Ctrl+R", "F5"}}, {"panel.toggle", {"Ctrl+I"}},
        {"focus.next", {"F6"}}, {"focus.previous", {"Shift+F6"}},
        {"message.send", {"Ctrl+Return"}}, {"queue.sendNow", {"Ctrl+Shift+Return"}},
        {"zoom.in", {"Ctrl+=", "Ctrl++"}}, {"zoom.out", {"Ctrl+-"}}, {"zoom.reset", {"Ctrl+0"}},
    };
    if (common.contains(id)) return common.value(id);
    if (id == "session.next") return {"Alt+Down", mac ? "Ctrl+Alt+Down" : "Ctrl+PgDown"};
    if (id == "session.previous") return {"Alt+Up", mac ? "Ctrl+Alt+Up" : "Ctrl+PgUp"};
    if (id == "tab.next") return {mac ? "Meta+Tab" : "Ctrl+Tab"};
    if (id == "tab.previous") return {mac ? "Meta+Shift+Tab" : "Ctrl+Shift+Tab"};
    if (id == "tab.terminal") return {mac ? "Meta+`" : "Ctrl+`"};
    if (id == "message.push") return {mac ? "Meta+X, Meta+S" : "Ctrl+X, Ctrl+S"};
    return {};
}

constexpr Qt::KeyboardModifiers Chord = Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;

bool functionKey(int key) { return key >= Qt::Key_F1 && key <= Qt::Key_F35; }
bool modifierKey(int key)
{
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta
        || key == Qt::Key_AltGr || key == Qt::Key_unknown || key == 0;
}
// Ctrl is Command on macOS, so the same combinations serve both families.
bool editingKey(QKeyCombination key)
{
    const auto mods = key.keyboardModifiers();
    const int k = key.key();
    const auto ctrl = Qt::ControlModifier, shift = Qt::ShiftModifier;
    if (mods == ctrl && (k == Qt::Key_C || k == Qt::Key_X || k == Qt::Key_V || k == Qt::Key_Z
            || k == Qt::Key_Y || k == Qt::Key_A || k == Qt::Key_Insert)) return true;
    if (mods == (ctrl | shift) && k == Qt::Key_Z) return true;
    // Word and line movement and deletion.
    const bool movement = k == Qt::Key_Left || k == Qt::Key_Right || k == Qt::Key_Home
        || k == Qt::Key_End || k == Qt::Key_Backspace || k == Qt::Key_Delete;
    return movement && (mods & Chord);
}
}

Platform currentPlatform()
{
#if defined(Q_OS_MACOS)
    return Platform::MacOS;
#elif defined(Q_OS_WIN)
    return Platform::Windows;
#else
    return Platform::Linux;
#endif
}

const QList<Command> &commands() { return table; }

const Command *command(const QString &id)
{
    for (const auto &command : table)
        if (id == QLatin1String(command.id)) return &command;
    return nullptr;
}

QString title(const Command &command) { return QCoreApplication::translate("Shortcuts", command.title); }
QString group(const Command &command) { return QCoreApplication::translate("Shortcuts", command.group); }

QList<QKeySequence> defaults(const QString &id, Platform platform)
{
    QList<QKeySequence> result;
    for (const auto &chord : portable(id, platform))
        result.append(QKeySequence::fromString(chord, QKeySequence::PortableText));
    return result;
}

QKeyCombination normalized(QKeyCombination key)
{
    auto mods = key.keyboardModifiers() & ~Qt::KeypadModifier;
    int k = key.key();
    if (k == Qt::Key_Backtab) { k = Qt::Key_Tab; mods |= Qt::ShiftModifier; }
    return QKeyCombination(mods, Qt::Key(k));
}

QKeyCombination combination(const QKeyEvent *event)
{
    return normalized(QKeyCombination(event->modifiers(), Qt::Key(event->key())));
}

bool selectionKey(QKeyCombination key)
{
    key = normalized(key);
    return key.keyboardModifiers() == Qt::ControlModifier && (key.key() == Qt::Key_X || key.key() == Qt::Key_C);
}

bool assignable(const QKeySequence &sequence, Platform platform, QString *reason)
{
    const auto fail = [reason](const QString &why) { if (reason) *reason = why; return false; };
    if (sequence.isEmpty()) return fail(QCoreApplication::translate("Shortcuts", "Press a key combination."));
    if (sequence.count() > 2) return fail(QCoreApplication::translate("Shortcuts", "Use one key combination or a sequence of two."));
    const bool mac = platform == Platform::MacOS;
    for (int i = 0; i < sequence.count(); ++i) {
        const auto key = normalized(sequence[i]);
        const auto mods = key.keyboardModifiers();
        const int k = key.key();
        if (modifierKey(k)) return fail(QCoreApplication::translate("Shortcuts", "Press a key together with the modifiers."));
        if ((k == Qt::Key_F10 && mods == Qt::ShiftModifier) || k == Qt::Key_Menu)
            return fail(QCoreApplication::translate("Shortcuts", "This key opens context menus."));
        if (!(mods & Chord) && !functionKey(k))
            return fail(mac ? QCoreApplication::translate("Shortcuts", "Keys without ⌘, ⌃ or ⌥ are used for typing and navigation.")
                            : QCoreApplication::translate("Shortcuts", "Keys without Ctrl or Alt are used for typing and navigation."));
        if (!mac && mods == (Qt::ControlModifier | Qt::ShiftModifier) && (k == Qt::Key_C || k == Qt::Key_V))
            return fail(QCoreApplication::translate("Shortcuts", "Terminal uses this key to copy and paste."));
        // Cut or Copy may start a sequence: without a selection they do nothing.
        if (editingKey(key) && !(i == 0 && sequence.count() > 1 && selectionKey(key)))
            return fail(QCoreApplication::translate("Shortcuts", "Text fields use this key for editing."));
    }
    return true;
}

bool terminalSafe(QKeyCombination key, Platform platform)
{
    key = normalized(key);
    const auto mods = key.keyboardModifiers();
    const int k = key.key();
    const auto ctrl = Qt::ControlModifier, shift = Qt::ShiftModifier;
    if (platform == Platform::MacOS) {
        if (mods & ctrl) return !(mods == ctrl && (k == Qt::Key_C || k == Qt::Key_V));
        const auto control = Qt::MetaModifier;
        return (mods == control || mods == (control | shift)) && (k == Qt::Key_Tab || k == Qt::Key_QuoteLeft);
    }
    if (mods != ctrl && mods != (ctrl | shift)) return false;
    if (k >= Qt::Key_0 && k <= Qt::Key_9) return true;
    if (k == Qt::Key_Tab || k == Qt::Key_PageUp || k == Qt::Key_PageDown || k == Qt::Key_QuoteLeft || k == Qt::Key_Comma)
        return true;
    return mods == (ctrl | shift) && (k == Qt::Key_Return || k == Qt::Key_Enter);
}

bool protectedInTextField(QKeyCombination key, Platform)
{
    key = normalized(key);
    if (!(key.keyboardModifiers() & Chord)) return !functionKey(key.key());
    return editingKey(key);
}

QString text(const QKeySequence &sequence) { return sequence.toString(QKeySequence::NativeText); }

// As terminalSafe() decides, in the platform's own notation.
QString terminalRule(Platform platform)
{
    if (platform == Platform::MacOS)
        return QCoreApplication::translate("Shortcuts", "In Terminal, a command's chord reaches Zerus only when it uses ⌘ (except ⌘C and ⌘V) or is ⌃Tab, ⌃⇧Tab, ⌃` or ⌃⇧`. Every other ⌃ chord and every ⌥ chord goes to the agent, bound or not.");
    return QCoreApplication::translate("Shortcuts", "In Terminal, a command's chord reaches Zerus only when it is Ctrl or Ctrl+Shift with a digit, Tab, Page Up, Page Down, backtick or comma, or Ctrl+Shift+Enter. Everything else goes to the agent, bound or not.");
}
}

ShortcutRegistry::ShortcutRegistry(Shortcuts::Platform platform, QObject *parent) : QObject(parent), m_platform(platform) {}

ShortcutRegistry &ShortcutRegistry::instance()
{
    static ShortcutRegistry registry;
    return registry;
}

static QString settingsKey(const QString &id) { return QStringLiteral("shortcuts/") + id; }

QList<QKeySequence> ShortcutRegistry::keys(const QString &id) const
{
    if (!Shortcuts::command(id)) return {};
    QSettings settings;
    if (!settings.contains(settingsKey(id))) return Shortcuts::defaults(id, m_platform);
    QList<QKeySequence> result;
    for (const auto &chord : settings.value(settingsKey(id)).toStringList()) {
        const auto sequence = QKeySequence::fromString(chord, QKeySequence::PortableText);
        if (!sequence.isEmpty()) result.append(sequence);
    }
    return result;
}

bool ShortcutRegistry::customized(const QString &id) const { return QSettings().contains(settingsKey(id)); }

void ShortcutRegistry::setKeys(const QString &id, const QList<QKeySequence> &keys)
{
    if (!Shortcuts::command(id)) return;
    QStringList chords;
    for (const auto &key : keys)
        if (!key.isEmpty()) chords.append(key.toString(QKeySequence::PortableText));
    QSettings settings;
    QStringList defaults;
    for (const auto &key : Shortcuts::defaults(id, m_platform)) defaults.append(key.toString(QKeySequence::PortableText));
    // Defaults are stored as an absence, so they follow future default changes.
    if (chords == defaults) settings.remove(settingsKey(id));
    else settings.setValue(settingsKey(id), chords);
    emit changed();
}

void ShortcutRegistry::reset(const QString &id)
{
    QSettings().remove(settingsKey(id));
    emit changed();
}

void ShortcutRegistry::resetAll()
{
    QSettings().remove(QStringLiteral("shortcuts"));
    emit changed();
}

QString ShortcutRegistry::hint(const QString &id) const
{
    const auto current = keys(id);
    return current.isEmpty() ? QString() : Shortcuts::text(current.first());
}

QString ShortcutRegistry::tooltip(const QString &caption, const QString &id) const
{
    const QString chord = hint(id);
    return chord.isEmpty() ? caption : QStringLiteral("%1 (%2)").arg(caption, chord);
}

QString ShortcutRegistry::conflict(const QString &id, const QKeySequence &sequence) const
{
    return conflicts(id, sequence).value(0);
}

QStringList ShortcutRegistry::conflicts(const QString &id, const QKeySequence &sequence) const
{
    QStringList result;
    if (sequence.isEmpty()) return result;
    for (const auto &command : Shortcuts::commands()) {
        const QString other = QLatin1String(command.id);
        if (other == id) continue;
        for (const auto &key : keys(other))
            // Equal, or one is a prefix of the other, which would make it ambiguous.
            if (key.matches(sequence) != QKeySequence::NoMatch || sequence.matches(key) != QKeySequence::NoMatch) {
                result.append(other); break;
            }
    }
    return result;
}

ShortcutRouter::ShortcutRouter(QWidget *window, Shortcuts::Platform platform, QObject *parent)
    : QObject(parent), m_window(window), m_platform(platform) {}

static bool terminalWidget(const QWidget *widget)
{
    for (; widget; widget = widget->parentWidget()) {
        if (widget->inherits("TerminalScreen")) return true;
        if (widget->isWindow()) break;
    }
    return false;
}

static bool hasSelection(const QWidget *widget)
{
    if (const auto *line = qobject_cast<const QLineEdit *>(widget)) return line->hasSelectedText();
    if (const auto *plain = qobject_cast<const QPlainTextEdit *>(widget)) return plain->textCursor().hasSelection();
    if (const auto *rich = qobject_cast<const QTextEdit *>(widget)) return rich->textCursor().hasSelection();
    return false;
}

static bool textField(const QWidget *widget)
{
    return qobject_cast<const QLineEdit *>(widget) || qobject_cast<const QPlainTextEdit *>(widget)
        || qobject_cast<const QTextEdit *>(widget);
}

// A combo box opens its list with Alt+Up, Alt+Down and F4, also where a command
// uses them. An editable combo box keeps the focus itself (its line edit's focus
// proxy) and forwards keys to the line edit.
static bool comboPopupKey(const QWidget *focus, QKeyCombination key)
{
    if (!qobject_cast<const QComboBox *>(focus)) return false;
    const auto mods = key.keyboardModifiers();
    return (mods == Qt::AltModifier && (key.key() == Qt::Key_Up || key.key() == Qt::Key_Down))
        || (mods == Qt::NoModifier && key.key() == Qt::Key_F4);
}

bool ShortcutRouter::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::ShortcutOverride) return false;
    // Platform input delivers an override to the window's QWindow first, then to
    // the focus widget and its ancestors: each override starts at a non-widget.
    // Overrides can reuse one address, so a remembered one must not outlive this.
    if (!watched->isWidgetType()) { m_yielded = nullptr; return false; }
    QWidget *focus = QApplication::focusWidget();
    if (!m_window || !focus || focus->window() != m_window) return false;
    // A yielded override propagates to the focus widget's ancestors; none may claim it.
    // Propagation ends at the window, so an override sent straight to an
    // ancestor later cannot match a stale address.
    if (watched != focus) {
        auto *widget = static_cast<QWidget *>(watched);
        const bool yielded = event == m_yielded && widget->isAncestorOf(focus);
        if (widget->isWindow()) m_yielded = nullptr;
        return yielded;
    }
    m_yielded = nullptr;
    const auto key = Shortcuts::combination(static_cast<QKeyEvent *>(event));
    if (comboPopupKey(focus, key)) { event->accept(); return true; }
    // The action Qt's shortcut map would consider: the focus chain's own actions,
    // then the window's.
    QKeySequence bound;
    for (QWidget *widget = focus; widget && bound.isEmpty(); widget = widget->parentWidget()) {
        for (QAction *action : widget->actions()) {
            const auto context = action->shortcutContext();
            if (!action->isEnabled() || (context == Qt::WidgetShortcut && widget != focus)) continue;
            for (const auto &sequence : action->shortcuts())
                if (!sequence.isEmpty() && Shortcuts::normalized(sequence[0]) == key) { bound = sequence; break; }
            if (!bound.isEmpty()) break;
        }
        if (widget->isWindow()) break;
    }
    if (bound.isEmpty()) return false;
    if (terminalWidget(focus)) {
        if (!Shortcuts::terminalSafe(key, m_platform)) return false;
    } else if (textField(focus) && Shortcuts::protectedInTextField(key, m_platform)) {
        if (!(bound.count() > 1 && Shortcuts::selectionKey(key) && !hasSelection(focus))) return false;
    }
    event->ignore();
    // A focused window has no ancestors to propagate to.
    m_yielded = focus->isWindow() ? nullptr : event;
    return true;
}
