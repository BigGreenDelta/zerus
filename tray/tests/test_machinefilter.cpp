#include <QtTest>
#include <QCheckBox>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include "MachineFilter.h"
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
private:
    QTemporaryDir dir;
};
QTEST_MAIN(TestMachineFilter)
#include "test_machinefilter.moc"
