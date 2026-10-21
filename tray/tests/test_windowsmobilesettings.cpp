#include <QtTest>
#include <QTemporaryDir>
#include "FakeCli.h"
#include "WindowsMobileSettings.h"

class TestWindowsMobileSettings : public QObject {
    Q_OBJECT
    static QJsonObject state(bool running=false){
        return {{"configured",true},{"running",running},{"revision",QString(64,'a')},
            {"server_url","https://relay.example.test"},{"state_dir","synthetic/private/receipts"},
            {"generation",running?QJsonValue(QString(32,'b')):QJsonValue(QJsonValue::Null)},
            {"startup_at_login",false}};
    }
    static bool reply(const QString &path,const QJsonObject &value){
        QFile file(path);return file.open(QIODevice::WriteOnly|QIODevice::Truncate)
            &&file.write(QJsonDocument(value).toJson())>0;
    }
    static bool install(const QString &fixture){return FakeCli::install(fixture,R"PY(#!/usr/bin/env python3
import json, pathlib, sys
root=pathlib.Path(__file__).parent
payload=json.load(sys.stdin)
with (root/'calls').open('a') as out: out.write(json.dumps({'argv':sys.argv[1:],'payload':payload})+'\n')
sys.stdout.write((root/'reply').read_text())
)PY");}
private slots:
    void malformedResults_data(){
        QTest::addColumn<QString>("action");QTest::addColumn<QString>("defect");
        for(const auto &action:{"status","save","start","stop"})
            for(const auto &defect:{"empty","partial","configured-type","action-state","running-type","revision-type","missing-revision","generation-type","startup-type","server-type","state-type","ok-type","identity","stopping-type"})
                QTest::newRow(qPrintable(QString(action)+"/"+defect))<<QString(action)<<QString(defect);
    }
    void malformedResults(){
        QFETCH(QString,action);QFETCH(QString,defect);
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto fixture=dir.path()+"/connector";
        QVERIFY(install(fixture));const auto initial=state(action=="stop");QVERIFY(reply(dir.path()+"/reply",initial));
        WindowsMobileSettings::Panel panel(FakeCli::program(fixture),dir.path()+"/private/connector.json");
        auto *reload=panel.findChild<QPushButton*>("reloadMobileConnector");
        auto *token=panel.findChild<QLineEdit*>("relayReplacementToken");
        auto *save=panel.findChild<QPushButton*>("saveRelayConfiguration");
        auto *start=panel.findChild<QPushButton*>("startMobileConnector");
        auto *stop=panel.findChild<QPushButton*>("stopMobileConnector");
        QTRY_VERIFY(reload->isEnabled());token->setText("synthetic-replacement-credential");
        auto bad=state(action=="start"||action=="stop");
        if(defect=="empty")bad={};
        else if(defect=="partial")bad={{"configured",true}};
        else if(defect=="configured-type")bad["configured"]="true";
        else if(defect=="action-state"){
            if(action=="status"){bad["configured"]=false;bad["running"]=true;}
            else bad["running"]=action!="start";
        }
        else if(defect=="running-type")bad["running"]="false";
        else if(defect=="revision-type")bad["revision"]=false;
        else if(defect=="missing-revision")bad.remove("revision");
        else if(defect=="generation-type")bad["generation"]=12;
        else if(defect=="startup-type")bad["startup_at_login"]="false";
        else if(defect=="server-type")bad["server_url"]=false;
        else if(defect=="state-type")bad["state_dir"]=42;
        else if(defect=="ok-type")bad["ok"]=1;
        else if(defect=="stopping-type")bad["stopping"]="true";
        else if(defect=="identity"){
            if(action=="status")bad["revision"]="not-a-revision";
            else if(action=="save")bad["server_url"]="https://other.example.test";
            else if(action=="start")bad["revision"]=QString(64,'c');
            else bad={{"stopping",true},{"generation",QString(32,'c')}};
        }
        QVERIFY(reply(dir.path()+"/reply",bad));
        (action=="status"?reload:action=="save"?save:action=="start"?start:stop)->click();
        QTRY_VERIFY(reload->isEnabled());
        QCOMPARE(panel.findChild<QLabel*>("connectorServiceStatus")->text(),QString("Connector: status unknown"));
        QVERIFY(!save->isEnabled());QVERIFY(!start->isEnabled());QVERIFY(!stop->isEnabled());
        QCOMPARE(token->text(),QString("synthetic-replacement-credential"));
        // An invalid explicit reload cannot unlock the panel either.
        QVERIFY(reply(dir.path()+"/reply",{}));
        reload->click();QTRY_VERIFY(reload->isEnabled());QVERIFY(!save->isEnabled());
        QVERIFY(reply(dir.path()+"/reply",state()));reload->click();QTRY_VERIFY(save->isEnabled());
    }
    void realResponseFormsAndPrivateInput(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto fixture=dir.path()+"/connector";
        QVERIFY(install(fixture));QVERIFY(reply(dir.path()+"/reply",{{"configured",false},{"running",false},{"revision",""},{"startup_at_login",false}}));
        WindowsMobileSettings::Panel panel(FakeCli::program(fixture),dir.path()+"/private/connector.json");
        auto button=[&](const char *name){return panel.findChild<QPushButton*>(name);};
        auto *save=button("saveRelayConfiguration"),*start=button("startMobileConnector"),*stop=button("stopMobileConnector"),*reload=button("reloadMobileConnector");
        auto *token=panel.findChild<QLineEdit*>("relayReplacementToken");
        QTRY_VERIFY(save->isEnabled());QVERIFY(!start->isEnabled());
        panel.findChild<QLineEdit*>("relayUrl")->setText("https://relay.example.test");
        token->setText("synthetic-computer-credential");QVERIFY(reply(dir.path()+"/reply",state()));save->click();
        QTRY_VERIFY(start->isEnabled());QVERIFY(token->text().isEmpty());
        QFile calls(dir.path()+"/calls");QVERIFY(calls.open(QIODevice::ReadOnly));
        const auto lines=calls.readAll().trimmed().split('\n');QCOMPARE(lines.size(),2);
        const auto sent=QJsonDocument::fromJson(lines.last()).object();
        QVERIFY(!sent.value("argv").toArray().contains("synthetic-computer-credential"));
        QCOMPARE(sent.value("payload").toObject().value("node_token").toString(),"synthetic-computer-credential");
        QCOMPARE(sent.value("payload").toObject().value("revision").toString(),QString());
        QVERIFY(reply(dir.path()+"/reply",state(true)));start->click();QTRY_VERIFY(stop->isEnabled());
        QVERIFY(reply(dir.path()+"/reply",{{"stopping",true},{"generation",QString(32,'b')}}));stop->click();
        QTRY_VERIFY(reload->isEnabled());QVERIFY(!save->isEnabled());QVERIFY(!stop->isEnabled());
        QVERIFY(reply(dir.path()+"/reply",state()));reload->click();QTRY_VERIFY(save->isEnabled());
        // A status-shaped stopped response is the idempotent stop form.
        QVERIFY(reply(dir.path()+"/reply",state(true)));start->click();QTRY_VERIFY(stop->isEnabled());
        QVERIFY(reply(dir.path()+"/reply",state()));stop->click();QTRY_VERIFY(save->isEnabled());
    }
    void configuredDefaultStateDirectoryRemainsSupported_data(){
        QTest::addColumn<QJsonValue>("defaultDirectory");
        QTest::newRow("null")<<QJsonValue(QJsonValue::Null);
        QTest::newRow("empty")<<QJsonValue(QString());
    }
    void configuredDefaultStateDirectoryRemainsSupported(){
        QFETCH(QJsonValue,defaultDirectory);
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto fixture=dir.path()+"/connector";
        QVERIFY(install(fixture));auto status=state();status["state_dir"]=defaultDirectory;
        QVERIFY(reply(dir.path()+"/reply",status));
        WindowsMobileSettings::Panel panel(FakeCli::program(fixture),dir.path()+"/private/connector.json");
        auto *start=panel.findChild<QPushButton*>("startMobileConnector");
        auto *save=panel.findChild<QPushButton*>("saveRelayConfiguration");
        auto *stop=panel.findChild<QPushButton*>("stopMobileConnector");
        auto *reload=panel.findChild<QPushButton*>("reloadMobileConnector");
        auto *token=panel.findChild<QLineEdit*>("relayReplacementToken");
        QTRY_VERIFY(start->isEnabled());
        // Saving an unchanged configuration keeps its documented default form.
        save->click();QTRY_VERIFY(start->isEnabled());
        status["running"]=true;status["generation"]=QString(32,'b');
        QVERIFY(reply(dir.path()+"/reply",status));start->click();
        QTRY_VERIFY(stop->isEnabled());
        status["running"]=false;status["generation"]=QJsonValue(QJsonValue::Null);
        QVERIFY(reply(dir.path()+"/reply",status));stop->click();QTRY_VERIFY(save->isEnabled());
        // Accepting a default must not weaken malformed-state refusal or lose
        // the user's replacement credential on an unconfirmed reload.
        token->setText("synthetic-replacement-credential");
        auto invalid=status;invalid["running"]="false";
        QVERIFY(reply(dir.path()+"/reply",invalid));reload->click();
        QTRY_VERIFY(reload->isEnabled());
        QVERIFY(!save->isEnabled());QVERIFY(!start->isEnabled());QVERIFY(!stop->isEnabled());
        QCOMPARE(token->text(),QString("synthetic-replacement-credential"));
        QVERIFY(reply(dir.path()+"/reply",status));reload->click();QTRY_VERIFY(start->isEnabled());
    }
};
QTEST_MAIN(TestWindowsMobileSettings)
#include "test_windowsmobilesettings.moc"
