#include <QtTest>
#include <QCheckBox>
#include <QMenu>
#include <QLabel>
#include <QDir>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyleOptionButton>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include "MachineFilter.h"
#include "ProjectFilter.h"
#include <QScrollArea>
#include <QScrollBar>
#include "MachineAppearance.h"

class TestMachineFilter : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("machine-filter");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
    }
    void multipleChoicesStayOpenAndChipsRemoveOne() {
        QWidget page; auto *layout=new QVBoxLayout(&page); auto *filter=new MachineFilter;
        layout->addWidget(filter->button()); layout->addWidget(filter); layout->addStretch();
        FleetState fleet; BoxState local; local.host="arch"; local.ok=true; fleet.setLocal(local,0);
        for(const QString &host: {"mac","build","work","test"}) { BoxState box; box.host=host; box.ok=true; fleet.setPeer(box,0); }
        filter->setFleet(fleet); page.resize(290,300); page.show();
        auto *menu=filter->button()->menu(); menu->popup(page.mapToGlobal(QPoint(0,30))); QTRY_VERIFY(menu->isVisible());
        const auto choices=menu->findChildren<QCheckBox *>("machineFilterChoice"); QCOMPARE(choices.size(),5);
        const auto choose=[&](const QString &host) { for(auto *choice:choices) if(choice->property("host")==host) choice->click(); };
        choose("@local"); choose("mac"); QCOMPARE(filter->selection(),(QSet<QString>{"@local","mac"})); QVERIFY(menu->isVisible());
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        auto chips=filter->findChildren<QPushButton *>("machineFilterChip"); QCOMPARE(chips.size(),2);
        for(auto *chip:chips) if(chip->property("host")=="@local") chip->click();
        QCOMPARE(filter->selection(),QSet<QString>{"mac"});
        // Reopening the menu reflects chip edits and resets only on explicit All machines.
        menu->close(); menu->popup(page.mapToGlobal(QPoint(0,30)));
        menu->findChild<QPushButton *>("allMachineFilters")->click(); QCOMPARE(filter->selection(),QSet<QString>());
        QVERIFY(filter->isHidden()); QVERIFY(menu->isVisible()); menu->close();
    }
    void wrappingAndAppearancePersist() {
        QWidget page; auto *layout=new QVBoxLayout(&page); auto *filter=new MachineFilter;
        layout->addWidget(filter->button()); layout->addWidget(filter); layout->addStretch();
        FleetState fleet; BoxState box; box.host="arch"; box.ok=true; fleet.setLocal(box,0);
        filter->setFleet(fleet); filter->setSelection({"@local","long-machine-name","another-machine"}); page.resize(260,400); page.show();
        QCoreApplication::processEvents();
        const auto chips=filter->findChildren<QPushButton *>("machineFilterChip"); QCOMPARE(chips.size(),3);
        bool wraps=false; for(auto *chip:chips) { QVERIFY(chip->width()<page.width()); wraps|=chip->y()>0; } QVERIFY(wraps);
        const auto before=MachineAppearance::color("arch"); MachineAppearance::setColor("arch",QColor("#ff8800"));
        QCOMPARE(MachineAppearance::color("arch"),QColor("#ff8800")); filter->refreshColors();
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        bool found=false; for(auto *chip:filter->findChildren<QPushButton *>("machineFilterChip")) if(chip->property("host")=="@local") {
            found=true; QVERIFY(chip->styleSheet().contains(MachineAppearance::backgroundColor(QColor("#ff8800"),false).name()));
        }
        QVERIFY(found); MachineAppearance::resetColor("arch"); QCOMPARE(MachineAppearance::color("arch"),before);
    }
    void sessionCountsStayVisibleAndUpdateWithoutReplacingChoices() {
        QWidget page; auto *layout=new QVBoxLayout(&page); auto *filter=new MachineFilter;
        layout->addWidget(filter->button()); layout->addWidget(filter);
        FleetState fleet; BoxState local; local.host="mac"; local.ok=true; fleet.setLocal(local,0);
        for (const auto &host : {"mac", "empty"}) { BoxState peer; peer.host=host; peer.ok=true; fleet.setPeer(peer,0); }
        filter->setFleet(fleet); filter->setSessionCounts({{"@local",12},{"mac",3}}, "Active");
        page.resize(320,300); page.show();
        auto *menu=filter->button()->menu(); menu->popup(page.mapToGlobal(QPoint(0,30))); QTRY_VERIFY(menu->isVisible());
        QMap<QString,QCheckBox *> choices;
        for (auto *choice : menu->findChildren<QCheckBox *>("machineFilterChoice")) choices[choice->property("host").toString()]=choice;
        QCOMPARE(choices.size(),3); QCOMPARE(choices["@local"]->text(),QString("mac (this machine) (12)"));
        QCOMPARE(choices["mac"]->text(),QString("mac (3)")); QCOMPARE(choices["empty"]->text(),QString("empty (0)"));
        auto *all=menu->findChild<QPushButton *>("allMachineFilters"); QCOMPARE(all->text(),QString("All machines (15)"));
        QCOMPARE(menu->findChild<QLabel *>("machineFilterCountScope")->text(),QString("Active before search"));
        QSignalSpy changed(filter,&MachineFilter::selectionChanged); choices["mac"]->click(); QCOMPARE(changed.size(),1);
        QCOMPARE(choices["@local"]->property("sessionCount").toInt(),12); QVERIFY(menu->isVisible());
        filter->setSessionCounts({{"@local",2},{"mac",4}}, "Archive");
        QCOMPARE(changed.size(),1); QCOMPARE(all->text(),QString("All machines (6)"));
        QVERIFY(choices["mac"]->isChecked()); QCOMPARE(choices["mac"]->text(),QString("mac (4)"));
        QVERIFY(choices["mac"]->accessibleName().contains("4 sessions"));
        const auto current=menu->findChildren<QCheckBox *>("machineFilterChoice"); QVERIFY(current.contains(choices["mac"]));
        QCOMPARE(choices["empty"]->property("sessionCount").toInt(),0); menu->close();
    }
    void projectChoicesKeepIdentityCountsAndMissingSelection_data() {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void projectChoicesKeepIdentityCountsAndMissingSelection() {
        QFETCH(bool,dark);
        QWidget page; auto *layout=new QVBoxLayout(&page); auto *filter=new ProjectFilter;
        layout->addWidget(filter->button()); layout->addWidget(filter); layout->addStretch();
        SessionOrganization org; const auto first=org.createGroup("Same name"),second=org.createGroup("Same name");
        org.setColor(first,"#ff8800"); filter->setProjects(org.groups()); filter->setTheme(dark);
        filter->setSessionCounts({{first,4},{second,2}},"Active / selected machines"); page.resize(260,400); page.show();
        QSignalSpy changed(filter,&ProjectFilter::selectionChanged);
        auto *menu=filter->button()->menu(); menu->popup(page.mapToGlobal(QPoint(0,30))); QTRY_VERIFY(menu->isVisible());
        QMap<QString,QCheckBox *> choices;
        for(auto *choice:menu->findChildren<QCheckBox *>("projectFilterChoice")) choices[choice->property("project").toString()]=choice;
        QCOMPARE(choices.size(),3); choices[first]->click(); choices[second]->click(); QVERIFY(menu->isVisible());
        QCOMPARE(filter->selection(),(QSet<QString>{first,second})); QCOMPARE(changed.size(),2);
        filter->setSessionCounts({{first,1},{second,0}},"Archive");
        QCOMPARE(choices[first]->property("sessionCount").toInt(),1); QCOMPARE(choices[second]->property("sessionCount").toInt(),0);
        QCOMPARE(menu->findChild<QPushButton *>("allProjectFilters")->text(),QString("All projects (1)"));
        QVERIFY(choices[first]->isChecked()); QVERIFY(choices[second]->isChecked()); QCOMPARE(changed.size(),2);
        org.renameGroup(first,"Renamed & retained"); filter->setProjects(org.groups());
        QCOMPARE(choices[first]->text(),QString("Renamed && retained (1)"));
        QCOMPARE(menu->findChildren<QCheckBox *>("projectFilterChoice").size(),3);
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        for(auto *chip:filter->findChildren<QPushButton *>("projectFilterChip")) if(chip->property("project")==first) chip->click();
        QCOMPARE(filter->selection(),QSet<QString>{second}); QVERIFY(!choices[first]->isChecked());
        filter->setConcealed(true); filter->setProjects(org.groups()); QVERIFY(filter->isHidden());
        filter->setConcealed(false); QVERIFY(!filter->isHidden());
        org.removeGroup(second); filter->setProjects(org.groups()); QCOMPARE(filter->selection(),QSet<QString>{second});
        QCOMPARE(choices[second]->text(),QString("Unavailable project (0)"));
        menu->hide(); menu->popup(page.mapToGlobal(QPoint(0,30)));
        menu->findChild<QPushButton *>("allProjectFilters")->click(); QVERIFY(filter->selection().isEmpty()); QVERIFY(filter->isHidden()); QVERIFY(menu->isVisible()); menu->hide();
        // A large catalog stays bounded and scrollable. New entries never join an explicit selection.
        filter->setSelection({first}); for(int i=0;i<60;++i) org.createGroup(QString("Project %1").arg(i)); filter->setProjects(org.groups());
        menu->popup(page.mapToGlobal(QPoint(0,30))); QCOMPARE(filter->selection(),QSet<QString>{first});
        auto *scroll=menu->findChild<QScrollArea *>("projectFilterChoices"); QVERIFY(scroll); QVERIFY(scroll->height()<=324);
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum()>0); menu->hide();
    }
    void controlOnlyComputerIsNoFilterChoice() {
        QWidget page; auto *layout=new QVBoxLayout(&page); auto *filter=new MachineFilter;
        layout->addWidget(filter->button()); layout->addWidget(filter); layout->addStretch();
        FleetState fleet; BoxState local; local.host="windows-pc"; local.ok=true; local.localSessions=false; fleet.setLocal(local,0);
        BoxState mac; mac.host="mac"; mac.ok=true; fleet.setPeer(mac,0);
        filter->setFleet(fleet); page.resize(290,300); page.show();
        auto *menu=filter->button()->menu(); menu->popup(page.mapToGlobal(QPoint(0,30))); QTRY_VERIFY(menu->isVisible());
        const auto choices=menu->findChildren<QCheckBox *>("machineFilterChoice"); QCOMPARE(choices.size(),1);
        QCOMPARE(choices[0]->property("host").toString(),QString("mac")); menu->close();
    }
    void menuRowsAndPaintedIndicatorsAlign_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void menuRowsAndPaintedIndicatorsAlign() {
        QFETCH(bool, dark);
        QWidget page; auto *layout=new QVBoxLayout(&page); auto *filter=new MachineFilter;
        page.setStyleSheet(QString("QWidget { font-size:13px; } QLabel { color:%1; background:transparent; } QPushButton { background:%2; color:%1; border:1px solid #627082; border-radius:7px; padding:8px 13px; min-height:18px; } QMenu { background:%2; color:%1; border:1px solid #627082; padding:4px; } QCheckBox { color:%1; spacing:8px; }")
            .arg(dark ? "#e8edf4" : "#1a2733",dark ? "#1c2229" : "#ffffff"));
        layout->addWidget(filter->button()); layout->addWidget(filter); layout->addStretch();
        FleetState fleet; BoxState local; local.host="mac"; local.ok=true; fleet.setLocal(local,0);
        BoxState peer; peer.host="arch"; peer.ok=true; fleet.setPeer(peer,0);
        filter->setFleet(fleet); filter->setTheme(dark); page.resize(300,250); page.show();
        filter->setSessionCounts({{"@local",12},{"arch",4}}, "Active");
        auto *menu=filter->button()->menu(); menu->popup(page.mapToGlobal(QPoint(0,30))); QTRY_VERIFY(menu->isVisible());
        auto *all=menu->findChild<QPushButton *>("allMachineFilters"); QVERIFY(all);
        const auto choices=menu->findChildren<QCheckBox *>("machineFilterChoice"); QCOMPARE(choices.size(),2);
        QRect previous=all->geometry();
        for (auto *choice:choices) {
            QVERIFY2(choice->geometry().top() > previous.bottom(),"Machine chooser rows overlap");
            previous=choice->geometry();
            QStyleOptionButton option; option.initFrom(choice); option.rect=choice->rect();
            option.text=choice->text(); option.icon=choice->icon(); option.iconSize=choice->iconSize();
            const QRect indicator=choice->style()->subElementRect(QStyle::SE_CheckBoxIndicator,&option,choice);
            const QRect contents=choice->style()->subElementRect(QStyle::SE_CheckBoxContents,&option,choice);
            QVERIFY(qAbs(indicator.center().y()-contents.center().y()) <= 1);
            QVERIFY(choice->rect().contains(indicator));
            // Check the actual pixels as well as Qt's reported geometry: the
            // native macOS indicator can be painted above its declared rectangle.
            const QSignalBlocker blocker(choice);
            choice->setChecked(false); const QImage off=choice->grab().toImage();
            choice->setChecked(true); const QImage on=choice->grab().toImage();
            QCOMPARE(off.size(),on.size()); QRect changed;
            for (int y=0;y<on.height();++y) for (int x=0;x<on.width();++x)
                if (on.pixel(x,y)!=off.pixel(x,y)) changed=changed.united(QRect(x,y,1,1));
            QVERIFY(!changed.isEmpty());
            const qreal dpr=on.devicePixelRatio();
            const QRectF painted(changed.x()/dpr,changed.y()/dpr,changed.width()/dpr,changed.height()/dpr);
            QVERIFY2(QRectF(indicator).adjusted(-1,-1,1,1).contains(painted),"Painted checkbox escapes its indicator rectangle");
        }
        const auto preview=qEnvironmentVariable("ZERUS_MACHINE_FILTER_PREVIEW");
        if (!preview.isEmpty()) { QDir().mkpath(preview); QVERIFY(menu->grab().save(preview+(dark ? "/desktop-dark.png" : "/desktop-light.png"))); }
        menu->close();
    }
private:
    QTemporaryDir dir;
};
QTEST_MAIN(TestMachineFilter)
#include "test_machinefilter.moc"
