#include "SessionOrganization.h"
#include <QJsonArray>
#include <QSet>
#include <QUuid>
#include <QColor>
#include <QDir>
#include <algorithm>
#include <utility>

namespace {
// Stored folders keep the form of their machine: local Windows folders are drive or
// UNC paths (MachinePlatform::clean), compared with either separator and any case.
bool sameFolder(const QString &a, const QString &b)
{
    using MachinePlatform::Style;
    if (MachinePlatform::windowsShaped(a) && MachinePlatform::windowsShaped(b))
        return MachinePlatform::same(MachinePlatform::clean(a, Style::Windows), MachinePlatform::clean(b, Style::Windows), Style::Windows);
    return QDir::cleanPath(a) == QDir::cleanPath(b);
}
}

SessionOrganization::SessionOrganization(const QJsonObject &data)
{
    QSet<QString> ids, sessions;
    for (const auto &value : data.value(data.contains("projects") ? "projects" : "groups").toArray()) {
        const auto obj = value.toObject();
        Group g{obj.value("id").toString(), obj.value("name").toString().trimmed().left(80), obj.value("collapsed").toBool(), {}};
        if (g.id.isEmpty() || g.name.isEmpty() || ids.contains(g.id)) continue;
        ids.insert(g.id);
        const auto color = QColor(obj.value("color").toString());
        g.color = color.isValid() ? color.name() : QStringLiteral("#64b5f6");
        g.vivid = obj.value("vivid").toBool(false);
        g.accessible = obj.value("accessible").toBool(true);
        QSet<QString> folders;
        for (const auto &value : obj.value("folders").toArray()) {
            const auto folder = value.toObject(); const auto machine = folder.value("machine").toString();
            const auto path = folder.value("path").toString(); const auto key = machine + '\n' + path;
            // Local Windows folders (D15) are drive or UNC paths, kept by this GUI only.
            if (machine.isEmpty() || (!path.startsWith('/') && !MachinePlatform::windowsShaped(path)) || folders.contains(key)) continue;
            folders.insert(key);
            const auto id = folder.value("id").toString(QUuid::createUuid().toString(QUuid::WithoutBraces));
            g.folders.append({id, machine, path, folder.value("name").toString(), folder.value("machine_id").toString(), folder.value("machine_name").toString()});
        }
        for (const auto &item : obj.value("sessions").toArray()) {
            const auto id = item.toString();
            if (!id.isEmpty() && !sessions.contains(id)) { g.sessions.append(id); sessions.insert(id); }
        }
        m_groups.append(g);
    }
    if (m_groups.isEmpty() || (!data.contains("default_project") && !ids.contains("ungrouped")))
        m_groups.append({"ungrouped", "General", false, {}, "#8d9baa", {}});
    indexGroups(); indexSessions();
    m_default = data.value("default_project").toString("ungrouped");
    if (!group(m_default) || m_default == "pinned") {
        for (const auto &g : m_groups) if (g.id != "pinned") { m_default = g.id; break; }
    }
    m_runs = data.value("runs").toObject();
    m_runGroups = data.value("run_groups").toObject();
    // Older caches have no marker: preserve their recorded assignments rather
    // than guessing which were implicit. Only newly observed rows are local.
    for (const auto &value : data.value("unassigned_sessions").toArray())
        if (sessions.contains(value.toString())) m_unassigned.insert(value.toString());
    const auto beforePin = data.value("before_pin").toObject();
    for (const auto &v : data.value("imported_machines").toArray()) if (!v.toString().isEmpty()) m_imported.append(v.toString());
    // Pinning used to move a session out of its group. Restore its real project
    // before discarding that presentation bucket; every session keeps one owner.
    QStringList pinned;
    for (int i = m_groups.size() - 1; i >= 0; --i) if (m_groups[i].id == "pinned") pinned = m_groups.takeAt(i).sessions;
    indexGroups(); indexSessions();
    for (const auto &session : pinned) {
        const auto previous = beforePin.value(session).toString();
        moveSession(session, group(previous) ? previous : m_default);
    }

}

