#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QComboBox>
#include <QDateEdit>
#include <QLabel>
#include <QPushButton>
#include "ArchiveBrowser.h"
#include "FakeCli.h"
class TestArchiveBrowser:public QObject {
 Q_OBJECT
private slots:
 void lazyPagesDateResetAndNarrowGeometry(){
    QTemporaryDir dir;const auto file=dir.filePath("hgs");
    QVERIFY(FakeCli::install(file,R"(#!/usr/bin/env python3
import sys,json,time
assert sys.argv[1:]==['archives','--json']
q=json.load(sys.stdin)
time.sleep(.15)
n=2 if q['cursor'] else 1
r={'version':1,'sessions':[{'name':'codex/example/archive'+str(n),'cmd':'codex','state':'archived','archive_id':'10000000-0000-4000-8000-00000000000'+str(n),'archived_at':q['from']+1}], 'next_cursor':'next' if n==1 else '', 'total':2,'from':q['from'],'until':q['until'],'query':q['query']}
print(json.dumps(r))
)"));
    HgsClient client(FakeCli::program(file));ArchiveBrowser browser(client);QSignalSpy ready(&client,&HgsClient::archivePageReady);
    browser.configure({{"","Local"}},false);QTest::qWait(100);QCOMPARE(ready.size(),0);
    browser.resize(250,280);browser.configure({{"","Local"}},true);browser.show();
    QTest::qWait(40);browser.configure({{"","Local"}},false);QTest::qWait(250);QCOMPARE(ready.size(),0);
    browser.configure({{"","Local"}},true);
    QTRY_COMPARE(browser.sessions("").size(),1);QCOMPARE(browser.total(),2);
    auto *more=browser.findChild<QPushButton*>("archiveMore");QVERIFY(more->isVisible());QTest::mouseClick(more,Qt::LeftButton);
    QTRY_COMPARE(browser.sessions("").size(),2);QVERIFY(!more->isVisible());
    auto *period=browser.findChild<QComboBox*>("archivePeriod");period->setCurrentIndex(1);
    QTRY_COMPARE(browser.sessions("").size(),1);QVERIFY(!browser.includes(1));
    period->setCurrentIndex(4);auto *from=browser.findChild<QDateEdit*>("archiveFrom");auto *until=browser.findChild<QDateEdit*>("archiveUntil");
    QTRY_VERIFY(from->isVisible());QVERIFY(until->isVisible());
    from->setDate(QDate(2026,10,15));until->setDate(QDate(2026,10,14));
    QVERIFY(browser.findChild<QLabel*>("archiveStatus")->text().contains("end date"));QVERIFY(!more->isEnabled());
    until->setDate(from->date());QTRY_VERIFY(!browser.sessions("").isEmpty());
    QVERIFY(browser.includes(QDate(2026,10,15).startOfDay().toSecsSinceEpoch()));QVERIFY(!browser.includes(QDate(2026,10,16).startOfDay().toSecsSinceEpoch()));
    for(auto *control:{static_cast<QWidget*>(period),static_cast<QWidget*>(from),static_cast<QWidget*>(until),static_cast<QWidget*>(more)})if(control->isVisible())QVERIFY(browser.rect().contains(QRect(control->mapTo(&browser,QPoint()),control->size())));
 }
};
QTEST_MAIN(TestArchiveBrowser)
#include "test_archivebrowser.moc"
