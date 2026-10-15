#include "GraphProjection.h"
#include "SessionCardDelegate.h"
#include <QJsonArray>
#include <QtTest>

class TestGraphProjection : public QObject {
    Q_OBJECT
    const QString machine = "11111111-1111-4111-8111-111111111111";
    QString id(const QString &kind, char c) const { return kind + ':' + QString(64, QLatin1Char(c)); }
    QJsonObject item(const QString &kind, const QJsonObject &value) const {
        return {{"kind",kind},{"value",value}};
    }
    QJsonObject facts() const {
        return {{id("node",'a'), item("node", {{"endpoint",QJsonObject{{"machine",machine},
            {"provider","claude"},{"namespace","account-one"},{"conversation","conversation-one"},{"provisional_run",""}}}})},
            {id("binding",'b'), item("binding", {{"node",id("node",'a')},{"name","claude/project/work"},
                {"run_id","run-one"},{"conversation_id","conversation-one"},{"provider","claude"}})}};
    }
    void apply(SessionGraphStore &store, QJsonObject values, const QString &route = "mac", int at = 1) const {
        QJsonArray rows;
        int revision = 0;
        for (auto it=values.begin();it!=values.end();++it)
            rows.append(QJsonObject{{"id",it.key()},{"revision",++revision},{"deleted",false},{"item",it.value()}});
        const QJsonObject page{{"schema",1},{"machine_id",machine},
            {"epoch","22222222-2222-4222-8222-222222222222"},{"revision",revision},
            {"cursor","page"},{"has_more",false},{"reset_required",false},{"coverage",QJsonObject()}, {"items",rows}};
        QCOMPARE(store.apply(route,page,false,{},at),SessionGraphStore::Applied);
    }
    GraphProjection::Binding live() const {
        return {"mac","claude/project/work","run-one","conversation-one","claude",{},{},
            "project-id","Same project label",{{SessionRoles::Title,"My work"},
            {SessionRoles::Status,"Working"},{SessionRoles::Draft,true},{SessionRoles::Unread,true},
            {SessionRoles::Host,"My Mac"},{SessionRoles::Agent,"claude"}},true};
    }
  private slots:
    void exactBindingPreservesSharedRolesWithoutOpeningAnything() {
        SessionGraphStore store; apply(store,facts());
        const auto b=live(); const auto result=GraphProjection::compose(store,{b},{"mac"});
        QCOMPARE(result.nodes.size(),1); QCOMPARE(result.relations.size(),0);
        QCOMPARE(result.nodes[0].card.roles,b.roles); QCOMPARE(result.nodes[0].card.group,b.project);
        QCOMPARE(result.nodes[0].targets.size(),1); QVERIFY(result.nodes[0].targets[0].available);
        QCOMPARE(result.nodes[0].targets[0].run,b.run);
    }
    void reusedNamesAndDifferentRoutesCannotBorrowCurrentState() {
        SessionGraphStore store; apply(store,facts());
        const auto original=live();
        for(int field=0;field<4;++field){
            auto b=original;
            if(field==0)b.run="new-run"; if(field==1)b.conversation="new-conversation";
            if(field==2)b.provider="codex"; if(field==3)b.route="other";
            const auto result=GraphProjection::compose(store,{b},{"mac","other"});
            QCOMPARE(result.nodes.size(),1); QVERIFY(!result.nodes[0].targets[0].available);
            QVERIFY(!result.nodes[0].card.roles.value(SessionRoles::Draft).toBool());
        }
    }
    void cachedRouteRemainsVisibleButUnavailable() {
        SessionGraphStore store; apply(store,facts());
        const auto result=GraphProjection::compose(store,{live()},{});
        QCOMPARE(result.nodes.size(),1); QVERIFY(!result.nodes[0].targets[0].available);
        QCOMPARE(result.nodes[0].card.roles.value(SessionRoles::Draft).toBool(),true);
    }
    void aliasesChooseOneCoherentMachineProjection() {
        SessionGraphStore store; apply(store,facts(),"older",100); apply(store,facts(),"mac",1);
        auto result=GraphProjection::compose(store,{live()},{"mac"});
        QCOMPARE(result.nodes.size(),1); QVERIFY(result.nodes[0].targets[0].available);
        result=GraphProjection::compose(store,{live()},{});
        QCOMPARE(result.nodes.size(),1); QVERIFY(!result.nodes[0].targets[0].available);
        QCOMPARE(result.nodes[0].targets[0].host,QString("older"));
    }
    void severalRunsRetainExplicitChoicesAndProjectConflict() {
        auto values=facts();auto extra=values[id("binding",'b')].toObject();
        auto v=extra["value"].toObject();v["name"]="claude/project/resumed";v["run_id"]="run-two";
        extra["value"]=v;values[id("binding",'c')]=extra;
        SessionGraphStore store;apply(store,values);auto other=live();other.name="claude/project/resumed";
        other.run="run-two";other.project="other-id";
        const auto result=GraphProjection::compose(store,{live(),other},{"mac"});
        QCOMPARE(result.nodes.size(),1);QCOMPARE(result.nodes[0].targets.size(),2);
        QCOMPARE(result.nodes[0].card.group,QString("#project-conflict"));
    }
    void ambiguityAndForeignMachineFailClosed() {
        SessionGraphStore store;apply(store,facts());
        auto result=GraphProjection::compose(store,{live(),live()},{"mac"});
        QVERIFY(!result.nodes[0].targets[0].available);QVERIFY(result.unresolved>0);
        auto values=facts();auto n=values[id("node",'a')].toObject();auto v=n["value"].toObject();
        auto e=v["endpoint"].toObject();e["machine"]="33333333-3333-4333-8333-333333333333";
        v["endpoint"]=e;n["value"]=v;values[id("node",'a')]=n;
        SessionGraphStore foreign;apply(foreign,values);
        result=GraphProjection::compose(foreign,{live()},{"mac"});
        QVERIFY(result.nodes.isEmpty());QVERIFY(result.unresolved>0);
    }
    void nativeChildEvidenceIsSeparateFromUnverifiedMessages() {
        auto values=facts();
        values[id("child",'c')]=item("child",{{"parent",id("node",'a')},{"agent_id","child-one"},
            {"generation",1},{"provider","claude"},{"binding",id("binding",'b')},{"source","native_roster"}});
        values[id("relation",'d')]=item("relation",{{"from",id("node",'a')},{"to",id("child",'c')},
            {"type","parent"},{"evidence","native_roster"},{"binding",id("binding",'b')}});
        values[id("relation",'e')]=item("relation",{{"from",id("node",'a')},{"to",id("child",'c')},
            {"type","message"},{"evidence","quoted_prose"}});
        SessionGraphStore store;apply(store,values);
        const auto result=GraphProjection::compose(store,{live()},{"mac"});
        QCOMPARE(result.nodes.size(),2);QCOMPARE(result.relations.size(),1);
        QCOMPARE(result.relations[0].line.kind,QString("parent"));QVERIFY(result.unresolved>0);
    }
    void canvasLimitReportsOmittedNodesAndKeepsCurrentWork() {
        auto values=facts();values[id("node",'0')]=values[id("node",'a')].toObject();
        SessionGraphStore store;apply(store,values);
        const auto result=GraphProjection::compose(store,{live()},{"mac"},1);
        QCOMPARE(result.nodes.size(),1);QCOMPARE(result.nodes[0].card.id,id("node",'a'));
        QCOMPARE(result.omitted,1);
    }
};
QTEST_GUILESS_MAIN(TestGraphProjection)
#include "test_graphprojection.moc"