const SessionOrganization::Group *SessionOrganization::group(const QString &id) const
{
    const auto found = m_groupIndexes.constFind(id);
    return found == m_groupIndexes.cend() ? nullptr : &m_groups[*found];
}

QString SessionOrganization::groupFor(const QString &session) const
{
    return m_sessionGroups.value(session, m_default);
}

void SessionOrganization::indexGroups()
{
    m_groupIndexes.clear();
    for (int i = 0; i < m_groups.size(); ++i) m_groupIndexes.insert(m_groups[i].id, i);
}

void SessionOrganization::indexSessions()
{
    m_sessionGroups.clear();
    for (const auto &g : std::as_const(m_groups))
        for (const auto &session : g.sessions)
            if (!m_sessionGroups.contains(session)) m_sessionGroups.insert(session, g.id);
}

void SessionOrganization::appendSession(const QString &session, const QString &id)
{
    const auto found = m_groupIndexes.constFind(id);
    if (found == m_groupIndexes.cend()) return;
    m_groups[*found].sessions.append(session);
    m_sessionGroups.insert(session, id);
}

QHash<QString, QList<int>> SessionOrganization::orderedMembers(const QStringList &sessions) const
{
    QHash<QString, int> positions;
    positions.reserve(m_sessionGroups.size());
    for (const auto &g : m_groups)
        for (int i = 0; i < g.sessions.size(); ++i)
            if (!positions.contains(g.sessions[i])) positions.insert(g.sessions[i], i);
    QHash<QString, QList<int>> result;
    QList<int> ranks;
    ranks.reserve(sessions.size());
    for (int i = 0; i < sessions.size(); ++i) {
        result[groupFor(sessions[i])].append(i);
        ranks.append(positions.value(sessions[i], -1));
    }
    for (auto &members : result)
        std::stable_sort(members.begin(), members.end(), [&](int a, int b) { return ranks[a] < ranks[b]; });
    return result;
}

QString SessionOrganization::createGroup(const QString &name)
{
    const QString title = name.trimmed().left(80);
    if (title.isEmpty()) return {};
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    int position = 0;
    while (position < m_groups.size() && m_groups[position].id != "ungrouped") ++position;
    m_groups.insert(position, {id, title, false, {}, "#64b5f6", {}});
    indexGroups();
    return id;
}

void SessionOrganization::renameGroup(const QString &id, const QString &name)
{
    if (name.trimmed().isEmpty()) return;
    for (auto &g : m_groups) if (g.id == id) g.name = name.trimmed().left(80);
}

bool SessionOrganization::removeGroup(const QString &id, const QString &destination)
{
    const auto target = destination.isEmpty() ? m_default : destination;
    if (id == target || !group(id) || !group(target)) return false;
    QStringList displaced;
    for (int i=0;i<m_groups.size();++i) if (m_groups[i].id==id) {displaced=m_groups.takeAt(i).sessions;break;}
    indexGroups();
    for (const auto &session : displaced) m_sessionGroups.remove(session);
    if (m_default==id) m_default=target;
    for (const auto &session:displaced) moveSession(session,target);
    return true;
}

void SessionOrganization::setCollapsed(const QString &id, bool collapsed)
{
    for (auto &g : m_groups) if (g.id == id) g.collapsed = collapsed;
}

void SessionOrganization::moveGroup(const QString &id, const QString &before)
{
    if (id == before || (!before.isEmpty() && !group(before))) return;
    int from = -1;
    for (int i = 0; i < m_groups.size(); ++i) if (m_groups[i].id == id) from = i;
    if (from < 0) return;
    const auto g = m_groups.takeAt(from);
    int to = 0;
    while (to < m_groups.size() && m_groups[to].id != before) ++to;
    m_groups.insert(to, g);
    indexGroups();
}

