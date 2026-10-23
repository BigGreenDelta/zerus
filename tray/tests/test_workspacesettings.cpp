#include <QtTest>
#include <QTemporaryDir>
#include "FakeCli.h"
#include "WorkspaceSettings.h"

class TestWorkspaceSettings : public QObject {
    Q_OBJECT
    static QJsonObject active(bool running=false,const QString &revision=QString(64,'a')){
        return {{"state","active"},{"encrypted",true},{"config_revision",revision},{"server_url","https://relay.example.test"},{"workspace_id","workspace-fixture"},{"node_id","computer-fixture"},{"workspace_name","Synthetic workspace"},{"computer_name","Synthetic computer"},{"connector_state",running?"running":"stopped"},{"running",running},{"generation",running?QJsonValue(QString(32,'b')):QJsonValue(QJsonValue::Null)}};
    }
    static QJsonObject unconfigured(){return {{"state","unconfigured"},{"encrypted",false},{"config_revision",""}};}
    static bool reply(const QString &directory,const QJsonObject &responses){return FakeCli::replace(directory+"/responses",QJsonDocument(responses).toJson());}
    static bool install(const QString &script){return FakeCli::install(script,R"PY(#!/usr/bin/env python3
import json,pathlib,sys
root=pathlib.Path(__file__).parent
payload=json.load(sys.stdin)
action=sys.argv[2]
with (root/'calls').open('a') as out:out.write(json.dumps({'argv':sys.argv[1:],'payload':payload})+'\n')
responses=json.loads((root/'responses').read_text())
result=responses.get(action,responses.get('status',{}))
print(json.dumps(result))
sys.exit(1 if result.get('ok') is False else 0)
)PY");}
    static QList<QJsonObject> calls(const QString &directory){
        QFile file(directory+"/calls");if(!file.open(QIODevice::ReadOnly))return {};
        QList<QJsonObject> result;for(const auto &line:file.readAll().split('\n'))if(!line.isEmpty())result.append(QJsonDocument::fromJson(line).object());return result;
    }
private slots:
    void openingIsReadOnlyAndJoinSecretsUseOnlyStdin(){
        QTemporaryDir directory;QVERIFY(directory.isValid());const auto fixture=directory.path()+"/connector";QVERIFY(install(fixture));
        QVERIFY(reply(directory.path(),{{"status",unconfigured()}}));
        WorkspaceSettings::Panel panel(FakeCli::program(fixture),directory.path()+"/private/connector.json");
        auto *submit=panel.findChild<QPushButton*>("submitWorkspaceEnrollment");QTRY_VERIFY(submit->isEnabled());
        QCOMPARE(calls(directory.path()).size(),1);QCOMPARE(calls(directory.path()).first().value("argv").toArray().at(1).toString(),QString("status"));
        QVERIFY(!QFileInfo::exists(directory.path()+"/private"));
        panel.findChild<QLineEdit*>("workspaceRelayUrl")->setText("https://relay.example.test");
        panel.findChild<QComboBox*>("workspaceEnrollmentMode")->setCurrentIndex(1);
        panel.findChild<QLineEdit*>("joinComputerWorkspaceCode")->setText("ze1.synthetic-secret-code");
        auto waiting=unconfigured();waiting.insert("pending",QJsonObject{{"action","join"},{"state","pending"}});
        QVERIFY(reply(directory.path(),{{"status",waiting},{"join",QJsonObject{{"state","pending"},{"node_id","candidate-fixture"},{"config_revision",""}}}}));
        submit->click();auto *check=panel.findChild<QPushButton*>("checkWorkspaceEnrollment");QTRY_VERIFY(check->isEnabled());
        const auto log=calls(directory.path());QVERIFY(log.size()>=3);const auto sent=log.at(1);QCOMPARE(sent.value("argv").toArray().at(1).toString(),QString("join"));
        QVERIFY(!QJsonDocument(sent.value("argv").toArray()).toJson().contains("synthetic-secret-code"));
        QCOMPARE(sent.value("payload").toObject().value("code").toString(),QString("ze1.synthetic-secret-code"));
        QCOMPARE(sent.value("payload").toObject().value("expected_revision").toString(),QString());
        QVERIFY(reply(directory.path(),{{"status",active()},{"check",active()}}));check->click();QTRY_VERIFY(panel.findChild<QPushButton*>("giveComputerWorkspaceCode")->isEnabled());
        QVERIFY(panel.findChild<QLineEdit*>("joinComputerWorkspaceCode")->text().isEmpty());
    }
    void computerInvitationRequiresExplicitCopyAndCanBeCanceled(){
        QTemporaryDir directory;const auto fixture=directory.path()+"/connector";QVERIFY(install(fixture));
        const auto code=QStringLiteral("ze1.synthetic-computer-invitation");
        QVERIFY(reply(directory.path(),{{"status",active()},{"invite-computer",QJsonObject{{"code",code},{"server_url","https://relay.example.test"},{"purpose","computer"},{"invitation_id","invitation-fixture"},{"expires_at",double(QDateTime::currentSecsSinceEpoch()+600)}}},{"cancel-invite",QJsonObject{{"cancelled",true}}}}));
        WorkspaceSettings::Panel panel(FakeCli::program(fixture),directory.path()+"/connector.json");auto *give=panel.findChild<QPushButton*>("giveComputerWorkspaceCode");QTRY_VERIFY(give->isEnabled());
        QApplication::clipboard()->setText("synthetic clipboard");give->click();auto *copy=panel.findChild<QPushButton*>("copyComputerWorkspaceCode");QTRY_VERIFY(copy->isEnabled());QCOMPARE(QApplication::clipboard()->text(),QString("synthetic clipboard"));
        copy->click();QCOMPARE(QApplication::clipboard()->text(),code);
        panel.findChild<QPushButton*>("cancelComputerWorkspaceCode")->click();QTRY_VERIFY(!copy->isEnabled());
        const auto log=calls(directory.path());for(const auto &call:log)QVERIFY(!QJsonDocument(call).toJson().contains(code.toUtf8()));
    }
    void malformedStatusCannotUnlockMutations(){
        QTemporaryDir directory;const auto fixture=directory.path()+"/connector";QVERIFY(install(fixture));QVERIFY(reply(directory.path(),{{"status",QJsonObject{}}}));
        WorkspaceSettings::Panel panel(FakeCli::program(fixture),directory.path()+"/connector.json");auto *reload=panel.findChild<QPushButton*>("refreshWorkspaceStatus");auto *submit=panel.findChild<QPushButton*>("submitWorkspaceEnrollment");
        QTRY_VERIFY(reload->isEnabled());QVERIFY(!submit->isEnabled());
        auto malformed=active();malformed.insert("encrypted","true");QVERIFY(reply(directory.path(),{{"status",malformed}}));reload->click();QTRY_VERIFY(reload->isEnabled());QVERIFY(!submit->isEnabled());
        QVERIFY(reply(directory.path(),{{"status",unconfigured()}}));reload->click();QTRY_VERIFY(submit->isEnabled());
    }
    void switchConfirmationCancelsWithoutMutationAndStopPrecedesChange(){
        QTemporaryDir directory;const auto fixture=directory.path()+"/connector";QVERIFY(install(fixture));QVERIFY(reply(directory.path(),{{"status",active(true)}}));
        WorkspaceSettings::Panel panel(FakeCli::program(fixture),directory.path()+"/connector.json");auto *change=panel.findChild<QPushButton*>("changeMobileWorkspace");QTRY_VERIFY(change->isEnabled());change->click();
        auto *submit=panel.findChild<QPushButton*>("submitWorkspaceEnrollment");submit->click();auto *dialog=panel.findChild<QMessageBox*>("confirmWorkspaceChange");QVERIFY(dialog);dialog->button(QMessageBox::Cancel)->click();QCoreApplication::processEvents();QCOMPARE(calls(directory.path()).size(),1);
        const auto changed=active(false,QString(64,'c'));
        QVERIFY(reply(directory.path(),{{"status",changed},{"stop",QJsonObject{{"state","stopped"},{"running",false},{"generation",QJsonValue::Null}}},{"switch-create",changed}}));
        submit->click();dialog=panel.findChild<QMessageBox*>("confirmWorkspaceChange");QVERIFY(dialog);dialog->button(QMessageBox::Yes)->click();
        QTRY_VERIFY(calls(directory.path()).size()>=4);const auto log=calls(directory.path());QCOMPARE(log.at(1).value("argv").toArray().at(1).toString(),QString("stop"));QCOMPARE(log.at(2).value("argv").toArray().at(1).toString(),QString("switch-create"));
        QCOMPARE(log.at(2).value("payload").toObject().value("expected_revision").toString(),QString(64,'a'));
    }
    void unconfirmedLegacyDisconnectRequiresSeparateConfirmation(){
        QTemporaryDir directory;const auto fixture=directory.path()+"/connector";QVERIFY(install(fixture));
        const QJsonObject legacy{{"state","legacy"},{"encrypted",false},{"config_revision",QString(64,'a')},{"pending",QJsonObject{{"action","switch-create"},{"state","legacy-retirement-unconfirmed"}}}};
        QVERIFY(reply(directory.path(),{{"status",legacy},{"check",active()}}));
        WorkspaceSettings::Panel panel(FakeCli::program(fixture),directory.path()+"/connector.json");auto *check=panel.findChild<QPushButton*>("checkWorkspaceEnrollment");QTRY_VERIFY(check->isEnabled());
        check->click();auto *dialog=panel.findChild<QMessageBox*>("confirmLegacyLocalDisconnect");QVERIFY(dialog);QVERIFY(dialog->text().contains("may remain active"));dialog->button(QMessageBox::Cancel)->click();QCoreApplication::processEvents();QCOMPARE(calls(directory.path()).size(),1);
        check->click();dialog=panel.findChild<QMessageBox*>("confirmLegacyLocalDisconnect");QVERIFY(dialog);dialog->button(QMessageBox::Yes)->click();QTRY_VERIFY(calls(directory.path()).size()>=3);
        const auto sent=calls(directory.path()).at(1);QCOMPARE(sent.value("argv").toArray().at(1).toString(),QString("check"));QCOMPARE(sent.value("payload").toObject().value("allow_unconfirmed_legacy_retirement"),QJsonValue(true));
        QCOMPARE(sent.value("payload").toObject().value("expected_revision").toString(),QString(64,'a'));
    }
    void removedComputerCanChooseNewWorkspaceButCannotStartOldConnector(){
        QTemporaryDir directory;const auto fixture=directory.path()+"/connector";QVERIFY(install(fixture));auto retired=active();retired.insert("state","retired");QVERIFY(reply(directory.path(),{{"status",retired}}));
        WorkspaceSettings::Panel panel(FakeCli::program(fixture),directory.path()+"/connector.json");auto *change=panel.findChild<QPushButton*>("changeMobileWorkspace");QTRY_VERIFY(change->isEnabled());
        QVERIFY(panel.findChild<QPushButton*>("startWorkspaceConnector")->isHidden());QVERIFY(panel.findChild<QLabel*>("workspaceSummary")->text().contains("removed"));change->click();QVERIFY(panel.findChild<QPushButton*>("submitWorkspaceEnrollment")->isEnabled());
        QCOMPARE(panel.findChild<QLineEdit*>("workspaceRelayUrl")->text(),QString("https://relay.example.test"));
        auto draining=active(true);draining.insert("state","retired");QVERIFY(reply(directory.path(),{{"status",draining}}));panel.findChild<QPushButton*>("refreshWorkspaceStatus")->click();auto *stop=panel.findChild<QPushButton*>("stopWorkspaceConnector");QTRY_VERIFY(stop->isVisibleTo(&panel)&&stop->isEnabled());QVERIFY(panel.findChild<QPushButton*>("startWorkspaceConnector")->isHidden());
        QVERIFY(reply(directory.path(),{{"status",retired},{"stop",QJsonObject{{"state","stopped"},{"running",false},{"generation",QJsonValue(QJsonValue::Null)}}}}));stop->click();QTRY_VERIFY(calls(directory.path()).size()>=4);const auto sent=calls(directory.path()).at(2);QCOMPARE(sent.value("argv").toArray().at(1).toString(),QString("stop"));QCOMPARE(sent.value("payload").toObject().value("generation").toString(),QString(32,'b'));
    }
};
QTEST_MAIN(TestWorkspaceSettings)
#include "test_workspacesettings.moc"
