#include <QtTest>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "FakeCli.h"
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
    void busyHostLeavesSlotsForOtherHosts();
    void unappliedMarksAreWrittenLessOften();
    void transientlyUnappliedMarkIsRetriedAfterTwoMinutes();
    void keyedPendingPassesThroughOneWritePerKey();
    void duplicateKeysMergeFieldsWhateverTheObjectKeyOrder_data();
    void duplicateKeysMergeFieldsWhateverTheObjectKeyOrder();
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
if (root/'unapplied').exists(): print(json.dumps({'ok':True,'applied':{'reply_id':False,'attention_signature':False},'read':None})); sys.exit(0)
if (root/'reply-only').exists(): print(json.dumps({'ok':True,'applied':{'reply_id':True,'attention_signature':False},'read':dict(read,attention_signature=None)})); sys.exit(0)
print(json.dumps({'ok':True,'applied':{'reply_id':payload['reply_id'] is not None,'attention_signature':payload['attention_signature'] is not None},'read':read}))
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
}

void TestReadMarkSync::init()
{
    for (const char *name : {"calls.jsonl", "slow", "fail", "unapplied", "reply-only"}) QFile::remove(m_dir.filePath(name));
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
    HgsClient client(FakeCli::program(hgs())); ReadMarkSync sync(&client); QSignalSpy done(&sync, &ReadMarkSync::markFinished);
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
    HgsClient client(FakeCli::program(hgs())); ReadMarkSync sync(&client); sync.setClock([&now] { return now; });
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
    HgsClient client(FakeCli::program(hgs())); ReadMarkSync sync(&client); QSignalSpy done(&sync, &ReadMarkSync::markFinished);
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

void TestReadMarkSync::busyHostLeavesSlotsForOtherHosts()
{
    touch("slow");
    HgsClient client(FakeCli::program(hgs())); ReadMarkSync sync(&client); QSignalSpy done(&sync, &ReadMarkSync::markFinished);
    QJsonObject pending;
    for (int i = 0; i < 6; ++i) {
        const ReadMark mark{"alpha", QString("codex/project/s%1").arg(i), "run", QString("conversation-%1").arg(i), "1:1.0", {}};
        pending[mark.key()] = mark.toJson();
    }
    // Sorted after every mark of the busy host.
    const ReadMark other{"zeta", "codex/project/other", "run", "conversation", "1:1.0", {}};
    pending[other.key()] = other.toJson();
    sync.setPending(pending);
    QCOMPARE(sync.inFlight(), ReadMarkSync::kMaxInFlight);
    QTRY_COMPARE(calls().size(), ReadMarkSync::kMaxInFlight);
    QStringList hosts;
    for (const auto &call : calls()) hosts << call.value("argv").toArray().first().toString();
    QVERIFY2(hosts.contains("@zeta"), qPrintable(hosts.join(' ')));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 7, 10000);
}

void TestReadMarkSync::unappliedMarksAreWrittenLessOften()
{
    qint64 now = 1000000;
    HgsClient client(FakeCli::program(hgs())); ReadMarkSync sync(&client); sync.setClock([&now] { return now; });
    QSignalSpy done(&sync, &ReadMarkSync::markFinished);
    touch("unapplied");
    // The host applies nothing: the request is no longer current or its
    // question index is not ready. Writing it again cannot help before a
    // snapshot changes, so the interval doubles from two minutes.
    const ReadMark mark{"mac", "codex/project/review", "run-1", "conversation-1", "7:100.5", QString(64, 'a')};
    sync.setPending({{mark.key(), mark.toJson()}});
    QTRY_COMPARE(done.size(), 1); QVERIFY(done[0][1].toBool());
    now += ReadMarkSync::kConfirmedResendMs; sync.pump(); QTRY_COMPARE(done.size(), 2);
    now += ReadMarkSync::kConfirmedResendMs; sync.pump(); QTest::qWait(300); QCOMPARE(calls().size(), 2);
    now += ReadMarkSync::kConfirmedResendMs; sync.pump(); QTRY_COMPARE(done.size(), 3);
    now += 4 * ReadMarkSync::kConfirmedResendMs - 1; sync.pump(); QTest::qWait(300); QCOMPARE(calls().size(), 3);
    now += 1; sync.pump(); QTRY_COMPARE(done.size(), 4);
    for (int i = 0; i < 2; ++i) {
        now += ReadMarkSync::kMaxUnappliedResendMs - 1; sync.pump(); QTest::qWait(300); QCOMPARE(calls().size(), 4 + i);
        now += 1; sync.pump(); QTRY_COMPARE(done.size(), 5 + i);
    }
    // Once the host applies it, the usual confirmed interval returns.
    QFile::remove(m_dir.filePath("unapplied"));
    now += ReadMarkSync::kMaxUnappliedResendMs; sync.pump(); QTRY_COMPARE(done.size(), 7);
    now += ReadMarkSync::kConfirmedResendMs - 1; sync.pump(); QTest::qWait(300); QCOMPARE(calls().size(), 7);
    now += 1; sync.pump(); QTRY_COMPARE(done.size(), 8);
}