void SessionOrganization::moveSession(const QString &session, const QString &id, const QString &before)
{
    const auto *target = group(id);
    if (session.isEmpty() || !target || session == before || (!before.isEmpty() && !target->sessions.contains(before))) return;

    m_unassigned.remove(session);
    if(m_currentRun.contains(session))m_runGroups[m_currentRun.value(session)]=id;
    const auto previous = m_sessionGroups.constFind(session);
    if (previous != m_sessionGroups.cend()) m_groups[m_groupIndexes.value(*previous)].sessions.removeAll(session);
    auto &ordered = m_groups[m_groupIndexes.value(id)].sessions;
    const int position = before.isEmpty() ? -1 : ordered.indexOf(before);
    ordered.insert(position < 0 ? ordered.size() : position, session);
    m_sessionGroups.insert(session, id);
}

void SessionOrganization::renameSession(const QString &oldId, const QString &newId)
{
    if (oldId == newId || oldId.isEmpty() || newId.isEmpty()) return;
    if (!m_sessionGroups.contains(oldId)) return;
    const bool implicit = m_unassigned.remove(oldId);
    const bool assignedTarget = m_sessionGroups.contains(newId) && !m_unassigned.contains(newId);
    if (implicit && assignedTarget) {
        // A remote assignment for the new name outranks our old fallback row.
        for (auto &g : m_groups) g.sessions.removeAll(oldId);
    } else {
        for (auto &g : m_groups) g.sessions.removeAll(newId);
        for (auto &g : m_groups) for (auto &session : g.sessions) if (session == oldId) session = newId;
        m_unassigned.remove(newId);
        if (implicit) m_unassigned.insert(newId);
    }
    for (auto it = m_runs.begin(); it != m_runs.end(); ++it) if (it.value().toString() == oldId) it.value() = newId;
    if(m_currentRun.contains(oldId))m_currentRun[newId]=m_currentRun.take(oldId);
    indexSessions();
}

QString SessionOrganization::observe(const QString &machine, const QString &name, const QString &run, const QString &archive)
{
    const QString id = machine + '\n' + (archive.isEmpty() ? name : "archive\n" + archive);
    if (!run.isEmpty() && archive.isEmpty()) {
        const QString token = machine + '\n' + run;
        const auto old = m_runs.value(token).toString();
        if (!old.isEmpty()) renameSession(old, id);
        m_runs.insert(token, id);
        m_currentRun[id]=token;
    }
    if (!m_sessionGroups.contains(id)) {
        const auto prior=archive.isEmpty()?QString():m_runGroups.value(machine+'\n'+run).toString();
        if(!prior.isEmpty()&&group(prior))appendSession(id,prior);
        else {appendSession(id,m_default);m_unassigned.insert(id);}
    }
    if(archive.isEmpty()&&!run.isEmpty()){const auto token=machine+'\n'+run;if(m_unassigned.contains(id))m_runGroups.remove(token);else m_runGroups[token]=groupFor(id);}
    return id;
}

