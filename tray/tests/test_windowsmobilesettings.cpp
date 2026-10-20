#include <QtTest>
#include <QTemporaryDir>
#include "FakeCli.h"
#include "WindowsMobileSettings.h"

class TestWindowsMobileSettings : public QObject {
    Q_OBJECT
private slots:
    void privateInputAndUncertainActionsRequireStatusReload(){
        QTemporaryDir dir;QVERIFY(dir.isValid());
        const auto fixture=dir.path()+"/connector";
        QVERIFY(FakeCli::install(fixture,R"PY(#!/usr/bin/env python3
import json, pathlib, sys
root=pathlib.Path(__file__).parent
payload=json.load(sys.stdin)
action=sys.argv[2]
with (root/'calls').open('a') as out: out.write(json.dumps({'argv':sys.argv[1:],'payload':payload})+'\n')
if action=='start':
    print('{"ok":false}');sys.exit(1)
print(json.dumps({'configured':True,'running':False,'revision':'saved-revision','server_url':'https://relay.example.test'}))
)PY"));
        WindowsMobileSettings::Panel panel(FakeCli::program(fixture),dir.path()+"/private/connector.json");
        auto *start=panel.findChild<QPushButton*>("startMobileConnector");
        auto *save=panel.findChild<QPushButton*>("saveRelayConfiguration");
        auto *reload=panel.findChild<QPushButton*>("reloadMobileConnector");
        auto *token=panel.findChild<QLineEdit*>("relayReplacementToken");
        QTRY_VERIFY(start->isEnabled());
        token->setText("synthetic-computer-credential");save->click();
        QTRY_VERIFY(token->text().isEmpty());
        QFile calls(dir.path()+"/calls");QVERIFY(calls.open(QIODevice::ReadOnly));
        const auto lines=calls.readAll().trimmed().split('\n');QCOMPARE(lines.size(),2);
        const auto sent=QJsonDocument::fromJson(lines.last()).object();
        QVERIFY(!sent.value("argv").toArray().contains("synthetic-computer-credential"));
        QCOMPARE(sent.value("payload").toObject().value("node_token").toString(),"synthetic-computer-credential");
        QCOMPARE(sent.value("payload").toObject().value("revision").toString(),"saved-revision");
        start->click();QTRY_VERIFY(reload->isEnabled());
        QVERIFY(!start->isEnabled());QVERIFY(!save->isEnabled());
        reload->click();QTRY_VERIFY(start->isEnabled());QVERIFY(save->isEnabled());
    }
};
QTEST_MAIN(TestWindowsMobileSettings)
#include "test_windowsmobilesettings.moc"
