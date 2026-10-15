#include "KeyboardSettings.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int IdRole = Qt::UserRole;

bool overlaps(const QKeySequence &a, const QKeySequence &b)
{
    return a.matches(b) != QKeySequence::NoMatch || b.matches(a) != QKeySequence::NoMatch;
}
}

KeyboardSettings::KeyboardSettings(ShortcutRegistry &registry, QWidget *parent) : QWidget(parent), m_registry(registry)
{
    setObjectName("keyboardSettings");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto *heading = new QLabel(tr("Keyboard"));
    heading->setObjectName("heading");
    layout->addWidget(heading);
    m_tree = new QTreeWidget;
    m_tree->setObjectName("keyboardCommands");
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Command"), tr("Shortcut"), tr("Alternate")});
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setMinimumHeight(320);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_tree, 1);
    auto *row = new QHBoxLayout;
    m_reset = new QPushButton(tr("Reset"));
    m_reset->setObjectName("keyboardResetCommand");
    m_reset->setToolTip(tr("Restore the default chords of the selected command"));
    m_reset->setAutoDefault(false);
    m_resetAll = new QPushButton(tr("Reset all"));
    m_resetAll->setObjectName("keyboardResetAll");
    m_resetAll->setAutoDefault(false);
    row->addWidget(m_reset);
    row->addWidget(m_resetAll);
    row->addStretch();
    layout->addLayout(row);
    auto *note = new QLabel(Shortcuts::terminalRule(m_registry.platform()) + QLatin1Char(' ') + tr("Chords marked ✓ work in Terminal."));
    note->setObjectName("keyboardTerminalNote");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *hint = new QLabel(tr("Double-click a shortcut, or press Enter on it, to change it."));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    // activated covers double-click and Enter; connecting doubleClicked too would open the editor twice.
    connect(m_tree, &QTreeWidget::itemActivated, this, &KeyboardSettings::editItem);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] { updateButtons(); });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        if (const auto *item = m_tree->currentItem()) {
            const QString id = item->data(0, IdRole).toString();
            if (!id.isEmpty()) m_registry.reset(id);
        }
    });
    connect(m_resetAll, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Reset all shortcuts"), tr("Restore the default chords of every command?")) == QMessageBox::Yes)
            m_registry.resetAll();
    });
    connect(&m_registry, &ShortcutRegistry::changed, this, &KeyboardSettings::rebuild);
    rebuild();
}

void KeyboardSettings::updateButtons()
{
    const auto *item = m_tree->currentItem();
    m_reset->setEnabled(item && !item->data(0, IdRole).toString().isEmpty());
}

void KeyboardSettings::rebuild()
{
    const QString selected = m_tree->currentItem() ? m_tree->currentItem()->data(0, IdRole).toString() : QString();
    const QSignalBlocker block(m_tree);
    m_tree->clear();
    QHash<QString, QTreeWidgetItem *> groups;
    QTreeWidgetItem *toSelect = nullptr;
    for (const auto &command : Shortcuts::commands()) {
        const QString group = Shortcuts::group(command);
        auto *parent = groups.value(group);
        if (!parent) {
            parent = new QTreeWidgetItem(m_tree, {group});
            QFont font = parent->font(0);
            font.setBold(true);
            parent->setFont(0, font);
            groups.insert(group, parent);
        }
        const QString id = QLatin1String(command.id);
        auto *item = new QTreeWidgetItem(parent, {Shortcuts::title(command)});
        item->setData(0, IdRole, id);
        const auto keys = m_registry.keys(id);
        for (int slot = 0; slot < 2 && slot < keys.size(); ++slot) {
            QString chord = Shortcuts::text(keys.at(slot));
            if (command.scope == Shortcuts::Scope::Window && keys.at(slot).count() > 0
                && Shortcuts::terminalSafe(keys.at(slot)[0], m_registry.platform())) {
                chord += QStringLiteral(" ✓");
                item->setToolTip(slot + 1, tr("Works in Terminal"));
            }
            item->setText(slot + 1, chord);
        }
        if (m_registry.customized(id)) {
            QFont font = item->font(0);
            font.setItalic(true);
            item->setFont(0, font);
        }
        if (id == selected) toSelect = item;
    }
    m_tree->expandAll();
    m_tree->resizeColumnToContents(1);
    m_tree->resizeColumnToContents(2);
    if (toSelect) m_tree->setCurrentItem(toSelect);
    updateButtons();
}

