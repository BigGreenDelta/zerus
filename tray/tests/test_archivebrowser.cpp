#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QFrame>
#include <QScreen>
#include <QScopeGuard>
#include <QCalendarWidget>
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
    auto *period=browser.findChild<QPushButton*>("archivePeriod");QTest::mouseClick(period,Qt::LeftButton);
    auto *panel=browser.findChild<QFrame*>("archiveDatePanel");QVERIFY(panel->isVisible());
    const auto before=ready.size();QTest::qWait(200);QCOMPARE(ready.size(),before);
    QTest::mouseClick(browser.findChild<QPushButton*>("archivePreset1"),Qt::LeftButton);
    QTRY_COMPARE(browser.sessions("").size(),1);QVERIFY(!browser.includes(1));
    QTest::mouseClick(period,Qt::LeftButton);auto *from=browser.findChild<QDateEdit*>("archiveFrom");auto *until=browser.findChild<QDateEdit*>("archiveUntil");
    QTRY_VERIFY(from->isVisible());QVERIFY(until->isVisible());
    const auto draftRequests=ready.size();
    from->setDate(QDate(2026,10,15));until->setDate(QDate(2026,10,14));
    auto *apply=browser.findChild<QPushButton*>("archiveRangeApply");QVERIFY(!apply->isEnabled());
    QVERIFY(browser.findChild<QLabel*>("archiveRangeHint")->text().contains("End date"));
    QTest::qWait(200);QCOMPARE(ready.size(),draftRequests);QVERIFY(!browser.sessions("").isEmpty());
    QTest::keyClick(from,Qt::Key_Escape);QVERIFY(!panel->isVisible());
    QTest::mouseClick(period,Qt::LeftButton);QCOMPARE(from->date(),QDate::currentDate());
    from->setDate(QDate(2026,10,15));until->setDate(from->date());QVERIFY(apply->isEnabled());
    QTest::qWait(200);QCOMPARE(ready.size(),draftRequests);
    QTest::mouseClick(apply,Qt::LeftButton);QVERIFY(!panel->isVisible());QTRY_COMPARE(ready.size(),draftRequests+1);
    QVERIFY(browser.includes(QDate(2026,10,15).startOfDay().toSecsSinceEpoch()));QVERIFY(!browser.includes(QDate(2026,10,16).startOfDay().toSecsSinceEpoch()));
    for(auto *control:{period,more,browser.findChild<QPushButton*>("archiveRefresh")})if(control->isVisible())QVERIFY(browser.rect().contains(QRect(control->mapTo(&browser,QPoint()),control->size())));
 }
 void themedPopoverGeometry_data(){
    QTest::addColumn<bool>("dark");QTest::addColumn<int>("width");QTest::addColumn<double>("scale");
    QTest::newRow("dark")<<true<<390<<1.;QTest::newRow("light")<<false<<390<<1.;
    QTest::newRow("narrow-large")<<true<<250<<1.5;
 }
 void themedPopoverGeometry(){
    QFETCH(bool,dark);QFETCH(int,width);QFETCH(double,scale);
    const auto originalFont=qApp->font();const auto restoreFont=qScopeGuard([&]{qApp->setFont(originalFont);});
    auto font=originalFont;font.setPointSizeF(font.pointSizeF()*scale);qApp->setFont(font);
    QTemporaryDir dir;const auto file=dir.filePath("hgs");
    QVERIFY(FakeCli::install(file,R"(#!/usr/bin/env python3
import json,sys
assert sys.argv[1:]==['archives','--json']
q=json.load(sys.stdin)
rows=[dict(name='codex/example/'+str(i),cmd='codex',state='archived',archive_id='10000000-0000-4000-8000-'+str(i+1).zfill(12),archived_at=q['from']+i+1) for i in range(50)]
print(json.dumps(dict(version=1,sessions=rows,next_cursor='next',previous_cursor='',position=0,total=2043,**{'from':q['from'],'until':q['until'],'query':q['query']})))
)"));
    HgsClient client(FakeCli::program(file));ArchiveBrowser browser(client);browser.setTheme(dark);
    auto palette=browser.palette();palette.setColor(QPalette::Window,dark?QColor("#161b21"):QColor("#f5f7f9"));browser.setPalette(palette);browser.setAutoFillBackground(true);
    browser.resize(width,browser.sizeHint().height());browser.configure({{"","Fixture computer"}},true);browser.show();
    QTRY_COMPARE(browser.sessions("").size(),50);
    auto *period=browser.findChild<QPushButton*>("archivePeriod");QTest::mouseClick(period,Qt::LeftButton);
    auto *panel=browser.findChild<QFrame*>("archiveDatePanel");QVERIFY(panel->isVisible());
    QCOMPARE(panel->font().pointSizeF(),font.pointSizeF());
    QVERIFY(panel->screen()->availableGeometry().contains(panel->frameGeometry()));
    for(auto *control:panel->findChildren<QWidget*>())if(control->isVisible()&&control->window()==panel)
        QVERIFY2(panel->rect().contains(QRect(control->mapTo(panel,QPoint()),control->size())),qPrintable(control->objectName()));
    auto *from=browser.findChild<QDateEdit*>("archiveFrom");QVERIFY(from->styleSheet().isEmpty());
    const auto output=qEnvironmentVariable("ZERUS_ARCHIVE_RASTER_DIR");
    if(!output.isEmpty()){
        QDir().mkpath(output);QVERIFY(browser.grab().save(output+"/"+QTest::currentDataTag()+"-toolbar.png"));
        QVERIFY(panel->grab().save(output+"/"+QTest::currentDataTag()+"-panel.png"));
    }
    QTest::mouseClick(from,Qt::LeftButton,Qt::NoModifier,QPoint(from->width()-12,from->height()/2));
    auto *calendar=from->calendarWidget();QTRY_VERIFY(calendar->isVisible());
    QVERIFY(calendar->screen()->availableGeometry().contains(calendar->window()->frameGeometry()));
    if(!output.isEmpty())QVERIFY(calendar->window()->grab().save(output+"/"+QTest::currentDataTag()+"-calendar.png"));
    QTest::keyClick(calendar,Qt::Key_Escape);QTRY_VERIFY(!calendar->isVisible());QVERIFY(panel->isVisible());
    QTest::mouseClick(browser.findChild<QPushButton*>("archiveRangeCancel"),Qt::LeftButton);QVERIFY(!panel->isVisible());
    QCOMPARE(period->text(),QString("All dates"));
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
