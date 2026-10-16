#include "GraphProjection.h"
#include "SessionCardDelegate.h"
#include "SessionOrganization.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace {
QString key(const QString &route, const QJsonObject &binding) {
    // JSON preserves component boundaries, including newlines in bad input.
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{route, binding["name"], binding["run_id"],
                                 binding["conversation_id"], binding["provider"],
                                 binding["archive_id"].toString()})
            .toJson(QJsonDocument::Compact));
}
QString key(const GraphProjection::Binding &binding) {
    return key(binding.route, {{"name", binding.name},
                               {"run_id", binding.run},
                               {"conversation_id", binding.conversation},
                               {"provider", binding.provider},
                               {"archive_id", binding.archive}});
}
QString endpointKey(const QJsonObject &endpoint) {
    static const QRegularExpression account("^account:[0-9a-f]{64}$");
    const auto machine = endpoint["machine"].toString(), provider = endpoint["provider"].toString();
    const QUuid id(machine);
    const auto conversation = endpoint["conversation"].toString(),
               run = endpoint["provisional_run"].toString();
    if (endpoint.size() != 5 || id.isNull() || id.toString(QUuid::WithoutBraces) != machine ||
        !QStringList{"claude", "codex", "kimi", "dsh", "opencode"}.contains(provider) ||
        !account.match(endpoint["namespace"].toString()).hasMatch() ||
        (conversation.isEmpty() == run.isEmpty()) || conversation.toUtf8().size() > 512 ||
        run.toUtf8().size() > 128)
        return {};
    for (const auto &value : {conversation, run})
        for (auto c : value)
            if (c.isNull() || c.category() == QChar::Other_Control)
                return {};
    return "node:" + QString::fromLatin1(QCryptographicHash::hash(
                                             QJsonDocument(endpoint).toJson(QJsonDocument::Compact),
                                             QCryptographicHash::Sha256)
                                             .toHex());
}
struct Fact {
    QString route, machine, kind;
    QJsonObject value;
};
} // namespace

