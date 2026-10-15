#include "ShortcutsDialog.h"
#include <QHash>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
QString native(const QKeySequence &sequence) { return sequence.toString(QKeySequence::NativeText); }

// Keys that keep their behavior and cannot be rebound. Spec: "Fixed keys".
QList<QPair<QString, QString>> fixedKeys()
{
    QString ctrl = native(QKeySequence(Qt::CTRL));
    if (ctrl.endsWith(QLatin1Char('+'))) ctrl.chop(1);
    if (ctrl.isEmpty()) ctrl = QStringLiteral("Ctrl");
    const QString comma = QStringLiteral(", ");
    const QString space = QStringLiteral(" ");
    return {
        {native(QKeySequence(Qt::Key_Return)), QObject::tr("Send the message")},
        {native(QKeySequence(Qt::SHIFT | Qt::Key_Return)), QObject::tr("New line in the message field")},
        {native(QKeySequence(Qt::Key_Escape)), QObject::tr("Clear the search, then stop the selected turn; close dialogs")},
        {native(QKeySequence(Qt::Key_Tab)) + comma + native(QKeySequence(Qt::SHIFT | Qt::Key_Tab)), QObject::tr("Move focus")},
        {native(QKeySequence(Qt::Key_Tab)), QObject::tr("Take the suggestion in an empty message field")},
        {native(QKeySequence(Qt::Key_Up)) + space + native(QKeySequence(Qt::Key_Down)) + space + native(QKeySequence(Qt::Key_Home)) + space
             + native(QKeySequence(Qt::Key_End)) + space + native(QKeySequence(Qt::Key_Return)),
         QObject::tr("Move and open in lists")},
        {native(QKeySequence(Qt::Key_Menu)) + comma + native(QKeySequence(Qt::SHIFT | Qt::Key_F10)), QObject::tr("Context menu")},
        {ctrl + QStringLiteral("-click"), QObject::tr("Select several items")},
        {native(QKeySequence(Qt::Key_Space)) + comma + native(QKeySequence(Qt::Key_Escape)), QObject::tr("Stop voice capture")},
        {native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C)) + comma + native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V)),
         QObject::tr("Copy and paste in Terminal")},
        {native(QKeySequence(Qt::SHIFT | Qt::Key_PageUp)) + comma + native(QKeySequence(Qt::SHIFT | Qt::Key_PageDown)),
         QObject::tr("Scroll Terminal")},
        {native(QKeySequence(Qt::ALT | Qt::Key_Up)) + comma + native(QKeySequence(Qt::ALT | Qt::Key_Down)),
         QObject::tr("Reorder projects in the Projects dialog")},
    };
}
}

ShortcutsDialog::ShortcutsDialog(ShortcutRegistry &registry, QWidget *parent) : QDialog(parent), m_registry(registry)
{
    setObjectName("shortcutsDialog");
    setWindowTitle(tr("Keyboard shortcuts"));
    setModal(false);
    resize(560, 640);
    auto *layout = new QVBoxLayout(this);
    m_filter = new QLineEdit;
    m_filter->setObjectName("shortcutsFilter");
    m_filter->setPlaceholderText(tr("Filter shortcuts"));
    m_filter->setClearButtonEnabled(true);
    layout->addWidget(m_filter);
    m_list = new QTreeWidget;
    m_list->setObjectName("shortcutsList");
    m_list->setColumnCount(2);
    m_list->setHeaderLabels({tr("Command"), tr("Shortcut")});
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_list, 1);
    auto *note = new QLabel(tr("In Terminal, only chords starting with Ctrl and a digit, Tab, Page Up, Page Down, backtick or comma reach the window; everything else goes to the agent."));
    note->setObjectName("shortcutsTerminalNote");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *customize = new QPushButton(tr("Customize…"));
    customize->setObjectName("shortcutsCustomize");
    customize->setAutoDefault(false);
    layout->addWidget(customize, 0, Qt::AlignRight);
    connect(customize, &QPushButton::clicked, this, &ShortcutsDialog::customizeRequested);
    connect(m_filter, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(&m_registry, &ShortcutRegistry::changed, this, &ShortcutsDialog::rebuild);
    rebuild();
}

void ShortcutsDialog::rebuild()
{
    const QString needle = m_filter->text().trimmed();
    auto matches = [&](const QString &a, const QString &b) {
        return needle.isEmpty() || a.contains(needle, Qt::CaseInsensitive) || b.contains(needle, Qt::CaseInsensitive);
    };
    m_list->clear();
    QHash<QString, QTreeWidgetItem *> groups;
    auto groupItem = [&](const QString &name) {
        auto *item = groups.value(name);
        if (!item) {
            item = new QTreeWidgetItem(m_list, {name});
            QFont font = item->font(0);
            font.setBold(true);
            item->setFont(0, font);
            groups.insert(name, item);
        }
        return item;
    };
    for (const auto &command : Shortcuts::commands()) {
        const QString title = Shortcuts::title(command);
        QStringList chords;
        for (const auto &key : m_registry.keys(QLatin1String(command.id))) chords.append(Shortcuts::text(key));
        const QString chord = chords.join(QStringLiteral(", "));
        if (matches(title, chord)) new QTreeWidgetItem(groupItem(Shortcuts::group(command)), {title, chord});
    }
    for (const auto &fixed : fixedKeys())
        if (matches(fixed.second, fixed.first)) new QTreeWidgetItem(groupItem(tr("Fixed keys")), {fixed.second, fixed.first});
    m_list->expandAll();
    m_list->resizeColumnToContents(1);
}
