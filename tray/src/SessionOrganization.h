#pragma once

#include <QJsonObject>
#include <QHash>
#include <QList>
#include <QStringList>
#include <QSet>
#include "MachinePlatform.h"

// Logical projects own membership and named folders across machines.
// Moving a session changes its project, never its working directory or process.
// Missing/offline sessions remain in the stored order and return to their place.
class SessionOrganization {
public:
    struct Folder { QString id, machine, path, name, machineId, machineName; };
    struct Group { QString id, name; bool collapsed = false; QStringList sessions; QString color; QList<Folder> folders; bool vivid = false; bool accessible = true; };
    explicit SessionOrganization(const QJsonObject &data = {});
    const QList<Group> &groups() const { return m_groups; }
    const Group *group(const QString &id) const;
    QString groupFor(const QString &session) const;
    // Indices into the caller's list, in project order. Unknown identities stay
    // first in the default project; ties keep the caller's original order.
    QHash<QString, QList<int>> orderedMembers(const QStringList &sessions) const;
    QString createGroup(const QString &name);
    void renameGroup(const QString &id, const QString &name);
    bool removeGroup(const QString &id, const QString &destination = {});
    QString defaultProject() const { return m_default; }
    void setDefaultProject(const QString &id) { if (group(id)) m_default = id; }
    void setCollapsed(const QString &id, bool collapsed);
    void moveGroup(const QString &id, const QString &before = {});
    void moveSession(const QString &session, const QString &group, const QString &before = {});
    void setVivid(const QString &id, bool vivid);
    void setColor(const QString &id, const QString &color);
    // style is the folder form of the machine (MachinePlatform::of): drive and UNC
    // folders of the local Windows machine stay local (D15); peers are POSIX.
    QString addFolder(const QString &project, const QString &machine, const QString &path, const QString &name = {},
                      MachinePlatform::Style style = MachinePlatform::Style::Posix);
    bool editFolder(const QString &project, const QString &folder, const QString &machine, const QString &path, const QString &name,
                    MachinePlatform::Style style = MachinePlatform::Style::Posix);
    void removeFolder(const QString &project, const QString &folder);
    // Transfer catalog links only; copying creates a distinct shared folder ID.
    QString moveFolder(const QString &project, const QString &folder, const QString &destination);
    QString copyFolder(const QString &project, const QString &folder, const QString &destination);
    bool hasImported(const QString &machine) const { return m_imported.contains(machine); }
    void markImported(const QString &machine) { if (!m_imported.contains(machine)) m_imported.append(machine); }
    void renameSession(const QString &oldId, const QString &newId);
    // A run token recognizes an external rename; machine/name survives pause/resume.
    // First observation gives a local fallback row, not a shared assignment.
    QString observe(const QString &machine, const QString &name, const QString &run,
                    const QString &archive = {});
    QJsonObject toJson() const;
private:
    void indexGroups();
    void indexSessions();
    void appendSession(const QString &session, const QString &group);
    QString transferFolder(const QString &project, const QString &folder, const QString &destination, bool copy);
    QList<Group> m_groups;
    // Derived only from m_groups, never persisted or retained after removal.
    QHash<QString, int> m_groupIndexes;
    QHash<QString, QString> m_sessionGroups;
    QJsonObject m_runs;
    QJsonObject m_runGroups;
    QHash<QString,QString> m_currentRun;
    QStringList m_imported;
    QString m_default;
    QSet<QString> m_unassigned;
};