GraphProjection::Result GraphProjection::compose(const SessionGraphStore &store,
                                                 const QList<Binding> &bindings,
                                                 const QSet<QString> &confirmed, int limit,
                                                 const QJsonObject &context) {
    Result result;
    SessionOrganization organization(context["organization"].toObject());
    const auto machineLabels = context["machines"].toObject();
    limit = qBound(1, limit, 2000);
    QHash<QString, QList<const Binding *>> current;
    for (const auto &binding : bindings)
        if (binding.agent.isEmpty() && !binding.run.isEmpty())
            current[key(binding)].append(&binding);

    // Two configured SSH aliases may reach the same machine. Use one coherent
    // projection, preferring a confirmed route and then the freshest observation.
    QMap<QString, QString> machines;
    for (const auto &route : store.routes()) {
        const auto *host = store.host(route);
        if (!host)
            continue;
        const auto old = machines.constFind(host->machine);
        if (old == machines.cend() ||
            (confirmed.contains(route) && !confirmed.contains(old.value())) ||
            (confirmed.contains(route) == confirmed.contains(old.value()) &&
             host->sampledAt > store.host(old.value())->sampledAt))
            machines[host->machine] = route;
    }
    QMap<QString, Fact> facts;
    QSet<QString> conflicts;
    for (const auto &route : machines) {
        const auto *host = store.host(route);
        for (auto it = host->items.begin(); it != host->items.end(); ++it) {
            const auto item = it.value().toObject();
            const Fact fact{route, host->machine, item["kind"].toString(),
                            item["value"].toObject()};
            if (facts.contains(it.key()))
                conflicts.insert(it.key());
            else
                facts.insert(it.key(), fact);
        }
    }
    for (const auto &id : conflicts)
        facts.remove(id);
    result.unresolved += conflicts.size();
    QMap<QString, GraphPage::Node> nodes;
    for (auto it = facts.cbegin(); it != facts.cend(); ++it) {
        if (it->kind != "node")
            continue;
        const auto endpoint = it->value["endpoint"].toObject();
        const auto provider = endpoint["provider"].toString();
        if (endpoint["machine"] != it->machine || endpoint["namespace"].toString().isEmpty() ||
            provider.isEmpty() ||
            (endpoint["conversation"].toString().isEmpty() &&
             endpoint["provisional_run"].toString().isEmpty())) {
            ++result.unresolved;
            continue;
        }
        GraphPage::Node node;
        node.card.id = it.key();
        node.card.roles = {
            {SessionRoles::Title, QObject::tr("Recorded conversation")},
            {SessionRoles::Status, QObject::tr("Recorded")},
            {SessionRoles::Agent, provider},
            {SessionRoles::Host, it->route.isEmpty() ? QObject::tr("This computer") : it->route}};
        node.identity =
            QObject::tr("Computer: %1\nConversation: %2")
                .arg(it->machine,
                     endpoint["conversation"].toString(endpoint["provisional_run"].toString()));
        node.detail = QObject::tr("Saved native identity. No current matching session binding.");
        nodes.insert(it.key(), node);
    }
    QHash<QString, QSet<QString>> projects;
    QSet<QString> presented, verifiedBindings;
    QHash<QString, QString> parentIds;
    for (auto it = facts.cbegin(); it != facts.cend(); ++it) {
        if (it->kind != "binding")
            continue;
        const auto id = it->value["node"].toString();
        auto node = nodes.find(id);
        if (node == nodes.end()) {
            ++result.unresolved;
            continue;
        }
        const auto endpoint = facts[id].value["endpoint"].toObject();
        const auto run = it->value["run_id"].toString();
        const auto name = it->value["name"].toString();
        if (facts[id].machine != it->machine || name.isEmpty() || run.isEmpty() ||
            it->value["provider"] != endpoint["provider"] ||
            it->value["conversation_id"] != endpoint["conversation"] ||
            (!endpoint["provisional_run"].toString().isEmpty() &&
             endpoint["provisional_run"] != run)) {
            ++result.unresolved;
            continue;
        }
        verifiedBindings.insert(it.key());
        parentIds[key(it->route, it->value)] = id;
        const auto matches = current.value(key(it->route, it->value));
        if (matches.size() == 1) {
            const auto &live = *matches.first();
            if (!presented.contains(id) || (node->archived && live.archive.isEmpty())) {
                node->card.roles = live.roles;
                node->card.group = live.project;
                node->project = live.projectLabel;
                node->detail =
                    QObject::tr("Exact native conversation and run matched to Sessions.");
                node->archived = !live.archive.isEmpty();
                presented.insert(id);
            }
            projects[id].insert(live.project);
            node->targets.append({live.route,
                                  live.name,
                                  live.run,
                                  live.conversation,
                                  live.archive,
                                  {},
                                  live.available && confirmed.contains(live.route),
                                  {},
                                  it->machine});
        } else {
            if (matches.size() > 1)
                ++result.unresolved;
            if (!presented.contains(id)) {
                node->card.roles[SessionRoles::Title] =
                    it->value["label"].toString().isEmpty() ? name : it->value["label"].toString();
                node->card.roles[SessionRoles::Meta] = it->value["cwd"].toString();
            }
            const auto archive = it->value["archive_id"].toString();
            if (!presented.contains(id)) {
                node->archived = it->value["archived"].toBool();
                if (node->archived)
                    node->card.roles[SessionRoles::Status] = QObject::tr("Archived");
            }
            const auto label = machineLabels[it->route].toString(
                it->route.isEmpty() ? QObject::tr("This computer") : it->route);
            node->card.roles[SessionRoles::Host] = label;
            node->card.roles[SessionRoles::MachineName] = label;
            const auto summary = it->value["summary"].toObject();
            const QUuid archiveUuid(archive);
            const bool archived = it->value["archived"].toBool() && !archiveUuid.isNull() &&
                                  archiveUuid.toString(QUuid::WithoutBraces) == archive &&
                                  summary["name"] == name && summary["run_id"] == run &&
                                  summary["conversation_id"] == endpoint["conversation"] &&
                                  summary["archive_id"] == archive;
            if (archived && !presented.contains(id)) {
                const auto identity = organization.observe(label, name, run, archive);
                node->card.group = organization.groupFor(identity);
                if (const auto *group = organization.group(node->card.group)) {
                    node->project = group->name;
                    node->card.roles[SessionRoles::ProjectColor] = QColor(group->color);
                    node->card.roles[SessionRoles::ProjectVivid] = group->vivid;
                }
            }
            node->targets.append({it->route,
                                  name,
                                  run,
                                  endpoint["conversation"].toString(),
                                  archive,
                                  {},
                                  archived && confirmed.contains(it->route),
                                  summary,
                                  it->machine});
        }
    }
    // A conversation with bindings assigned to different logical projects has
    // an explicit conflict, rather than silently inheriting the first project.
    for (auto it = projects.cbegin(); it != projects.cend(); ++it)
        if (it->size() > 1) {
            nodes[it.key()].card.group = QStringLiteral("#project-conflict");
            nodes[it.key()].project = QObject::tr("Multiple projects");
            nodes[it.key()].detail +=
                QObject::tr(" Current bindings belong to different projects.");
        }
    // A live and archived binding can contribute the same structural identity.
    // Resolve only equivalent proofs; different parents/generations/endpoints
    // remain conflicts. Prefer a current binding without merging action targets.
    for (auto it = facts.begin(); it != facts.end(); ++it) {
        if (it->kind != "conflict" || it->value["truncated"].toBool())
            continue;
        const auto observations = it->value["observations"].toArray();
        QString kind;
        QJsonObject identity, chosen;
        int preference = -1;
        bool equivalent = !observations.isEmpty();
        for (const auto &observation : observations) {
            const auto row = observation.toObject();
            const auto type = row["kind"].toString();
            const auto value = row["value"].toObject();
            QJsonObject stable;
            QStringList fields;
            if (type == "child")
                fields = {"parent", "agent_id", "generation", "provider", "source"};
            else if (type == "relation")
                fields = {"from", "to", "type", "evidence", "source_endpoint", "target_endpoint"};
            else if (type == "alias")
                fields = {"from", "to"};
            else {
                equivalent = false;
                break;
            }
            for (const auto &field : fields)
                stable[field] = value[field];
            if (kind.isEmpty()) {
                kind = type;
                identity = stable;
            } else if (kind != type || identity != stable) {
                equivalent = false;
                break;
            }
            const auto binding = value["binding"].toString();
            if (!verifiedBindings.contains(binding))
                continue;
            const int score = facts[binding].value["archived"].toBool() ? 0 : 1;
            if (score > preference) {
                preference = score;
                chosen = value;
            }
        }
        if (equivalent && preference >= 0) {
            it->kind = kind;
            it->value = chosen;
        } else
            ++result.unresolved;
    }
    QSet<QString> ambiguousAliases;
    for (auto it = facts.cbegin(); it != facts.cend(); ++it) {
        if (it->kind != "alias")
            continue;
        const auto binding = it->value["binding"].toString(), to = it->value["to"].toString();
        if (!verifiedBindings.contains(binding) || !nodes.contains(to) ||
            facts[to].machine != it->machine || facts[binding].value["node"] != to) {
            ++result.unresolved;
            continue;
        }
        auto endpoint = facts[to].value["endpoint"].toObject();
        if (endpointKey(endpoint) != to || endpoint["conversation"].toString().isEmpty()) {
            ++result.unresolved;
            continue;
        }
        endpoint["conversation"] = "";
        endpoint["provisional_run"] = facts[binding].value["run_id"];
        const auto from = endpointKey(endpoint);
        if (from.isEmpty() || it->value["from"] != from) {
            ++result.unresolved;
            continue;
        }
        if (ambiguousAliases.contains(from))
            continue;
        if (result.aliases.contains(from) && result.aliases[from] != to) {
            result.aliases.remove(from);
            ambiguousAliases.insert(from);
            ++result.unresolved;
        } else
            result.aliases[from] = to;
    }
    // Structural children retain their verified parent and generation; no
    // display-name or child-ID matching to an independently listed conversation.
    for (auto it = facts.cbegin(); it != facts.cend(); ++it) {
        if (it->kind != "child")
            continue;
        const auto parent = it->value["parent"].toString(),
                   agent = it->value["agent_id"].toString();
        const auto binding = facts.constFind(it->value["binding"].toString());
        const auto generation = it->value["generation"];
        if (!nodes.contains(parent) || facts[parent].machine != it->machine || agent.isEmpty() ||
            it->value["source"] != "native_roster" ||
            !verifiedBindings.contains(it->value["binding"].toString()) ||
            binding == facts.cend() || binding->kind != "binding" ||
            binding->machine != it->machine || binding->value["node"] != parent ||
            (generation.toString().isEmpty() && generation.toDouble() <= 0)) {
            ++result.unresolved;
            continue;
        }
        GraphPage::Node child;
        const auto label = it->value["label"].toString();
        child.card = {it.key(),
                      nodes[parent].card.group,
                      {{SessionRoles::Title, label.isEmpty() ? agent : label},
                       {SessionRoles::Status, QObject::tr("Recorded")},
                       {SessionRoles::Agent, it->value["provider"].toString()},
                       {SessionRoles::ChildId, agent},
                       {SessionRoles::Host, nodes[parent].card.roles.value(SessionRoles::Host)}}};
        child.project = nodes[parent].project;
        child.identity = QObject::tr("Native child: %1\nParent: %2").arg(agent, parent);
        child.detail = QObject::tr("Recorded native child relationship. Current "
                                   "child state is not verified.");
        child.archived = nodes[parent].archived;
        child.completedChild = it->value["state"] == "finished";
        for (const auto &target : nodes[parent].targets)
            if (target.run == binding->value["run_id"].toString() &&
                target.archive == binding->value["archive_id"].toString()) {
                auto childTarget = target;
                childTarget.agent = agent;
                child.targets.append(childTarget);
            }
        for (const auto &b : bindings)
            if (b.agent == agent && parentIds.value(key(b)) == parent &&
                b.generation == generation) {
                child.card.roles = b.roles;
                child.completedChild = b.completed;
                child.archived = !b.archive.isEmpty();
                child.detail =
                    QObject::tr("Exact parent conversation and native child matched to Sessions.");
                break;
            }
        nodes.insert(it.key(), child);
    }
    // Native API adapters can report children in the accepted fleet snapshot
    // without a persisted terminal-hook roster. Their exact parent binding is
    // still required; profile aggregates never enter this list.
    QList<GraphPage::Relation> currentChildren;
    for (const auto &b : bindings) {
        if (b.agent.isEmpty() || !parentIds.contains(key(b)))
            continue;
        const auto parent = parentIds.value(key(b));
        bool present = false;
        for (auto it = facts.cbegin(); it != facts.cend(); ++it)
            if (it->kind == "child" && it->value["parent"] == parent &&
                it->value["agent_id"] == b.agent && it->value["generation"] == b.generation) {
                present = true;
                break;
            }
        if (present)
            continue;
        const auto child =
            "child:" + QString::fromLatin1(QCryptographicHash::hash(
                                               QJsonDocument(QJsonArray{"current-native", parent,
                                                                        b.agent, b.generation})
                                                   .toJson(QJsonDocument::Compact),
                                               QCryptographicHash::Sha256)
                                               .toHex());
        GraphPage::Node node;
        node.card = {child, b.project, b.roles};
        node.project = b.projectLabel;
        node.archived = !b.archive.isEmpty();
        node.completedChild = b.completed;
        node.identity = QObject::tr("Native child: %1\nParent: %2").arg(b.agent, parent);
        node.detail = QObject::tr("Native child from the current exact parent snapshot.");
        node.targets.append({b.route,
                             b.name,
                             b.run,
                             b.conversation,
                             b.archive,
                             b.agent,
                             b.available && confirmed.contains(b.route),
                             {},
                             facts[parent].machine});
        nodes.insert(child, node);
        currentChildren.append({{"relation:" + child.mid(6), parent, child, "parent",
                                 QObject::tr("Native parent → child")},
                                QObject::tr("Verified by the accepted native child snapshot")});
    }
    // A delivery receipt may name a verified endpoint on a currently offline
    // computer. Keep a boundary node without borrowing a live name or route.
    QMap<QString, QList<QPair<QString, QJsonObject>>> exchanges;
    for (auto it = facts.cbegin(); it != facts.cend(); ++it) {
        if (it->kind != "event")
            continue;
        const auto &v = it->value;
        auto from = v["from"].toString(), to = v["to"].toString();
        const auto source = v["source_endpoint"].toObject(),
                   target = v["target_endpoint"].toObject();
        const QUuid request(v["request_id"].toString());
        const double at = v["at"].toDouble(-1);
        if (v["type"] != "message" || v["evidence"] != "verified_zerus_delivery" ||
            v["delivery_state"] != "submitted" || source["machine"] != it->machine ||
            endpointKey(source) != from || endpointKey(target) != to || request.isNull() ||
            request.toString(QUuid::WithoutBraces) != v["request_id"].toString() ||
            !std::isfinite(at) || at < 0 || at >= 253402300800.) {
            ++result.unresolved;
            continue;
        }
        from = result.aliases.value(from, from);
        to = result.aliases.value(to, to);
        for (const auto &pair : {qMakePair(from, source), qMakePair(to, target)})
            if (!nodes.contains(pair.first)) {
                GraphPage::Node node;
                node.card.id = pair.first;
                const auto label = v[pair.first == from ? "source_name" : "target_name"].toString();
                node.card.roles = {{SessionRoles::Title,
                                    label.isEmpty() ? QObject::tr("Recorded endpoint") : label},
                                   {SessionRoles::Agent, pair.second["provider"].toString()},
                                   {SessionRoles::Status, QObject::tr("Unavailable")},
                                   {SessionRoles::Host, pair.second["machine"].toString()}};
                node.identity = QObject::tr("Computer: %1\nConversation: %2")
                                    .arg(pair.second["machine"].toString(),
                                         pair.second["conversation"].toString());
                node.detail = QObject::tr("Verified delivery endpoint. No current "
                                          "configured binding is available.");
                nodes.insert(pair.first, node);
            }
        const auto relation =
            "relation:" +
            QString::fromLatin1(
                QCryptographicHash::hash(
                    QJsonDocument(QJsonArray{"message", from, to}).toJson(QJsonDocument::Compact),
                    QCryptographicHash::Sha256)
                    .toHex());
        auto presentedEvent = v; presentedEvent["from"] = from; presentedEvent["to"] = to;
        exchanges[relation].append({it.key(), presentedEvent});
    }
    // A retained native fork receipt can outlive its origin binding. Preserve
    // the verified origin as an unavailable boundary, without recreating a run.
    QMap<QString, Fact> forkBoundaries;
    for (auto it = facts.cbegin(); it != facts.cend(); ++it) {
        if (it->kind != "relation" || it->value["type"] != "fork" ||
            it->value["evidence"] != "native_fork_binding")
            continue;
        const auto from = it->value["from"].toString(), to = it->value["to"].toString(),
                   binding = it->value["binding"].toString();
        const auto source = it->value["source_endpoint"].toObject(),
                   target = it->value["target_endpoint"].toObject();
        if (nodes.contains(from) || !nodes.contains(to) || !verifiedBindings.contains(binding) ||
            facts[binding].value["node"] != to || source["machine"] != it->machine ||
            target["machine"] != it->machine || endpointKey(source) != from ||
            endpointKey(target) != to || facts[to].value["endpoint"] != target)
            continue;
        GraphPage::Node node;
        node.card = {from,
                     {},
                     {{SessionRoles::Title, QObject::tr("Fork origin")},
                      {SessionRoles::Status, QObject::tr("Unavailable")},
                      {SessionRoles::Agent, source["provider"].toString()},
                      {SessionRoles::Host, machineLabels[it->route].toString(it->route)}}};
        node.identity = QObject::tr("Computer: %1\nConversation: %2")
                            .arg(it->machine, source["conversation"].toString());
        node.detail = QObject::tr("Origin retained by a verified native fork binding. Its "
                                  "session binding is no longer available.");
        nodes[from] = node;
        forkBoundaries[from] = {it->route, it->machine, "node", {{"endpoint", source}}};
    }
    for (auto it = forkBoundaries.cbegin(); it != forkBoundaries.cend(); ++it)
        facts[it.key()] = it.value();
    // Live bindings first, then stable identity order. Never let unrelated
    // recorded history evict all current work from the bounded canvas.
    QStringList ordered = nodes.keys();
    std::stable_sort(ordered.begin(), ordered.end(), [&](const auto &a, const auto &b) {
        return presented.contains(a) > presented.contains(b);
    });
    QSet<QString> shown;
    for (const auto &id : ordered) {
        if (result.nodes.size() >= limit) {
            ++result.omitted;
            continue;
        }
        shown.insert(id);
        result.nodes.append(nodes[id]);
    }
    for (auto it = facts.cbegin(); it != facts.cend(); ++it) {
        if (it->kind != "relation")
            continue;
        const auto from = it->value["from"].toString(), to = it->value["to"].toString();
        const auto kind = it->value["type"].toString(), evidence = it->value["evidence"].toString();
        if (!shown.contains(from) || !shown.contains(to)) {
            ++result.unresolved;
            continue;
        }
        if ((kind != "parent" || evidence != "native_roster") &&
            (kind != "fork" || evidence != "native_fork_binding")) {
            ++result.unresolved;
            continue;
        }
        const auto binding = facts.constFind(it->value["binding"].toString());
        if (facts[from].machine != it->machine || facts[to].machine != it->machine ||
            !verifiedBindings.contains(it->value["binding"].toString()) ||
            binding == facts.cend() || binding->kind != "binding" ||
            binding->machine != it->machine || facts[from].kind != "node" ||
            (kind == "fork" && facts[to].kind != "node") ||
            binding->value["node"] != (kind == "parent" ? from : to) ||
            (kind == "parent" &&
             (facts[to].kind != "child" || facts[to].value["parent"] != from))) {
            ++result.unresolved;
            continue;
        }
        if (result.relations.size() >= 10000) {
            ++result.omitted;
            continue;
        }
        result.relations.append(
            {{it.key(), from, to, kind,
              kind == "parent" ? QObject::tr("Native parent → child") : QObject::tr("Native fork")},
             kind == "parent" ? QObject::tr("Verified by the saved native roster")
                              : QObject::tr("Verified by the confirmed native fork binding")});
    }
    for (auto it = exchanges.begin(); it != exchanges.end(); ++it) {
        auto &events = it.value();
        const auto from = events.first().second["from"].toString(),
                   to = events.first().second["to"].toString();
        if (!shown.contains(from) || !shown.contains(to)) {
            result.omitted += events.size();
            continue;
        }
        std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
            const double x = a.second["at"].toDouble(), y = b.second["at"].toDouble();
            return x == y ? a.first < b.first : x > y;
        });
        GraphPage::Relation relation{
            {it.key(), from, to, "message",
             QObject::tr("%n recorded message(s)", nullptr, events.size())},
            QObject::tr("Verified Zerus submission receipts. Submission does not "
                        "prove a reply or task completion. Times are local "
                        "observations, not cross-computer causal order.")};
        for (const auto &event : events)
            relation.events.append(QJsonObject{{"id", event.first},
                                               {"request_id", event.second["request_id"]},
                                               {"at", event.second["at"]},
                                               {"delivery_state", "submitted"},
                                               {"source_locator", event.second["source_locator"]}});
        if (result.relations.size() < 10000)
            result.relations.append(relation);
        else
            result.omitted += events.size();
    }
    for (const auto &relation : currentChildren)
        if (shown.contains(relation.line.from) && shown.contains(relation.line.to)) {
            if (result.relations.size() < 10000)
                result.relations.append(relation);
            else
                ++result.omitted;
        }
    return result;
}
