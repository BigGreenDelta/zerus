#pragma once
#include "FleetState.h"
#include <QSet>
#include <QWidget>
class QPushButton;
class QMenu;
class QLayout;

// Empty selection means all machines; explicit selection remains explicit even
// when it currently contains every machine (new peers do not join it silently).
class MachineFilter : public QWidget {
    Q_OBJECT
public:
    explicit MachineFilter(QWidget *parent = nullptr);
    QPushButton *button() const { return m_button; }
    QSet<QString> selection() const { return m_selection; }
    void setSelection(const QSet<QString> &hosts);
    void setFleet(const FleetState &fleet);
    void setTheme(bool dark);
    void refreshColors();
    // The chip row shows with a selection unless its owner has no room for it
    // (the session strip); a poll then cannot show it for a moment.
    void setConcealed(bool concealed);
signals:
    void selectionChanged(const QSet<QString> &hosts);
private:
    void rebuildChips();
    void rebuildMenu();
    void toggle(const QString &host, bool selected);
    QString name(const QString &host) const;
    FleetState m_fleet;
    QSet<QString> m_selection;
    QPushButton *m_button;
    QMenu *m_menu;
    QLayout *m_flow;
    QString m_signature;
    bool m_dark = false, m_concealed = false;
};