void TestReadMarkSync::transientlyUnappliedMarkIsRetriedAfterTwoMinutes()
{
    qint64 now = 1000000;
    HgsClient client(FakeCli::program(hgs())); ReadMarkSync sync(&client); sync.setClock([&now] { return now; });
    QSignalSpy done(&sync, &ReadMarkSync::markFinished);
    // The host applies the reply but not the request, for example because its
    // own question index slice ran out; the listing still shows that request.
    touch("reply-only");
    const ReadMark mark{"mac", "codex/project/review", "run-1", "conversation-1", "7:100.5", QString(64, 'a')};
    sync.setPending({{mark.key(), mark.toJson()}});
    QTRY_COMPARE(done.size(), 1); QVERIFY(done[0][1].toBool());
    QCOMPARE(done[0][2].toJsonObject().value("applied").toObject().value("attention_signature").toBool(), false);
    // The first retry comes after two minutes, as for any delivered mark.
    QFile::remove(m_dir.filePath("reply-only"));
    now += ReadMarkSync::kConfirmedResendMs - 1; sync.pump(); QTest::qWait(300); QCOMPARE(calls().size(), 1);
    now += 1; sync.pump(); QTRY_COMPARE(done.size(), 2);
    QCOMPARE(calls().last().value("payload").toObject().value("attention_signature").toString(), QString(64, 'a'));
    QVERIFY(done[1][2].toJsonObject().value("applied").toObject().value("attention_signature").toBool());
    // Applied: the next write is again two minutes later, not a doubled interval.
    now += ReadMarkSync::kConfirmedResendMs - 1; sync.pump(); QTest::qWait(300); QCOMPARE(calls().size(), 2);
    now += 1; sync.pump(); QTRY_COMPARE(done.size(), 3);
}

void TestReadMarkSync::keyedPendingPassesThroughOneWritePerKey()
{
    HgsClient client(FakeCli::program(hgs())); ReadMarkSync sync(&client); QSignalSpy done(&sync, &ReadMarkSync::markFinished);
    const ReadMark one{"mac", "codex/project/a", "run-1", "conversation-1", "7:100.5", {}};
    const ReadMark two{"mac", "codex/project/b", "run-2", "conversation-2", "8:1.5", {}};
    sync.setPending({{one.key(), one.toJson()}, {two.key(), two.toJson()}});
    QTRY_COMPARE(done.size(), 2);
    QTest::qWait(300);
    QCOMPARE(calls().size(), 2);
}

void TestReadMarkSync::duplicateKeysMergeFieldsWhateverTheObjectKeyOrder_data()
{
    QTest::addColumn<QString>("replyKey"); QTest::addColumn<QString>("signatureKey");
    QTest::newRow("reply first") << "a" << "b";
    QTest::newRow("signature first") << "b" << "a";
}

void TestReadMarkSync::duplicateKeysMergeFieldsWhateverTheObjectKeyOrder()
{
    QFETCH(QString, replyKey); QFETCH(QString, signatureKey);
    touch("slow");
    HgsClient client(FakeCli::program(hgs())); ReadMarkSync sync(&client); QSignalSpy done(&sync, &ReadMarkSync::markFinished);
    const ReadMark reply{"mac", "codex/project/review", "run-1", "conversation-1", "7:100.5", {}};
    const ReadMark signature{"mac", "codex/project/review", "run-1", "conversation-1", {}, QString(64, 'a')};
    QJsonObject pending; pending[replyKey] = reply.toJson(); pending[signatureKey] = signature.toJson();
    sync.setPending(pending);
    QCOMPARE(sync.inFlight(), 1);
    QTRY_COMPARE(done.size(), 1);
    QTest::qWait(300);
    QCOMPARE(calls().size(), 1);
    const auto payload = calls().first().value("payload").toObject();
    QCOMPARE(payload.value("reply_id").toString(), QString("7:100.5"));
    QCOMPARE(payload.value("attention_signature").toString(), QString(64, 'a'));
}

QTEST_GUILESS_MAIN(TestReadMarkSync)
#include "test_readmarksync.moc"