void SessionOrganization::setVivid(const QString &id, bool vivid)
{
    for (auto &g : m_groups) if (g.id == id) g.vivid = vivid;
}
void SessionOrganization::setColor(const QString &id, const QString &color)
{
    const QColor chosen(color); if (!chosen.isValid()) return;
    for (auto &g : m_groups) if (g.id == id) g.color = chosen.name();
}
QString SessionOrganization::addFolder(const QString &project, const QString &machine, const QString &path, const QString &name,
                                      MachinePlatform::Style style)
{
    if (machine.trimmed().isEmpty() || !MachinePlatform::absolute(path, style) || path.contains('\n')) return {};
    const auto clean = MachinePlatform::clean(path, style);
    for (auto &g : m_groups) if (g.id == project) {
        for (const auto &folder : g.folders) if (folder.machine == machine && MachinePlatform::same(folder.path, clean, style)) return folder.id;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        g.folders.append({id, machine, clean, name.trimmed().left(80)}); return id;
    }
    return {};
}
bool SessionOrganization::editFolder(const QString &project, const QString &id, const QString &machine, const QString &path, const QString &name,
                                     MachinePlatform::Style style)
{
    if (machine.isEmpty() || !MachinePlatform::absolute(path, style) || path.contains('\n')) return false;
    const auto clean = MachinePlatform::clean(path, style);
    for (auto &g : m_groups) if (g.id == project) {
        for (const auto &f : g.folders) if (f.id != id && f.machine == machine && MachinePlatform::same(f.path, clean, style)) return false;
        for (auto &f : g.folders) if (f.id == id) { if(f.machine!=machine){f.machineId.clear();f.machineName.clear();} f.machine=machine; f.path=clean; f.name=name.trimmed().left(80); return true; }
    }
    return false;
}
void SessionOrganization::removeFolder(const QString &project, const QString &folder)
{
    for (auto &g : m_groups) if (g.id == project) for (int i = 0; i < g.folders.size(); ++i)
        if (g.folders[i].id == folder) { g.folders.removeAt(i); return; }
}
QString SessionOrganization::moveFolder(const QString &project, const QString &folder, const QString &destination)
{
    return transferFolder(project, folder, destination, false);
}
QString SessionOrganization::copyFolder(const QString &project, const QString &folder, const QString &destination)
{
    return transferFolder(project, folder, destination, true);
}
QString SessionOrganization::transferFolder(const QString &project, const QString &id, const QString &destination, bool copy)
{
    if (project == destination || id.isEmpty()) return {};
    int from = -1, to = -1;
    for (int i = 0; i < m_groups.size(); ++i) {
        if (m_groups[i].id == project) from = i;
        if (m_groups[i].id == destination) to = i;
    }
    if (from < 0 || to < 0) return {};
    int index = -1;
    for (int i = 0; i < m_groups[from].folders.size(); ++i)
        if (m_groups[from].folders[i].id == id) { index = i; break; }
    if (index < 0) return {};
    auto folder = m_groups[from].folders[index];
    QString targetId;
    for (const auto &existing : m_groups[to].folders) {
        const bool sameMachine = !folder.machineId.isEmpty() && !existing.machineId.isEmpty()
            ? folder.machineId == existing.machineId : folder.machine == existing.machine;
        if (sameMachine && sameFolder(folder.path, existing.path)) {
            targetId = existing.id;
            break;
        }
    }
    if (targetId.isEmpty()) {
        if (copy) folder.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        targetId = folder.id;
        m_groups[to].folders.append(folder);
    }
    if (!copy) m_groups[from].folders.removeAt(index);
    return targetId;
}
QJsonObject SessionOrganization::toJson() const
{
    QJsonArray projects;
    for (const auto &g : m_groups) {
        QJsonArray folders;
        for (const auto &f : g.folders) folders.append(QJsonObject{{"id",f.id},{"machine",f.machine},{"path",f.path},{"name",f.name},{"machine_id",f.machineId},{"machine_name",f.machineName}});
        projects.append(QJsonObject{{"id", g.id}, {"name", g.name}, {"color",g.color}, {"vivid",g.vivid}, {"folders",folders},
            {"collapsed", g.collapsed}, {"accessible",g.accessible}, {"sessions", QJsonArray::fromStringList(g.sessions)}});
    }
    QStringList unassigned(m_unassigned.cbegin(), m_unassigned.cend()); unassigned.sort();
    return {{"version", 2}, {"default_project", m_default}, {"projects", projects}, {"runs", m_runs}, {"run_groups",m_runGroups},
        {"imported_machines",QJsonArray::fromStringList(m_imported)}, {"unassigned_sessions",QJsonArray::fromStringList(unassigned)}};
}
