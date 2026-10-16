#include "GraphProjection.h"
#include "SessionCardDelegate.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QtTest>

class TestGraphProjection : public QObject {
    Q_OBJECT
    const QString machine = "11111111-1111-4111-8111-111111111111";
    QString id(const QString &kind, char c) const {
        return kind + ':' + QString(64, QLatin1Char(c));
    }
    QJsonObject item(const QString &kind, const QJsonObject &value) const {
        return {{"kind", kind}, {"value", value}};
    }
    QJsonObject facts() const {
        return {{id("node", 'a'),
                 item("node", {{"endpoint", QJsonObject{{"machine", machine},
                                                        {"provider", "claude"},
                                                        {"namespace", "account-one"},
                                                        {"conversation", "conversation-one"},
                                                        {"provisional_run", ""}}}})},
                {id("binding", 'b'), item("binding", {{"node", id("node", 'a')},
                                                      {"name", "claude/project/work"},
                                                      {"run_id", "run-one"},
                                                      {"conversation_id", "conversation-one"},
                                                      {"provider", "claude"}})}};
    }
    void apply(SessionGraphStore &store, QJsonObject values, const QString &route = "mac",
               int at = 1) const {
        QJsonArray rows;
        int revision = 0;
        for (auto it = values.begin(); it != values.end(); ++it)
            rows.append(QJsonObject{{"id", it.key()},
                                    {"revision", ++revision},
                                    {"deleted", false},
                                    {"item", it.value()}});
        const QJsonObject page{{"schema", 1},
                               {"machine_id", machine},
                               {"epoch", "22222222-2222-4222-8222-222222222222"},
                               {"revision", revision},
                               {"cursor", "page"},
                               {"has_more", false},
                               {"reset_required", false},
                               {"coverage", QJsonObject()},
                               {"items", rows}};
        QCOMPARE(store.apply(route, page, false, {}, at), SessionGraphStore::Applied);
    }
    GraphProjection::Binding live() const {
        return {"mac",
                "claude/project/work",
                "run-one",
                "conversation-one",
                "claude",
                {},
                {},
                "project-id",
                "Same project label",
                {{SessionRoles::Title, "My work"},
                 {SessionRoles::Status, "Working"},
                 {SessionRoles::Draft, true},
                 {SessionRoles::Unread, true},
                 {SessionRoles::Host, "My Mac"},
                 {SessionRoles::Agent, "claude"}},
                true};
    }
  private slots:
    void currentWorkScopeIsExplicitAndRecordedMetadataCannotImpersonateActivity() {
        SessionGraphStore store;
        apply(store, facts());
        auto active = live();
        active.active = true;
        auto result = GraphProjection::compose(store, {active}, {"mac"});
        QCOMPARE(result.nodes.size(), 1);
        QVERIFY(result.nodes[0].active);
        QVERIFY(!result.nodes[0].saved);
        active.active = false;
        active.saved = true;
        result = GraphProjection::compose(store, {active}, {"mac"});
        QVERIFY(!result.nodes[0].active);
        QVERIFY(result.nodes[0].saved);
        result = GraphProjection::compose(store, {}, {"mac"});
        QVERIFY(!result.nodes[0].active);
        QVERIFY(!result.nodes[0].saved);
    }
    void verifiedCrossMachineDeliveriesAggregateDistinctRequestsAndKeepBoundaryIdentity() {
        const auto endpoint = [&](const QString &host, const QString &conversation) {
            return QJsonObject{{"machine", host},
                               {"provider", "claude"},
                               {"namespace", "account:" + QString(64, 'a')},
                               {"conversation", conversation},
                               {"provisional_run", ""}};
        };
        const auto key = [](const QJsonObject &v) {
            return "node:" +
                   QString::fromLatin1(
                       QCryptographicHash::hash(QJsonDocument(v).toJson(QJsonDocument::Compact),
                                                QCryptographicHash::Sha256)
                           .toHex());
        };
        const auto from = endpoint(machine, "one"),
                   to = endpoint("33333333-3333-4333-8333-333333333333", "two");
        QJsonObject values{{key(from), item("node", {{"endpoint", from}})}};
        for (int i = 0; i < 2; ++i)
            values[id("event", char('b' + i))] =
                item("event",
                     {{"type", "message"},
                      {"from", key(from)},
                      {"to", key(to)},
                      {"source_endpoint", from},
                      {"target_endpoint", to},
                      {"source_name", "claude/project/source"},
                      {"target_name", "claude/other/target"},
                      {"request_id", QString("10000000-0000-4000-8000-00000000000%1").arg(i + 1)},
                      {"at", 100 + i},
                      {"delivery_state", "submitted"},
                      {"evidence", "verified_zerus_delivery"}});
        SessionGraphStore store;
        apply(store, values);
        const auto result = GraphProjection::compose(store, {}, {"mac"});
        QCOMPARE(result.nodes.size(), 2);
        QCOMPARE(result.relations.size(), 1);
        QCOMPARE(result.relations[0].events.size(), 2);
        QCOMPARE(result.relations[0].line.kind, QString("message"));
        QCOMPARE(result.relations[0].line.from, key(from));
        QCOMPARE(result.relations[0].line.to, key(to));
        QVERIFY(result.nodes[0].targets.isEmpty());
        QVERIFY(result.nodes[1].targets.isEmpty());
        auto repeated = values;
        repeated[id("event", 'd')] = values[id("event", 'b')];
        SessionGraphStore duplicate;
        apply(duplicate, repeated);
        const auto deduplicated = GraphProjection::compose(duplicate, {}, {"mac"});
        QCOMPARE(deduplicated.relations.size(), 1);
        QCOMPARE(deduplicated.relations[0].events.size(), 2);
        auto forged = values;
        auto row = forged[id("event", 'b')].toObject();
        auto value = row["value"].toObject();
        value["evidence"] = "quoted_prose";
        row["value"] = value;
        forged[id("event", 'b')] = row;
        SessionGraphStore rejected;
        apply(rejected, forged);
        const auto partial = GraphProjection::compose(rejected, {}, {"mac"});
        QCOMPARE(partial.relations.size(), 1);
        QCOMPARE(partial.relations[0].events.size(), 1);
        QVERIFY(partial.unresolved > 0);
    }
    void currentNativeChildrenReuseExactParentAndSharedStatusRoles() {
        SessionGraphStore store;
        apply(store, facts());
        auto child = live();
        child.agent = "native-child";
        child.generation = 2;
        child.completed = false;
        child.roles = {{SessionRoles::Title, "Native reviewer"},
                       {SessionRoles::Status, "Working"},
                       {SessionRoles::Working, true},
                       {SessionRoles::ChildId, "native-child"},
                       {SessionRoles::Agent, "claude"}};
        auto result = GraphProjection::compose(store, {live(), child}, {"mac"});
        QCOMPARE(result.nodes.size(), 2);
        QCOMPARE(result.relations.size(), 1);
        const auto found =
            std::find_if(result.nodes.cbegin(), result.nodes.cend(), [&](const GraphPage::Node &n) {
                return n.card.roles.value(SessionRoles::ChildId) == "native-child";
            });
        QVERIFY(found != result.nodes.cend());
        QCOMPARE(found->card.roles, child.roles);
        QCOMPARE(found->targets.size(), 1);
        QVERIFY(found->targets[0].available);
        QCOMPARE(found->targets[0].agent, child.agent);
        child.run = "replacement";
        result = GraphProjection::compose(store, {live(), child}, {"mac"});
        QCOMPARE(result.nodes.size(), 1);
        QVERIFY(result.relations.isEmpty());
    }
    void equivalentArchiveAndLiveChildProofsRetainOneStableIdentity() {
        auto values = facts();
        auto archived = values[id("binding", 'b')].toObject()["value"].toObject();
        archived["archived"] = true;
        archived["archive_id"] = "33333333-3333-4333-8333-333333333333";
        values[id("binding", 'c')] = item("binding", archived);
        const QJsonObject child{{"parent", id("node", 'a')},
                                {"agent_id", "reviewer"},
                                {"generation", 1},
                                {"provider", "claude"},
                                {"binding", id("binding", 'b')},
                                {"source", "native_roster"},
                                {"state", "working"}};
        auto oldChild = child;
        oldChild["binding"] = id("binding", 'c');
        oldChild["state"] = "finished";
        values[id("child", 'd')] =
            item("conflict",
                 {{"observations", QJsonArray{item("child", oldChild), item("child", child)}}});
        const QJsonObject edge{{"from", id("node", 'a')},
                               {"to", id("child", 'd')},
                               {"type", "parent"},
                               {"evidence", "native_roster"},
                               {"binding", id("binding", 'b')}};
        auto oldEdge = edge;
        oldEdge["binding"] = id("binding", 'c');
        values[id("relation", 'e')] =
            item("conflict",
                 {{"observations", QJsonArray{item("relation", oldEdge), item("relation", edge)}}});
        SessionGraphStore store;
        apply(store, values);
        auto result = GraphProjection::compose(store, {live()}, {"mac"});
        QCOMPARE(result.nodes.size(), 2);
        QCOMPARE(result.relations.size(), 1);
        QCOMPARE(result.relations[0].line.to, id("child", 'd'));
        const auto parent =
            std::find_if(result.nodes.cbegin(), result.nodes.cend(),
                         [&](const auto &n) { return n.card.id == id("node", 'a'); });
        QVERIFY(parent != result.nodes.cend());
        QVERIFY(!parent->archived);
        QCOMPARE(parent->card.roles.value(SessionRoles::Title).toString(), QString("My work"));
        oldChild["generation"] = 2;
        values[id("child", 'd')] =
            item("conflict",
                 {{"observations", QJsonArray{item("child", oldChild), item("child", child)}}});
        SessionGraphStore contradictory;
        apply(contradictory, values);
        result = GraphProjection::compose(contradictory, {live()}, {"mac"});
        QCOMPARE(result.nodes.size(), 1);
        QVERIFY(result.relations.isEmpty());
        QVERIFY(result.unresolved > 0);
    }
    void provisionalAliasRequiresExactNativeEndpointAndRunProof() {
        QJsonObject endpoint{{"machine", machine},
                             {"provider", "claude"},
                             {"namespace", "account:" + QString(64, 'a')},
                             {"conversation", "native"},
                             {"provisional_run", ""}};
        const auto key = [](const QJsonObject &v) {
            return "node:" +
                   QString::fromLatin1(
                       QCryptographicHash::hash(QJsonDocument(v).toJson(QJsonDocument::Compact),
                                                QCryptographicHash::Sha256)
                           .toHex());
        };
        const auto to = key(endpoint);
        auto prior = endpoint;
        prior["conversation"] = "";
        prior["provisional_run"] = "run-one";
        const auto from = key(prior);
        QJsonObject values{
            {to, item("node", {{"endpoint", endpoint}})},
            {id("binding", 'b'), item("binding", {{"node", to},
                                                  {"name", "claude/project/work"},
                                                  {"run_id", "run-one"},
                                                  {"conversation_id", "native"},
                                                  {"provider", "claude"}})},
            {id("alias", 'c'),
             item("alias", {{"from", from}, {"to", to}, {"binding", id("binding", 'b')}})}};
        SessionGraphStore store;
        apply(store, values);
        auto result = GraphProjection::compose(store, {}, {"mac"});
        QCOMPARE(result.aliases.value(from), to);
        values[id("alias", 'c')] =
            item("alias", {{"from", id("node", 'd')}, {"to", to}, {"binding", id("binding", 'b')}});
        SessionGraphStore forged;
        apply(forged, values);
        result = GraphProjection::compose(forged, {}, {"mac"});
        QVERIFY(result.aliases.isEmpty());
        QVERIFY(result.unresolved > 0);
    }
    void exactBindingPreservesSharedRolesWithoutOpeningAnything() {
        SessionGraphStore store;
        apply(store, facts());
        const auto b = live();
        const auto result = GraphProjection::compose(store, {b}, {"mac"});
        QCOMPARE(result.nodes.size(), 1);
        QCOMPARE(result.relations.size(), 0);
        QCOMPARE(result.nodes[0].card.roles, b.roles);
        QCOMPARE(result.nodes[0].card.group, b.project);
        QCOMPARE(result.nodes[0].targets.size(), 1);
        QVERIFY(result.nodes[0].targets[0].available);
        QCOMPARE(result.nodes[0].targets[0].run, b.run);
    }
    void reusedNamesAndDifferentRoutesCannotBorrowCurrentState() {
        SessionGraphStore store;
        apply(store, facts());
        const auto original = live();
        for (int field = 0; field < 4; ++field) {
            auto b = original;
            if (field == 0)
                b.run = "new-run";
            if (field == 1)
                b.conversation = "new-conversation";
            if (field == 2)
                b.provider = "codex";
            if (field == 3)
                b.route = "other";
            const auto result = GraphProjection::compose(store, {b}, {"mac", "other"});
            QCOMPARE(result.nodes.size(), 1);
            QVERIFY(!result.nodes[0].targets[0].available);
            QVERIFY(!result.nodes[0].card.roles.value(SessionRoles::Draft).toBool());
        }
    }
    void cachedRouteRemainsVisibleButUnavailable() {
        SessionGraphStore store;
        apply(store, facts());
        const auto result = GraphProjection::compose(store, {live()}, {});
        QCOMPARE(result.nodes.size(), 1);
        QVERIFY(!result.nodes[0].targets[0].available);
        QCOMPARE(result.nodes[0].card.roles.value(SessionRoles::Draft).toBool(), true);
    }
    void aliasesChooseOneCoherentMachineProjection() {
        SessionGraphStore store;
        apply(store, facts(), "older", 100);
        apply(store, facts(), "mac", 1);
        auto result = GraphProjection::compose(store, {live()}, {"mac"});
        QCOMPARE(result.nodes.size(), 1);
        QVERIFY(result.nodes[0].targets[0].available);
        result = GraphProjection::compose(store, {live()}, {});
        QCOMPARE(result.nodes.size(), 1);
        QVERIFY(!result.nodes[0].targets[0].available);
        QCOMPARE(result.nodes[0].targets[0].host, QString("older"));
    }
    void severalRunsRetainExplicitChoicesAndProjectConflict() {
        auto values = facts();
        auto extra = values[id("binding", 'b')].toObject();
        auto v = extra["value"].toObject();
        v["name"] = "claude/project/resumed";
        v["run_id"] = "run-two";
        extra["value"] = v;
        values[id("binding", 'c')] = extra;
        SessionGraphStore store;
        apply(store, values);
        auto other = live();
        other.name = "claude/project/resumed";
        other.run = "run-two";
        other.project = "other-id";
        const auto result = GraphProjection::compose(store, {live(), other}, {"mac"});
        QCOMPARE(result.nodes.size(), 1);
        QCOMPARE(result.nodes[0].targets.size(), 2);
        QCOMPARE(result.nodes[0].card.group, QString("#project-conflict"));
    }
    void ambiguityAndForeignMachineFailClosed() {
        SessionGraphStore store;
        apply(store, facts());
        auto result = GraphProjection::compose(store, {live(), live()}, {"mac"});
        QVERIFY(!result.nodes[0].targets[0].available);
        QVERIFY(result.unresolved > 0);
        auto values = facts();
        auto n = values[id("node", 'a')].toObject();
        auto v = n["value"].toObject();
        auto e = v["endpoint"].toObject();
        e["machine"] = "33333333-3333-4333-8333-333333333333";
        v["endpoint"] = e;
        n["value"] = v;
        values[id("node", 'a')] = n;
        SessionGraphStore foreign;
        apply(foreign, values);
        result = GraphProjection::compose(foreign, {live()}, {"mac"});
        QVERIFY(result.nodes.isEmpty());
        QVERIFY(result.unresolved > 0);
    }
    void nativeChildEvidenceIsSeparateFromUnverifiedMessages() {
        auto values = facts();
        values[id("child", 'c')] = item("child", {{"parent", id("node", 'a')},
                                                  {"agent_id", "child-one"},
                                                  {"generation", 1},
                                                  {"provider", "claude"},
                                                  {"binding", id("binding", 'b')},
                                                  {"source", "native_roster"}});
        values[id("relation", 'd')] = item("relation", {{"from", id("node", 'a')},
                                                        {"to", id("child", 'c')},
                                                        {"type", "parent"},
                                                        {"evidence", "native_roster"},
                                                        {"binding", id("binding", 'b')}});
        values[id("relation", 'e')] = item("relation", {{"from", id("node", 'a')},
                                                        {"to", id("child", 'c')},
                                                        {"type", "message"},
                                                        {"evidence", "quoted_prose"}});
        SessionGraphStore store;
        apply(store, values);
        const auto result = GraphProjection::compose(store, {live()}, {"mac"});
        QCOMPARE(result.nodes.size(), 2);
        QCOMPARE(result.relations.size(), 1);
        QCOMPARE(result.relations[0].line.kind, QString("parent"));
        QVERIFY(result.unresolved > 0);
    }
    void canvasLimitReportsOmittedNodesAndKeepsCurrentWork() {
        auto values = facts();
        values[id("node", '0')] = values[id("node", 'a')].toObject();
        SessionGraphStore store;
        apply(store, values);
        const auto result = GraphProjection::compose(store, {live()}, {"mac"}, 1);
        QCOMPARE(result.nodes.size(), 1);
        QCOMPARE(result.nodes[0].card.id, id("node", 'a'));
        QCOMPARE(result.omitted, 1);
    }
};
QTEST_GUILESS_MAIN(TestGraphProjection)
#include "test_graphprojection.moc"
