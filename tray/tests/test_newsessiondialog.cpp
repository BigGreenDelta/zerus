#include <QtTest>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QScrollBar>
#include "NewSessionDialog.h"
#include "AccountUsageStore.h"
#include "SessionsWindow.h"
#include "FakeCli.h"

class TestNewSessionDialog : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings,m_fixture;
    QString program() const {return FakeCli::program(m_fixture.filePath("hgs"));}
    void flag(const QString &name,bool enabled=true) {
        QFile f(m_fixture.filePath(name));if(enabled){QVERIFY(f.open(QIODevice::WriteOnly));}else f.remove();
    }
    static FleetState fleet() {
        FleetState fleet;BoxState local;local.host="pc";local.ok=true;
        fleet.setLocal(local,QDateTime::currentMSecsSinceEpoch());BoxState mac;mac.host="mac";mac.ok=true;
        fleet.setPeer(mac,QDateTime::currentMSecsSinceEpoch());return fleet;
    }
    static QListWidget *choices(NewSessionDialog &dialog) {return dialog.findChild<QListWidget *>("launchAccount");}
    static int row(NewSessionDialog &dialog,const QString &id) {
        const auto *list=choices(dialog);for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==id)return i;return -1;
    }
private slots:
    void initTestCase() {
        QVERIFY(m_settings.isValid());QCoreApplication::setOrganizationName("hgs-tests");QCoreApplication::setApplicationName("new-session-dialog");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,m_settings.path());
    }
    void init() {
        QSettings().clear();
        for(const auto &name:{"fail","removed","slow-mac","released","missing","ended"})flag(name,false);
        QVERIFY(FakeCli::install(m_fixture.filePath("hgs"),R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
a=sys.argv[1:];host=''
if a and a[0].startswith('@'):host=a.pop(0)[1:]
if a[:2]==['account','ls']:
 if (root/'fail').exists():sys.exit(1)
 profiles=[{'id':i,'provider':p,'label':label,'home':'/profiles/'+i,'installed':True,'is_default':i=='other'}
           for i,p,label in [('work','codex','Work'),('other','codex','Work'),('claude','claude','Review'),('native-dsh','dsh','DeepSeek')]
           if not ((root/'removed').exists() and i=='work')]
 print(json.dumps({'profiles':profiles,'removed_profiles':[{'id':'native-kimi','provider':'kimi'}]}))
elif a[:2]==['account','inspect']:
 i=a[2];p='claude' if i=='claude' else 'dsh' if i=='native-dsh' else 'codex'
 if host=='mac' and i=='work' and (root/'slow-mac').exists():
  (root/'mac-started').touch()
  for _ in range(500):
   if (root/'released').exists():break
   time.sleep(.02)
 if (root/'fail').exists():sys.exit(1)
 window={'window_minutes':300,'used_percent':91 if host=='mac' else 17,'resets_at':int(time.time())+3600}
 if (root/'missing').exists():window.pop('used_percent')
 if (root/'ended').exists():window['resets_at']=int(time.time())-60
 print(json.dumps({'id':i,'provider':p,'home':'/profiles/'+i,'status':'signed_out' if p=='dsh' else 'ok',
                   'windows':[] if p=='dsh' else [window,{'window_minutes':10080,'used_percent':78,'resets_at':int(time.time())+86400}]}))
elif a and a[0]=='dirs':print(json.dumps({'path':a[-1],'directories':[]}))
elif a and a[0]=='worktrees':print(json.dumps({'state':'not_repo','worktrees':[]}))
)PY"));
    }
    void pairsAndLimitsLaunchExactProfile_data() {
        QTest::addColumn<QString>("host");QTest::addColumn<bool>("dark");
        QTest::newRow("pc-light")<<QString()<<false;QTest::newRow("pc-dark")<<QString()<<true;
        QTest::newRow("mac-light")<<QString("mac")<<false;QTest::newRow("mac-dark")<<QString("mac")<<true;
    }
    void pairsAndLimitsLaunchExactProfile() {
        QFETCH(QString,host);QFETCH(bool,dark);
        QSettings().setValue("workspace/theme",dark?"dark":"light");
        SessionsWindow parent(program());
        NewSessionDialog dialog(program(),fleet(),host,"codex",{},&parent);
        dialog.selectPath("/workspace/example",false);dialog.show();
        auto *list=choices(dialog);QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(list->count(),5);QVERIFY(!dialog.findChild<QComboBox *>("launchAgent"));
        QTRY_VERIFY(list->item(row(dialog,"work"))->toolTip().contains(host.isEmpty()?"17% used":"91% used"));
        const auto tooltip=list->item(row(dialog,"work"))->toolTip();
        QVERIFY(tooltip.contains("5h"));QVERIFY(tooltip.contains("7d"));QVERIFY(tooltip.contains("Reset in"));
        QVERIFY(list->item(row(dialog,"work"))->text().contains("work"));QVERIFY(list->item(row(dialog,"other"))->text().contains("other"));
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),QString("other"));
        QTRY_VERIFY(list->item(row(dialog,"native-dsh"))->toolTip().contains("signed out"));
        QCOMPARE(list->item(list->count()-1)->text(),QString("Run shell"));
        list->setCurrentRow(row(dialog,"work"));
        const auto preview=qEnvironmentVariable("ZERUS_LAUNCH_PREVIEW");
        if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(dialog.grab().save(preview+"/launch-"+(host.isEmpty()?"pc":"mac")+(dark?"-dark":"-light")+".png"));}
        auto *start=dialog.findChild<QPushButton *>("primary");QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);
        QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][0].toString(),host);QCOMPARE(launched[0][1].toString(),QString("codex"));QCOMPARE(launched[0][4].toString(),QString("work"));
        NewSessionDialog next(program(),fleet(),host);QTRY_VERIFY(row(next,"work")>=0);QTRY_COMPARE(choices(next)->count(),5);
        QCOMPARE(choices(next)->currentItem()->data(Qt::UserRole).toString(),QString("work"));
    }
    void rememberedAndExplicitMissingAccounts() {
        QSettings().setValue("workspace/launchAccounts",QByteArray(R"({"":{"codex":"gone"}})"));
        NewSessionDialog dialog(program(),fleet(),{},"codex");dialog.selectPath("/workspace/example",false);dialog.show();
        QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(choices(dialog)->count(),5);QCOMPARE(choices(dialog)->currentItem()->data(Qt::UserRole).toString(),QString("other"));
        dialog.selectAccount("gone");QVERIFY(!choices(dialog)->currentItem());QVERIFY(!dialog.findChild<QPushButton *>("primary")->isEnabled());
        QVERIFY(dialog.findChild<QLabel *>("launchError")->text().contains("unavailable"));
        choices(dialog)->setCurrentRow(row(dialog,"work"));flag("removed");dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTRY_COMPARE(row(dialog,"work"),-1);QVERIFY(!choices(dialog)->currentItem());
        choices(dialog)->setCurrentRow(choices(dialog)->count()-1);
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);auto *start=dialog.findChild<QPushButton *>("primary");
        QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][1].toString(),QString("sh"));QVERIFY(launched[0][4].toString().isEmpty());
        NewSessionDialog next(program(),fleet());QTRY_COMPARE(choices(next)->count(),4);
        QCOMPARE(choices(next)->currentItem()->data(Qt::UserRole+1).toString(),QString("sh"));
    }
    void refreshKeepsSelectionAndLastReportedLimits() {
        NewSessionDialog dialog(program(),fleet(),{},"codex");dialog.show();auto *list=choices(dialog);
        QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(list->count(),5);QTRY_VERIFY(list->item(row(dialog,"work"))->toolTip().contains("17% used"));
        list->setCurrentRow(row(dialog,"work"));QPersistentModelIndex index=list->currentIndex();
        dialog.setFleet(fleet());QCOMPARE(list->currentIndex(),QModelIndex(index));
        const auto scroll=list->verticalScrollBar()->value();flag("fail");dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTRY_VERIFY(list->currentItem()->toolTip().contains("Update failed (last reported limits)"));
        QVERIFY(list->currentItem()->toolTip().contains("17% used"));QCOMPARE(list->currentIndex(),QModelIndex(index));
        QCOMPARE(list->verticalScrollBar()->value(),scroll);QCOMPARE(list->count(),5);
    }
    void lateUsageCannotRetargetAnotherComputer() {
        flag("slow-mac");flag("mac-started",false);
        NewSessionDialog dialog(program(),fleet(),"mac","codex");dialog.show();
        QTRY_VERIFY(QFile::exists(m_fixture.filePath("mac-started")));
        auto *computer=dialog.findChild<QComboBox *>("launchComputer");computer->setCurrentIndex(computer->findData(QString()));
        QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(choices(dialog)->count(),5);choices(dialog)->setCurrentRow(row(dialog,"work"));flag("released");
        QTRY_VERIFY(choices(dialog)->currentItem()->toolTip().contains("17% used"));
        QVERIFY(!choices(dialog)->currentItem()->toolTip().contains("91% used"));
        QTest::qWait(100);QVERIFY(!choices(dialog)->currentItem()->toolTip().contains("91% used"));
    }
    void unknownAndEndedWindowsAreExplicit() {
        flag("missing");NewSessionDialog dialog(program(),fleet(),{},"codex");dialog.show();
        QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(choices(dialog)->count(),5);
        QTRY_VERIFY(choices(dialog)->item(row(dialog,"work"))->toolTip().contains("Usage unavailable"));
        QVERIFY(!choices(dialog)->item(row(dialog,"work"))->toolTip().contains("0% used"));
        flag("missing",false);flag("ended");dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTRY_VERIFY(choices(dialog)->item(row(dialog,"work"))->toolTip().contains("Window ended; refresh usage"));
    }
};
QTEST_MAIN(TestNewSessionDialog)
#include "test_newsessiondialog.moc"
