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
    QTRY_VERIFY(browser.sessions("").size()==1 && browser.sessions("").first().name.endsWith("archive2"));QVERIFY(!more->isVisible());
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
 void bidirectionalWindowsStayBoundedBeyondTheCap(){
    QTemporaryDir dir;const auto file=dir.filePath("hgs");
    QVERIFY(FakeCli::install(file,R"(#!/usr/bin/env python3
import json,sys
assert sys.argv[1:]==['archives','--json']
q=json.load(sys.stdin);start=int(q['cursor'].removeprefix('page_') or '0');total=257
rows=[dict(name='codex/example/'+str(i),cmd='codex',state='archived',archive_id='10000000-0000-4000-8000-'+str(i+1).zfill(12),archived_at=q['from']+total-i) for i in range(start,min(start+50,total))]
print(json.dumps(dict(version=1,sessions=rows,next_cursor='page_'+str(start+50) if start+50<total else '',previous_cursor='page_'+str(max(0,start-50)) if start else '',position=start,total=total,**{'from':q['from'],'until':q['until'],'query':q['query']})))
)"));
    HgsClient client(FakeCli::program(file));ArchiveBrowser browser(client);QSignalSpy ready(&client,&HgsClient::archivePageReady);
    browser.resize(250,400);browser.configure({{"","Local"}},true);browser.show();
    auto *older=browser.findChild<QPushButton*>("archiveMore"),*newer=browser.findChild<QPushButton*>("archiveNewer");QVERIFY(older&&newer);
    QSet<QString> seen;int requests=0;
    for(int start=0;start<257;start+=50){
        ++requests;QTRY_COMPARE(ready.size(),requests);QVERIFY(browser.sessions("").size()<=50);
        QCOMPARE(browser.sessions("").first().name,QString("codex/example/%1").arg(start));
        QVERIFY(browser.findChild<QLabel*>("archiveStatus")->text().contains(QString::number(start+1)+"–"));
        for(const auto &row:browser.sessions(""))seen.insert(row.archiveId);
        QCOMPARE(newer->isVisible(),start>0);if(older->isVisible())QTest::mouseClick(older,Qt::LeftButton);
    }
    QCOMPARE(seen.size(),257);QVERIFY(!older->isVisible());QVERIFY(browser.hasMore());
    for(int start:{200,150,100,50,0}){
        QTest::mouseClick(newer,Qt::LeftButton);++requests;QTRY_COMPARE(ready.size(),requests);
        QCOMPARE(browser.sessions("").size(),50);QCOMPARE(browser.sessions("").first().name,QString("codex/example/%1").arg(start));
    }
    QVERIFY(!newer->isVisible());QCOMPARE(requests,11);
 }
};
QTEST_MAIN(TestArchiveBrowser)
#include "test_archivebrowser.moc"
