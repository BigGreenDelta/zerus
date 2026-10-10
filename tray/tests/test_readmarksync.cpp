#include <QtTest>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "HgsClient.h"
#include "ReadMarkSync.h"

class TestReadMarkSync : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void sendsExactPayloadToEachSessionHost();
    void unreachableHostBacksOffAndConfirmedMarksWaitForSnapshots();
    void limitsConcurrentHostWrites();
    void duplicateEntriesForOneKeyIssueOneWrite();
private:
    QList<QJsonObject> calls() const;
    void touch(const char *name) const {
        QFile file(m_dir.filePath(name));
        if (!file.open(QIODevice::WriteOnly)) QFAIL("cannot create the fixture flag");
    }
    QString hgs() const { return m_dir.filePath("hgs"); }
    QTemporaryDir m_dir;
};

void TestReadMarkSync::initTestCase()
{
    QVERIFY(m_dir.isValid());
    QFile script(hgs()); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
payload=json.load(sys.stdin)
with (root/'calls.jsonl').open('a') as log: log.write(json.dumps({'argv':sys.argv[1:],'payload':payload})+'\n')
if (root/'slow').exists(): time.sleep(0.5)
if (root/'fail').exists(): print('ssh: connect to host mac port 22: No route to host',file=sys.stderr); sys.exit(255)
read={'conversation_id':payload['conversation_id'],'reply_id':payload['reply_id'],'attention_signature':payload['attention_signature'],'at':1.0}
print(json.dumps({'ok':True,'applied':{'reply_id':payload['reply_id'] is not None,'attention_signature':payload['attention_signature'] is not None},'read':read}))
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
}

void TestReadMarkSync::init()
{
    for (const char *name : {"calls.jsonl", "slow", "fail"}) QFile::remove(m_dir.filePath(name));
}

QList<QJsonObject> TestReadMarkSync::calls() const
{
    QList<QJsonObject> result; QFile file(m_dir.filePath("calls.jsonl"));
    if (!file.open(QIODevice::ReadOnly)) return result;
    for (const auto &line : file.readAll().split('\n')) if (!line.isEmpty()) result << QJsonDocument::fromJson(line).object();
    return result;
}

void TestReadMarkSync::sendsExactPayloadToEachSessionHost()
{
    HgsClient client(hgs()); ReadMarkSync sync(&client); QSignalSpy done(&sync, &ReadMarkSync::markFinished);
    const ReadMark local{{}, "codex/project/review", "run-1", "conversation-1", "7:100.5", {}};
    const ReadMark peer{"mac", "claude/project/fix", "run-2", "conversation-2", {}, QString(64, 'a')};
    sync.setPending({{local.key(), local.toJson()}, {peer.key(), peer.toJson()}});
    QTRY_COMPARE(done.size(), 2); QVERIFY(done[0][1].toBool()); QVERIFY(done[1][1].toBool());
    QHash<QString, QJsonObject> byName;
    for (const auto &call : calls()) { const auto argv = call.value("argv").toArray(); byName.insert(argv.at(argv.size() - 2).toString(), call); }
    QCOMPARE(byName.value(local.name).value("argv").toArray(), QJsonArray({"read", local.name, "--json"}));
    QCOMPARE(byName.value(peer.name).value("argv").toArray(), QJsonArray({"@mac", "read", peer.name, "--json"}));
    QCOMPARE(byName.value(local.name).value("payload").toObject(), (QJsonObject{{"run_id", "run-1"},
        {"conversation_id", "conversation-1"}, {"reply_id", "7:100.5"}, {"attention_signature", QJsonValue()}}));
    QCOMPARE(byName.value(peer.name).value("payload").toObject(), (QJsonObject{{"run_id", "run-2"},
        {"conversation_id", "conversation-2"}, {"reply_id", QJsonValue()}, {"attention_signature", QString(64, 'a')}}));
}

void TestReadMarkSync::unreachableHostBacksOffAndConfirmedMarksWaitForSnapshots()
{
    qint64 now = 1000000;
    HgsClient client(hgs()); ReadMarkSync sync(&client); sync.setClock([&now] { return now; });
    QSignalSpy done(&sync, &ReadMarkSync::markFinished);
    touch("fail");
    const ReadMark mark{"mac", "codex/project/review", "run-1", "conversation-1", "7:100.5", {}};
    sync.setPending({{mark.key(), mark.toJson()}});
    QTRY_COMPARE(done.size(), 1); QVERIFY(!done[0][1].toBool()); QVERIFY(done[0][3].toString().contains("No route"));
    now += ReadMarkSync::kFirstBackoffMs - 1; sync.pump(); QTest::qWait(300); QCOMPARE(calls().size(), 1);
    QFile::remove(m_dir.filePath("fail"));
    now += 1; sync.pump(); QTRY_COMPARE(done.size(), 2); QVERIFY(done[1][1].toBool());
    // Delivered: written again only if no snapshot settles it for two minutes.
    now += ReadMarkSync::kConfirmedResendMs - 1; sync.pump(); QTest::qWait(300); QCOMPARE(calls().size(), 2);
    now += 1; sync.pump(); QTRY_COMPARE(done.size(), 3);
    // A newer mark for the same conversation is written at once.
    auto newer = mark; newer.replyId = "8:200.5"; sync.setPending({{newer.key(), newer.toJson()}});
    QTRY_COMPARE(done.size(), 4);
    QCOMPARE(calls().last().value("payload").toObject().value("reply_id").toString(), QString("8:200.5"));
    // A settled mark leaves the queue and is never written again.
    sync.setPending({}); now += ReadMarkSync::kConfirmedResendMs * 2; sync.pump(); QTest::qWait(300);
    QCOMPARE(calls().size(), 4);
}

void TestReadMarkSync::limitsConcurrentHostWrites()
{
    touch("slow");
    HgsClient client(hgs()); ReadMarkSync sync(&client); QSignalSpy done(&sync, &ReadMarkSync::markFinished);
    QJsonObject pending;
    for (int i = 0; i < 6; ++i) {
        const ReadMark mark{"mac", QString("codex/project/s%1").arg(i), "run", QString("conversation-%1").arg(i), "1:1.0", {}};
        pending[mark.key()] = mark.toJson();
    }
    sync.setPending(pending);
    QCOMPARE(sync.inFlight(), ReadMarkSync::kMaxInFlight);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 6, 10000);
    QCOMPARE(calls().size(), 6);
}

void TestReadMarkSync::duplicateEntriesForOneKeyIssueOneWrite()
{
    touch("slow");
    HgsClient client(hgs()); ReadMarkSync sync(&client); QSignalSpy done(&sync, &ReadMarkSync::markFinished);
    const ReadMark mark{"mac", "codex/project/review", "run-1", "conversation-1", "7:100.5", {}};
    auto older = mark; older.replyId = "6:90.5";
    // The same mark under two object keys, plus an older one: the latest entry wins.
    sync.setPending({{"a", older.toJson()}, {"b", mark.toJson()}, {"c", mark.toJson()}});
    QCOMPARE(sync.inFlight(), 1);
    QTRY_COMPARE(done.size(), 1);
    QTest::qWait(300);
    QCOMPARE(calls().size(), 1);
    QCOMPARE(calls().first().value("payload").toObject().value("reply_id").toString(), QString("7:100.5"));
}

QTEST_GUILESS_MAIN(TestReadMarkSync)
#include "test_readmarksync.moc"
