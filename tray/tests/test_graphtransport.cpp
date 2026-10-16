#include "FakeCli.h"
#include "HgsClient.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

class TestGraphTransport : public QObject {
    Q_OBJECT
    QTemporaryDir root;
    QString program;
  private slots:
    void initTestCase() {
        QVERIFY(root.isValid());
        program = root.filePath("graph-cli.py");
        QFile file(program);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"PY(#!/usr/bin/env python3
import json,sys
from pathlib import Path
with open(__file__+'.calls','a') as log:
    log.write(json.dumps(sys.argv[1:])+'\n')
if '@old' in sys.argv:
    print('unknown option --json (agent flags go after --)',file=sys.stderr)
    sys.exit(1)
if '@oversized' in sys.argv:
    print(json.dumps({'data':'x'*(1024*1024)}))
elif '@invalid' in sys.argv:
    print('[]')
else:
    print(json.dumps({'schema':1,'fixture':True}))
)PY");
        file.close();
        QVERIFY(FakeCli::makeRunnable(program));
    }
    void explicitTargetReadsValidateEveryIdentityAndClearNativeSenderLabels() {
        const auto fixture = root.filePath("target.py");
        QFile f(fixture);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"PY(#!/usr/bin/env python3
import json,os,sys
args=sys.argv[1:]
host=args.pop(0) if args and args[0].startswith('@') else ''
body=json.load(sys.stdin) if args[0]=='graph' else None
with open(__file__+'.calls','a') as log:
 log.write(json.dumps({'args':args,'input':body,'sender':os.environ.get('HGS_SESSION'),'run':os.environ.get('HGS_RUN_ID')})+'\n')
reply={'schema':1,'machine_id':'11111111-1111-4111-8111-111111111111','name':'claude/example/work','run_id':'run-one','conversation_id':'native-one','archive_id':'','agent_id':''}
if body:
 reply.update(message_request_id=body['message_request_id'],event={'source':'hgs_delivery','message_id':body['message_request_id'],'detail':'Exact public receipt'})
if host=='@wrong-machine':reply['machine_id']='22222222-2222-4222-8222-222222222222'
if host=='@wrong-run':reply['run_id']='reused-run'
if host=='@wrong-message':reply['event']['message_id']='another-message'
print(json.dumps(reply))
)PY");
        f.close();
        QVERIFY(FakeCli::makeRunnable(fixture));
        const auto oldSession = qgetenv("HGS_SESSION"), oldRun = qgetenv("HGS_RUN_ID");
        qputenv("HGS_SESSION", "native-source-label");
        qputenv("HGS_RUN_ID", "native-source-run");
        HgsClient client(FakeCli::program(fixture));
        QSignalSpy ready(&client, &HgsClient::graphTargetReady),
            failed(&client, &HgsClient::graphTargetFailed);
        const QString machine = "11111111-1111-4111-8111-111111111111",
                      message = "33333333-3333-4333-8333-333333333333";
        client.requestGraphTarget({}, "claude/example/work", "run-one", "native-one", {}, {},
                                  machine);
        QTRY_COMPARE(ready.size(), 1);
        client.requestGraphTarget({}, "claude/example/work", "run-one", "native-one", {}, {},
                                  machine, message);
        QTRY_COMPARE(ready.size(), 2);
        for (const auto &host : {"wrong-machine", "wrong-run", "wrong-message"})
            client.requestGraphTarget(host, "claude/example/work", "run-one", "native-one", {}, {},
                                      machine, message);
        QTRY_COMPARE(failed.size(), 3);
        QCOMPARE(ready.size(), 2);
        if (oldSession.isNull())
            qunsetenv("HGS_SESSION");
        else
            qputenv("HGS_SESSION", oldSession);
        if (oldRun.isNull())
            qunsetenv("HGS_RUN_ID");
        else
            qputenv("HGS_RUN_ID", oldRun);
        QFile calls(fixture + ".calls");
        QVERIFY(calls.open(QIODevice::ReadOnly));
        const auto rows = calls.readAll().trimmed().split('\n');
        QCOMPARE(rows.size(), 5);
        for (const auto &row : rows) {
            const auto v = QJsonDocument::fromJson(row).object();
            QVERIFY(v["sender"].isNull());
            QVERIFY(v["run"].isNull());
            const auto input = v["input"].toObject();
            if (!input.isEmpty()) {
                QCOMPARE(input["expected_run_id"].toString(), QString("run-one"));
                QCOMPARE(input["message_request_id"].toString(), message);
            }
        }
    }
    void mandatoryReadOnlyShapeAndOpaqueCursorArePreserved() {
        HgsClient client(FakeCli::program(program));
        QSignalSpy ready(&client, &HgsClient::graphReady), failed(&client, &HgsClient::graphFailed);
        const auto first = client.requestGraph("fixture-mac", false);
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready[0][0].toULongLong(), first);
        QCOMPARE(ready[0][1].toString(), QString("fixture-mac"));
        QFile calls(program + ".calls");
        QVERIFY(calls.open(QIODevice::ReadOnly));
        const auto lines = calls.readAll().trimmed().split('\n');
        QCOMPARE(QJsonDocument::fromJson(lines.last()).array(),
                 QJsonArray({"@fixture-mac", "graph", "snapshot", "--json", "--limit", "200"}));
        calls.close();
        const auto second = client.requestGraph({}, true, "opaque-cursor");
        QTRY_COMPARE(ready.size(), 2);
        QCOMPARE(ready[1][0].toULongLong(), second);
        QCOMPARE(failed.size(), 0);
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(calls.readAll().trimmed().split('\n').last()).array(),
                 QJsonArray(
                     {"graph", "changes", "--json", "--limit", "200", "--after", "opaque-cursor"}));
    }
    void oldInvalidAndOversizedHostsReportFailureWithoutRetry() {
        HgsClient client(FakeCli::program(program));
        QSignalSpy ready(&client, &HgsClient::graphReady), failed(&client, &HgsClient::graphFailed);
        for (const auto &host : {"old", "invalid", "oversized"}) {
            const int before = failed.size();
            const auto id = client.requestGraph(host, false);
            QTRY_COMPARE(failed.size(), before + 1);
            QCOMPARE(failed.last()[0].toULongLong(), id);
            QCOMPARE(failed.last()[1].toString(), QString(host));
        }
        QCOMPARE(ready.size(), 0);
        QFile calls(program + ".calls");
        QVERIFY(calls.open(QIODevice::ReadOnly));
        const auto lines = calls.readAll().trimmed().split('\n');
        for (const auto &host : {"old", "invalid", "oversized"}) {
            int count = 0;
            for (const auto &line : lines)
                if (QJsonDocument::fromJson(line).array().contains('@' + QString(host)))
                    ++count;
            QCOMPARE(count, 1);
        }
        const auto invalid = client.requestGraph({}, false, QString(2049, 'x'));
        QTRY_COMPARE(failed.size(), 4);
        QCOMPARE(failed.last()[0].toULongLong(), invalid);
    }
};
QTEST_GUILESS_MAIN(TestGraphTransport)
#include "test_graphtransport.moc"
