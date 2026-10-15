#pragma once
#include "Shortcuts.h"
#include <QDialog>

class QLineEdit;
class QTreeWidget;

// The non-modal "Keyboard shortcuts" cheat sheet. Spec: "Cheat sheet".
class ShortcutsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShortcutsDialog(ShortcutRegistry &registry, QWidget *parent = nullptr);
signals:
    void customizeRequested();
private:
    void rebuild();
    ShortcutRegistry &m_registry;
    QLineEdit *m_filter;
    QTreeWidget *m_list;
};
