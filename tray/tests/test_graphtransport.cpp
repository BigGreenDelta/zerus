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