void KeyboardSettings::editItem(QTreeWidgetItem *item, int column)
{
    if (!item) return;
    const QString id = item->data(0, IdRole).toString();
    if (id.isEmpty()) return;
    // Enter activates the row without a column: edit the first chord.
    editChord(id, column == 2 ? 1 : 0);
}

void KeyboardSettings::editChord(const QString &id, int slot)
{
    const auto *command = Shortcuts::command(id);
    if (!command || slot < 0 || slot > 1) return;
    auto keys = m_registry.keys(id);
    QDialog dialog(this);
    dialog.setObjectName("keyboardEditDialog");
    dialog.setWindowTitle(tr("Change shortcut"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Press the new shortcut for %1.").arg(Shortcuts::title(*command))));
    auto *row = new QHBoxLayout;
    auto *edit = new QKeySequenceEdit(slot < keys.size() ? keys.at(slot) : QKeySequence());
    edit->setObjectName("keyboardEdit");
    edit->setMaximumSequenceLength(2);
    auto *clear = new QPushButton(tr("Clear"));
    clear->setObjectName("keyboardEditClear");
    clear->setAutoDefault(false);
    row->addWidget(edit, 1);
    row->addWidget(clear);
    layout->addLayout(row);
    auto *reason = new QLabel;
    reason->setObjectName("keyboardEditReason");
    reason->setWordWrap(true);
    reason->setVisible(false);
    layout->addWidget(reason);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(clear, &QPushButton::clicked, edit, &QKeySequenceEdit::clear);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const QKeySequence sequence = edit->keySequence();
        QString why;
        if (!sequence.isEmpty() && !Shortcuts::assignable(sequence, m_registry.platform(), &why)) {
            reason->setText(why);
            reason->setVisible(true);
            return;
        }
        // Every other command whose chord equals the sequence or is a prefix
        // either way loses that chord, after one question naming them all.
        if (const QStringList others = m_registry.conflicts(id, sequence); !others.isEmpty()) {
            QStringList names;
            for (const auto &other : others) {
                const auto *otherCommand = Shortcuts::command(other);
                names.append(otherCommand ? Shortcuts::title(*otherCommand) : other);
            }
            if (QMessageBox::question(&dialog, tr("Shortcut in use"), tr("Already used by %1. Reassign?").arg(QLocale().createSeparatedList(names))) != QMessageBox::Yes)
                return;
            for (const auto &other : others) {
                auto otherKeys = m_registry.keys(other);
                otherKeys.removeIf([&sequence](const QKeySequence &key) { return overlaps(key, sequence); });
                m_registry.setKeys(other, otherKeys);
            }
        }
        // This command's own slot may have shifted if it lost nothing; re-read.
        keys = m_registry.keys(id);
        if (sequence.isEmpty()) {
            if (slot < keys.size()) keys.removeAt(slot);
        } else if (slot < keys.size()) {
            keys[slot] = sequence;
            // The other slot goes when it is the same chord or a prefix either way.
            for (int i = keys.size() - 1; i >= 0; --i)
                if (i != slot && overlaps(keys.at(i), sequence)) keys.removeAt(i);
        } else {
            keys.removeIf([&sequence](const QKeySequence &key) { return overlaps(key, sequence); });
            keys.append(sequence);
        }
        m_registry.setKeys(id, keys);
        dialog.accept();
    });
    dialog.exec();
}
