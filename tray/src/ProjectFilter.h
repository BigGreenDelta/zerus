#pragma once
#include "SessionOrganization.h"
#include <QMap>
#include <QWidget>
class QPushButton;
class QMenu;
class QLayout;

// Empty means all projects. Explicit IDs stay selected across catalog changes,
// including removal, so a missing project never silently broadens the scope.
class ProjectFilter : public QWidget {
    Q_OBJECT
public:
    explicit ProjectFilter(QWidget *parent = nullptr);
    QPushButton *button() const { return m_button; }
    QSet<QString> selection() const { return m_selection; }
    void setSelection(const QSet<QString> &projects);
    void setProjects(const QList<SessionOrganization::Group> &projects);
    void setSessionCounts(const QMap<QString, int> &counts, const QString &caption);
    void setTheme(bool dark);
    void setConcealed(bool concealed);
signals:
    void selectionChanged(const QSet<QString> &projects);
private:
    void rebuildChips();
    void rebuildMenu();
    void updateMenu();
    void toggle(const QString &id, bool selected);
    QString name(const QString &id) const;
    QColor color(const QString &id) const;
    bool vivid(const QString &id) const;
    QList<SessionOrganization::Group> m_projects;
    QSet<QString> m_selection;
    QMap<QString, int> m_counts;
    QString m_caption, m_signature;
    QPushButton *m_button;
    QMenu *m_menu;
    QLayout *m_flow;
    bool m_dark = false, m_concealed = false;
};
