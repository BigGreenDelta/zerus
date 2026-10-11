#pragma once

#include <QJsonObject>
#include <QList>
#include <QStringList>

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
    QString addFolder(const QString &project, const QString &machine, const QString &path, const QString &name = {});
    bool editFolder(const QString &project, const QString &folder, const QString &machine, const QString &path, const QString &name);
    void removeFolder(const QString &project, const QString &folder);
    // Transfer catalog links only; copying creates a distinct shared folder ID.
    QString moveFolder(const QString &project, const QString &folder, const QString &destination);
    QString copyFolder(const QString &project, const QString &folder, const QString &destination);
    bool hasImported(const QString &machine) const { return m_imported.contains(machine); }
    void markImported(const QString &machine) { if (!m_imported.contains(machine)) m_imported.append(machine); }
    void renameSession(const QString &oldId, const QString &newId);
    // A run token recognizes an external rename; machine/name survives pause/resume.
    QString observe(const QString &machine, const QString &name, const QString &run,
                    const QString &archive = {});
    QJsonObject toJson() const;
private:
    QString transferFolder(const QString &project, const QString &folder, const QString &destination, bool copy);
    QList<Group> m_groups;
    QJsonObject m_runs;
    QStringList m_imported;
    QString m_default;
};
