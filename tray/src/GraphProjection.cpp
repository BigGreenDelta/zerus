#include "GraphProjection.h"
#include "SessionCardDelegate.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>

namespace {
QString key(const QString &route, const QJsonObject &binding) {
    // JSON preserves component boundaries, including newlines in bad input.
    return QString::fromUtf8(QJsonDocument(QJsonArray{route, binding["name"],
        binding["run_id"], binding["conversation_id"], binding["provider"]})
        .toJson(QJsonDocument::Compact));
}
QString key(const GraphProjection::Binding &binding) {
    return key(binding.route, {{"name", binding.name}, {"run_id", binding.run},
        {"conversation_id", binding.conversation}, {"provider", binding.provider}});
}
struct Fact {
    QString route, machine, kind;
    QJsonObject value;
};
}

GraphProjection::Result GraphProjection::compose(const SessionGraphStore &store,
    const QList<Binding> &bindings, const QSet<QString> &confirmed, int limit) {
    Result result;
    limit = qBound(1, limit, 2000);
    QHash<QString, QList<const Binding *>> current;
    for (const auto &binding : bindings)
        if (binding.agent.isEmpty() && !binding.run.isEmpty()) current[key(binding)].append(&binding);

    // Two configured SSH aliases may reach the same machine. Use one coherent
    // projection, preferring a confirmed route and then the freshest observation.
    QMap<QString, QString> machines;
    for (const auto &route : store.routes()) {
        const auto *host = store.host(route);
        if (!host) continue;
        const auto old = machines.constFind(host->machine);
        if (old == machines.cend() ||
            (confirmed.contains(route) && !confirmed.contains(old.value())) ||
            (confirmed.contains(route) == confirmed.contains(old.value()) &&
             host->sampledAt > store.host(old.value())->sampledAt)) machines[host->machine] = route;
    }
    QMap<QString, Fact> facts;
    QSet<QString> conflicts;
    for (const auto &route : machines) {
        const auto *host = store.host(route);
        for (auto it = host->items.begin(); it != host->items.end(); ++it) {
            const auto item = it.value().toObject();
            const Fact fact{route, host->machine, item["kind"].toString(), item["value"].toObject()};
            if (facts.contains(it.key())) conflicts.insert(it.key());
            else facts.insert(it.key(), fact);
        }
    }
    for (const auto &id : conflicts) facts.remove(id);
    result.unresolved += conflicts.size();
    QMap<QString, GraphPage::Node> nodes;
    for (auto it = facts.cbegin(); it != facts.cend(); ++it) {
        if (it->kind != "node") continue;
        const auto endpoint = it->value["endpoint"].toObject();
        const auto provider = endpoint["provider"].toString();
        if (endpoint["machine"] != it->machine || endpoint["namespace"].toString().isEmpty() ||
            provider.isEmpty() || (endpoint["conversation"].toString().isEmpty() &&
                                  endpoint["provisional_run"].toString().isEmpty())) {
            ++result.unresolved; continue;
        }
        GraphPage::Node node;
        node.card.id = it.key();
        node.card.roles = {{SessionRoles::Title, QObject::tr("Recorded conversation")},
            {SessionRoles::Status, QObject::tr("Recorded")}, {SessionRoles::Agent, provider},
            {SessionRoles::Host, it->route.isEmpty() ? QObject::tr("This computer") : it->route}};
        node.identity = QObject::tr("Computer: %1\nConversation: %2").arg(it->machine,
            endpoint["conversation"].toString(endpoint["provisional_run"].toString()));
        node.detail = QObject::tr("Saved native identity. No current matching session binding.");
        nodes.insert(it.key(), node);
    }
    QHash<QString, QSet<QString>> projects;
    QSet<QString> presented, verifiedBindings;
    for (auto it = facts.cbegin(); it != facts.cend(); ++it) {
        if (it->kind != "binding") continue;
        const auto id = it->value["node"].toString();
        auto node = nodes.find(id);
        if (node == nodes.end()) { ++result.unresolved; continue; }
        const auto endpoint = facts[id].value["endpoint"].toObject();
        const auto run = it->value["run_id"].toString();
        const auto name = it->value["name"].toString();
        if (facts[id].machine != it->machine || name.isEmpty() || run.isEmpty() ||
            it->value["provider"] != endpoint["provider"] ||
            it->value["conversation_id"] != endpoint["conversation"] ||
            (!endpoint["provisional_run"].toString().isEmpty() && endpoint["provisional_run"] != run)) {
            ++result.unresolved; continue;
        }
        verifiedBindings.insert(it.key());
        const auto matches = current.value(key(it->route, it->value));
        if (matches.size() == 1) {
            const auto &live = *matches.first();
            if (!presented.contains(id)) {
                node->card.roles = live.roles;
                node->card.group = live.project;
                node->project = live.projectLabel;
                node->detail = QObject::tr("Exact native conversation and run matched to Sessions.");
                presented.insert(id);
            }
            projects[id].insert(live.project);
            node->targets.append({live.route, live.name, live.run, live.conversation,
                                  live.archive, {}, live.available && confirmed.contains(live.route)});
        } else {
            if (matches.size() > 1) ++result.unresolved;
            if (!presented.contains(id)) {
                node->card.roles[SessionRoles::Title] = it->value["label"].toString().isEmpty()
                    ? name : it->value["label"].toString();
                node->card.roles[SessionRoles::Meta] = it->value["cwd"].toString();
            }
            node->targets.append({it->route, name, run, endpoint["conversation"].toString(), {}, {}, false});
        }
    }
    // A conversation with bindings assigned to different logical projects has
    // an explicit conflict, rather than silently inheriting the first project.
    for (auto it = projects.cbegin(); it != projects.cend(); ++it) if (it->size() > 1) {
        nodes[it.key()].card.group = QStringLiteral("#project-conflict");
        nodes[it.key()].project = QObject::tr("Multiple projects");
        nodes[it.key()].detail += QObject::tr(" Current bindings belong to different projects.");
    }
    // Structural children retain their verified parent and generation; no
    // display-name or child-ID matching to an independently listed conversation.
    for (auto it = facts.cbegin(); it != facts.cend(); ++it) {
        if (it->kind != "child") continue;
        const auto parent = it->value["parent"].toString(), agent = it->value["agent_id"].toString();
        const auto binding = facts.constFind(it->value["binding"].toString());
        const auto generation = it->value["generation"];
        if (!nodes.contains(parent) || facts[parent].machine != it->machine || agent.isEmpty() ||
            it->value["source"] != "native_roster" || !verifiedBindings.contains(it->value["binding"].toString()) || binding == facts.cend() ||
            binding->kind != "binding" || binding->machine != it->machine || binding->value["node"] != parent ||
            (generation.toString().isEmpty() && generation.toDouble() <= 0)) {
            ++result.unresolved; continue;
        }
        GraphPage::Node child;
        const auto label = it->value["label"].toString();
        child.card = {it.key(), nodes[parent].card.group,
            {{SessionRoles::Title, label.isEmpty() ? agent : label},
             {SessionRoles::Status, QObject::tr("Recorded")},
             {SessionRoles::Agent, it->value["provider"].toString()},
             {SessionRoles::ChildId, agent},
             {SessionRoles::Host, nodes[parent].card.roles.value(SessionRoles::Host)}}};
        child.project = nodes[parent].project;
        child.identity = QObject::tr("Native child: %1\nParent: %2").arg(agent, parent);
        child.detail = QObject::tr("Recorded native child relationship. Current child state is not verified.");
        nodes.insert(it.key(), child);
    }
    // Live bindings first, then stable identity order. Never let unrelated
    // recorded history evict all current work from the bounded canvas.
    QStringList ordered = nodes.keys();
    std::stable_sort(ordered.begin(), ordered.end(), [&](const auto &a, const auto &b) {
        return presented.contains(a) > presented.contains(b);
    });
    QSet<QString> shown;
    for (const auto &id : ordered) {
        if (result.nodes.size() >= limit) { ++result.omitted; continue; }
        shown.insert(id); result.nodes.append(nodes[id]);
    }
    for (auto it = facts.cbegin(); it != facts.cend(); ++it) {
        if (it->kind != "relation") continue;
        const auto from = it->value["from"].toString(), to = it->value["to"].toString();
        const auto kind = it->value["type"].toString(), evidence = it->value["evidence"].toString();
        if (!shown.contains(from) || !shown.contains(to)) { ++result.unresolved; continue; }
        if ((kind != "parent" || evidence != "native_roster") &&
            (kind != "fork" || evidence != "native_fork_binding")) { ++result.unresolved; continue; }
        const auto binding = facts.constFind(it->value["binding"].toString());
        if (facts[from].machine != it->machine || facts[to].machine != it->machine ||
            !verifiedBindings.contains(it->value["binding"].toString()) || binding == facts.cend() || binding->kind != "binding" || binding->machine != it->machine ||
            facts[from].kind != "node" || (kind == "fork" && facts[to].kind != "node") ||
            binding->value["node"] != (kind == "parent" ? from : to) ||
            (kind == "parent" && (facts[to].kind != "child" || facts[to].value["parent"] != from))) {
            ++result.unresolved; continue;
        }
        if (result.relations.size() >= 10000) { ++result.omitted; continue; }
        result.relations.append({{it.key(), from, to, kind,
            kind == "parent" ? QObject::tr("Native parent → child") : QObject::tr("Native fork")},
            kind == "parent" ? QObject::tr("Verified by the saved native roster")
                             : QObject::tr("Verified by the confirmed native fork binding")});
    }
    return result;
}
