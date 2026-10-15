#pragma once
#include "Shortcuts.h"
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;

// Settings > Keyboard: every command by group with up to two chords, editing,
// conflict handling and reset. Spec: "Rebinding".
class KeyboardSettings : public QWidget {
    Q_OBJECT
public:
    explicit KeyboardSettings(ShortcutRegistry &registry, QWidget *parent = nullptr);
    // Opens the chord editor for a command and slot (0 or 1).
    void editChord(const QString &id, int slot);
private:
    void rebuild();
    void editItem(QTreeWidgetItem *item, int column);
    void updateButtons();
    ShortcutRegistry &m_registry;
    QTreeWidget *m_tree;
    QPushButton *m_reset;
    QPushButton *m_resetAll;
};
