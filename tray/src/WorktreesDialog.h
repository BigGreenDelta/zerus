#pragma once

#include "FleetState.h"
#include "SessionOrganization.h"
#include <QDialog>
#include <QMap>
#include <QSet>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

// A project-catalog overview. Catalog reads are bounded; cleanup still belongs
// to WorktreeCleanupDialog and the owning machine's reviewed removal contract.
class WorktreesDialog : public QDialog {
public:
    WorktreesDialog(HgsClient *client, const SessionOrganization &projects,
                    const FleetState &fleet, bool allProjects, QWidget *parent = nullptr);
    void setContext(const SessionOrganization &projects, const FleetState &fleet, bool allProjects);
    bool changed() const { return m_changed; }
private:
    struct Source {
        QString host, machine, path;
        QStringList projectIds, projectNames;
        QJsonObject catalog;
        QString error;
        bool attempted = false, fresh = false;
    };
    struct Request { QString source, host, path; quint64 generation; };
    static QString key(const QString &host, const QString &path);
    const BoxState *box(const Source &source) const;
    bool online(const Source &source) const;
    Source *selectedSource();
    QJsonObject selectedRow() const;
    QJsonObject reviewedRow(const Source &source, const QJsonObject &row) const;
    QString state(const Source &source, const QJsonObject &row) const;
    QStringList sessions(const Source &source, const QJsonObject &row) const;
    bool canClean(const Source &source) const;
    void refresh();
    void pump();
    void render();
    void showDetails();
    void reviewSelection();
    void cleanUp(bool focused);
    HgsClient *m_client;
    FleetState m_fleet;
    QByteArray m_context, m_rendered;
    QMap<QString, Source> m_sources;
    QMap<quint64, Request> m_requests;
    QMap<QString, QJsonObject> m_reviews;
    QMap<QString, QString> m_reviewErrors;
    QSet<QString> m_reviewAttempted;
    quint64 m_generation = 0, m_reviewRequest = 0, m_reviewGeneration = 0;
    QString m_reviewKey;
    bool m_changed = false;
    QComboBox *m_project, *m_computer;
    QTreeWidget *m_tree;
    QPlainTextEdit *m_details;
    QLabel *m_status;
    QPushButton *m_refresh, *m_cleanup, *m_remove, *m_copy;
};
