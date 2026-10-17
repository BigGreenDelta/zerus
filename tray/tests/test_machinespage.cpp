#include <QtTest>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include "FakeCli.h"
#include "PopupMouse.h"
#include "MachinesPage.h"
#include "MessageBoxIcons.h"
#include "MachineAppearance.h"
#include "MachinePlatform.h"

class TestMachinesPage : public QObject {
    Q_OBJECT
private slots:
    void init();
    void removalRequiresExplicitConfirmationOfSameConnection_data();
    void removalRequiresExplicitConfirmationOfSameConnection();
    void localReadOnlyAndNewProfileCannotOverwrite();
    void newMachineDraftCancelAndDiscard_data();
    void newMachineDraftCancelAndDiscard();
    void editsKeepDraftWhileFleetRefreshesAndFailuresAreVisible();
    void testConnectionAndOpenSshKeepSelectedAlias();
    void installationStreamsOutputAndKeepsProfileLocked();
    void effectiveValuesNeverBecomeOverridesOrReplaceDrafts();
    void staleResolutionCannotPopulateAnotherMachine();
    void colorsWorkForLocalAndPersistWithoutSavingConnection();
    void colorPickerDismissalKeepsTheSavedColor_data();
    void colorPickerDismissalKeepsTheSavedColor();
    void dashboardSelectionWaitsForProfiles();
    void controlOnlyComputerIsAConsoleWithoutSessions();
    void countsReadInSingularAndPlural();
    void windowsConsoleEnablesLocalSessions();
    void enablingIsOfferedOnlyToAWindowsConsole();
    void localSessionHostStates_data();
    void localSessionHostStates();
    void preview();
private:
    template<class T> T *widget(MachinesPage &page, const char *name) {
        auto *result = page.findChild<T *>(name); Q_ASSERT(result); return result;
    }
    void start(MachinesPage &page);
    QJsonArray calls() const;
    void flag(const QString &name, const QString &text = "1");
    QTemporaryDir m_dir;
    QString m_script;
};

void TestMachinesPage::init()
{
    MessageBoxIcons::install();
    QVERIFY(m_dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());
    QCoreApplication::setOrganizationName("hgs-machines-test"); QCoreApplication::setApplicationName("MachinesPage"); QSettings().clear(); MachineAppearance::invalidate();
    for (const auto &name : {"calls.jsonl", "stale", "probe-mode", "resolve-delay", "list-delay", "host-mode"}) QFile::remove(m_dir.filePath(name));
    m_script = m_dir.filePath("hgs");
    QFile fixture(m_script); QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"(#!/usr/bin/env python3
import json, pathlib, sys, time
root=pathlib.Path(__file__).parent
args=sys.argv[1:]
with (root/'calls.jsonl').open('a') as out: out.write(json.dumps(args)+'\n')
if args[0]=='session-host':
 assert args==['session-host','install','--json'],args
 time.sleep(.05)
 if (root/'host-mode').exists():
  print('cannot enable local sessions: access denied',file=sys.stderr); sys.exit(1)
 print(json.dumps({'enabled':True,'version':'1.52.0','marker_created':True,
  'cli':{'path':'C:\\Users\\user\\.local\\bin\\hgs.exe','sha256':'a'*64,'version':'1.52.0','changed':True}}))
 sys.exit(0)
assert args[0]=='machine'
command=args[1]
if command=='ls':
 if (root/'list-delay').exists(): time.sleep(.05)
 print((root/'data.json').read_text())
elif command=='resolve':
 if (root/'resolve-delay').exists(): time.sleep(.2 if args[2]=='mac' else .01)
 print(json.dumps({'alias':args[2],'effective':{'hostname':args[2]+'.example','user':'ssh-user','port':'2222','identity_files':['~/.ssh/id_ed25519'],'proxy_jump':'jump.example'}}))
elif command=='set':
 if (root/'stale').exists():
  print('Machine settings changed elsewhere. Reload the page before saving.',file=sys.stderr); sys.exit(1)
 data=json.loads((root/'data.json').read_text())
 profile=json.loads(args[args.index('--json')+1])
 data['machines']=[r for r in data['machines'] if r['alias']!=args[2]]
 data['machines'].append({'alias':args[2],'connection':profile,'enabled':True,'source':'local'})
 data['revision']='revision-2'
 (root/'data.json').write_text(json.dumps(data))
 print('{"ok":true}')
elif command=='remove':
 data=json.loads((root/'data.json').read_text());data['machines']=[r for r in data['machines'] if r['alias']!=args[2]]
 (root/'data.json').write_text(json.dumps(data));print('{"ok":true}')
elif command=='check':
 time.sleep(.03)
 if (root/'probe-mode').exists(): print('{"ssh_ok":true,"ok":false,"error":"SSH connected, but no setup report"}')
 else: print('{"ssh_ok":true,"ok":true,"os":"Darwin","arch":"arm64","hgs":"hgs 1.17.0","tmux":"tmux 3.5"}')
elif command=='setup':
 print('Building HGS for this machine…',flush=True); time.sleep(.06)
 print('Setup complete: hgs fixture')
else: raise AssertionError(args)
)");
    fixture.close(); QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    QFile data(m_dir.filePath("data.json")); QVERIFY(data.open(QIODevice::WriteOnly));
    data.write(R"({"local":"arch","revision":"revision-1","machines":[{"alias":"mac","connection":{"enabled":true},"enabled":true,"source":"config","inherited":true}]})");
}
void TestMachinesPage::removalRequiresExplicitConfirmationOfSameConnection_data()
{
    QTest::addColumn<QString>("choice");
    for (const auto *choice : {"cancel", "enter", "escape", "proceed", "selection", "revision"}) QTest::newRow(choice) << QString(choice);
}

void TestMachinesPage::removalRequiresExplicitConfirmationOfSameConnection()
{
    QFETCH(QString, choice);
    flag("data.json", R"({"local":"arch","revision":"revision-1","machines":[{"alias":"mac","connection":{"enabled":true,"hostname":"mac.example"},"enabled":true,"source":"local"}]})");
    MachinesPage page(FakeCli::program(m_script)); start(page); page.selectMachine("mac");
    auto *remove=widget<QPushButton>(page,"removeMachine");QTRY_VERIFY(remove->isEnabled());
    bool shown=false;
    QTimer respond;respond.setInterval(5);
    connect(&respond,&QTimer::timeout,&page,[&] {
        auto *box=page.findChild<QMessageBox *>("removeMachineConfirm");if(!box||!box->isVisible())return;
        respond.stop();shown=true;
        QCOMPARE(box->defaultButton(),qobject_cast<QPushButton *>(box->button(QMessageBox::Cancel)));
        QCOMPARE(box->escapeButton(),box->button(QMessageBox::Cancel));QVERIFY(box->text().contains("mac"));
        if(choice=="cancel"){box->button(QMessageBox::Cancel)->click();return;}
        if(choice=="enter"||choice=="escape"){QTest::keyClick(box,choice=="enter"?Qt::Key_Return:Qt::Key_Escape);return;}
        if(choice=="selection")page.selectMachine({});
        if(choice=="revision"){
            flag("data.json",R"({"local":"arch","revision":"revision-2","machines":[{"alias":"mac","connection":{"enabled":true,"hostname":"mac.example"},"enabled":true,"source":"local"}]})");
            page.reload();QTRY_VERIFY(remove->isEnabled());
        }
        for(auto *button:box->buttons())if(box->buttonRole(button)==QMessageBox::DestructiveRole)button->click();
    });respond.start();remove->click();respond.stop();QVERIFY(shown);
    const auto removals=[&]{QJsonArray result;for(const auto &call:calls())if(call.toArray().at(1)=="remove")result.append(call);return result;};
    // The confirmed removal runs a fake CLI process, which a loaded runner can start
    // later than any fixed wait; a refused one must send nothing within the window.
    if(choice=="proceed")QTRY_COMPARE(removals().size(),1);
    else{QTest::qWait(150);QCOMPARE(removals().size(),0);}
    if(choice=="proceed")QCOMPARE(removals().first().toArray(),QJsonArray({"machine","remove","mac","--revision","revision-1"}));
    if(choice=="selection"||choice=="revision")QVERIFY(widget<QLabel>(page,"machineNotice")->text().contains("connection changed"));
}

void TestMachinesPage::flag(const QString &name, const QString &text)
{
    QFile file(m_dir.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(text.toUtf8());
}
void TestMachinesPage::start(MachinesPage &page)
{
    BoxState local; local.host = "arch"; local.ok = true;
    FleetState fleet; fleet.setLocal(local, 0); page.setFleet(fleet);
    page.resize(980, 800); page.show();
    QTRY_COMPARE(widget<QListWidget>(page, "machineProfiles")->count(), 2);
    QTRY_VERIFY(widget<QPushButton>(page, "addMachine")->isEnabled());
}
QJsonArray TestMachinesPage::calls() const
{
    QFile file(m_dir.filePath("calls.jsonl")); if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonArray result;
    for (const auto &line : file.readAll().split('\n')) if (!line.isEmpty()) result.append(QJsonDocument::fromJson(line).array());
    return result;
}
void TestMachinesPage::localReadOnlyAndNewProfileCannotOverwrite()
{
    MachinesPage page(FakeCli::program(m_script)); start(page);
    QVERIFY(!widget<QPushButton>(page, "browseMachineKey")->isEnabled());
    QVERIFY(!widget<QPushButton>(page, "saveMachine")->isEnabled());
    QSignalSpy accounts(&page, &MachinesPage::accountsRequested);
    widget<QPushButton>(page, "machineAccounts")->click(); QCOMPARE(accounts.size(), 1); QCOMPARE(accounts[0][0].toString(), "");
    widget<QPushButton>(page, "addMachine")->click();
    QVERIFY(!widget<QPushButton>(page, "machineAccounts")->isEnabled());
    auto *alias = widget<QLineEdit>(page, "machineAlias");
    auto *save = widget<QPushButton>(page, "saveMachine");
    QVERIFY(!alias->isReadOnly());
    alias->setText("mac"); QVERIFY(!save->isEnabled()); QVERIFY(save->toolTip().contains("already exists"));
    alias->setText("arch"); QVERIFY(!save->isEnabled());
    alias->setText("studio"); QVERIFY(save->isEnabled());
    widget<QLineEdit>(page, "machineHostname")->setText("192.0.2.1");
    widget<QLineEdit>(page, "machineUser")->setText("alice");
    widget<QLineEdit>(page, "machineKey")->setText(QString::fromUtf8("~/Keys/ключ space"));
    widget<QSpinBox>(page, "machinePort")->setValue(2222);
    QSignalSpy changed(&page, &MachinesPage::machinesChanged); save->click();
    QVERIFY(!widget<QPushButton>(page, "browseMachineKey")->isEnabled());
    QTRY_COMPARE(changed.size(), 1);
    QTRY_COMPARE(widget<QListWidget>(page, "machineProfiles")->count(), 3);
    QTRY_VERIFY(alias->isReadOnly());
    QJsonArray saved;
    for (const auto &call : calls()) if (call.toArray().at(1).toString() == "set") saved = call.toArray();
    QVERIFY(saved.contains("--create")); QVERIFY(saved.contains("revision-1"));
    const auto profile = QJsonDocument::fromJson(saved.at(4).toString().toUtf8()).object();
    QCOMPARE(profile["hostname"].toString(), "192.0.2.1");
    QCOMPARE(profile["user"].toString(), "alice");
    QCOMPARE(profile["port"].toInt(), 2222);
    QCOMPARE(profile["identity_file"].toString(), QString::fromUtf8("~/Keys/ключ space"));
}
void TestMachinesPage::newMachineDraftCancelAndDiscard_data()
{
    QTest::addColumn<QString>("answer");
    for(const auto *answer:{"cancel","escape","discard"})QTest::newRow(answer)<<QString(answer);
}
void TestMachinesPage::newMachineDraftCancelAndDiscard()
{
    QFETCH(QString,answer);MachinesPage page(FakeCli::program(m_script));start(page);
    auto *list=widget<QListWidget>(page,"machineProfiles");auto *alias=widget<QLineEdit>(page,"machineAlias");
    // Only two rows fit, so the draft scrolls the list and discarding it
    // scrolls back while the click that asked is still pressed. Otherwise the
    // phone panel decides the height whenever its own requests finish.
    const int twoRows=list->sizeHintForRow(0)*2+2*list->frameWidth();list->setFixedHeight(twoRows);QTRY_COMPARE(list->height(),twoRows);
    widget<QPushButton>(page,"addMachine")->click();QCOMPARE(list->count(),3);
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),QString("@new"));QTRY_VERIFY(list->verticalScrollBar()->value()>0);
    alias->setText("studio");widget<QLineEdit>(page,"machineHostname")->setText("draft.example");
    widget<QPushButton>(page,"addMachine")->click();QCOMPARE(list->count(),3);QCOMPARE(alias->text(),QString("studio"));
    FleetState state;BoxState local;local.host="arch";local.ok=true;state.setLocal(local,0);
    int prompts=0;bool draftSelectedDuringPoll=false;
    QTimer respond;respond.setInterval(5);
    connect(&respond,&QTimer::timeout,&page,[&]{
        auto *dialog=page.findChild<QMessageBox *>("unsavedMachineDialog");if(!dialog)return;
        respond.stop(); // macOS animates Escape's button click; repeated keys restart that timer.
        ++prompts;page.setFleet(state);
        draftSelectedDuringPoll=list->currentItem()&&list->currentItem()->data(Qt::UserRole)=="@new";
        if(answer=="escape")QTest::keyClick(dialog,Qt::Key_Escape);
        else dialog->button(answer=="discard"?QMessageBox::Discard:QMessageBox::Cancel)->click();
    });respond.start();
    QTest::mouseClick(list->viewport(),Qt::LeftButton,{},list->visualItemRect(list->item(1)).center());
    respond.stop();QCOMPARE(prompts,1);QVERIFY(draftSelectedDuringPoll);
    const bool discarded=answer=="discard";
    QCOMPARE(list->count(),discarded?2:3);QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),discarded?QString("mac"):QString("@new"));
    // The press selects the row under its old position after the dialog; the
    // highlight returns to the current row once that press has finished.
    const auto highlighted=[list]{QStringList keys;for(auto *item:list->selectedItems())keys<<item->data(Qt::UserRole).toString();return keys;};
    QTRY_COMPARE(highlighted(),QStringList{list->currentItem()->data(Qt::UserRole).toString()});
    if(!discarded){
        QCOMPARE(alias->text(),QString("studio"));QCOMPARE(widget<QLineEdit>(page,"machineHostname")->text(),QString("draft.example"));
        QVERIFY(widget<QPushButton>(page,"saveMachine")->isEnabled());
        const auto preview=qEnvironmentVariable("HGS_MACHINES_PREVIEW");
        if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(page.grab().save(preview+"/new-machine-draft.png"));}
        answer="discard";respond.start();widget<QPushButton>(page,"discardNewMachine")->click();respond.stop();
        QCOMPARE(list->count(),2);QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),QString("@local"));
    }
    for(const auto &call:calls())QVERIFY(call.toArray().at(1)!="set");
    // An untouched draft can also be abandoned without a dialog.
    widget<QPushButton>(page,"addMachine")->click();QCOMPARE(list->count(),3);
    widget<QPushButton>(page,"discardNewMachine")->click();QCOMPARE(list->count(),2);
}

void TestMachinesPage::editsKeepDraftWhileFleetRefreshesAndFailuresAreVisible()
{
    MachinesPage page(FakeCli::program(m_script)); start(page);
    widget<QListWidget>(page, "machineProfiles")->setCurrentRow(1);
    QVERIFY(widget<QLineEdit>(page, "machineAlias")->isReadOnly());
    auto *hostname = widget<QLineEdit>(page, "machineHostname"); hostname->setText("changed.local");
    QVERIFY(!widget<QPushButton>(page, "checkMachine")->isEnabled());
    FleetState fleet; BoxState local; local.host = "arch"; local.ok = true; fleet.setLocal(local, 0);
    page.setFleet(fleet); QCOMPARE(hostname->text(), "changed.local");
    flag("stale"); widget<QPushButton>(page, "saveMachine")->click();
    QTRY_VERIFY(widget<QLabel>(page, "machineNotice")->text().contains("changed elsewhere"));
    QCOMPARE(hostname->text(), "changed.local");
    QVERIFY(widget<QPushButton>(page, "saveMachine")->isEnabled());
    QVERIFY(!widget<QPushButton>(page, "checkMachine")->isEnabled());
    for (const auto &call : calls()) if (call.toArray().at(1).toString() == "set") QVERIFY(!call.toArray().contains("--create"));
}
void TestMachinesPage::testConnectionAndOpenSshKeepSelectedAlias()
{
    MachinesPage page(FakeCli::program(m_script)); start(page);
    widget<QListWidget>(page, "machineProfiles")->setCurrentRow(1);
    auto *check = widget<QPushButton>(page, "checkMachine"); check->click();
    QVERIFY(!widget<QListWidget>(page, "machineProfiles")->isEnabled());
    QTRY_VERIFY(widget<QLabel>(page, "machineNotice")->text().contains("Darwin arm64"));
    QSignalSpy terminal(&page, &MachinesPage::sshTerminalRequested);
    widget<QPushButton>(page, "sshMachine")->click();
    QCOMPARE(terminal.size(), 1); QCOMPARE(terminal[0][0].toString(), "mac");
    QSignalSpy accounts(&page, &MachinesPage::accountsRequested); widget<QPushButton>(page, "machineAccounts")->click();
    QCOMPARE(accounts.size(), 1); QCOMPARE(accounts[0][0].toString(), "mac");
    flag("probe-mode"); check->click();
    QTRY_VERIFY(widget<QLabel>(page, "machineNotice")->text().contains("no setup report"));
}
void TestMachinesPage::installationStreamsOutputAndKeepsProfileLocked()
{
    MachinesPage page(FakeCli::program(m_script)); start(page);
    widget<QListWidget>(page, "machineProfiles")->setCurrentRow(1);
    QTimer::singleShot(0, &page, [&] {
        auto *dialog = page.findChild<QDialog *>("machineSetupDialog"); QVERIFY(dialog);
        dialog->findChild<QLineEdit *>("machineSetupSource")->setText(QString::fromUtf8("/tmp/source дерево"));
        dialog->accept();
    });
    QSignalSpy changed(&page, &MachinesPage::machinesChanged);
    widget<QPushButton>(page, "installMachine")->click();
    QVERIFY(!widget<QListWidget>(page, "machineProfiles")->isEnabled());
    QVERIFY(!widget<QPushButton>(page, "browseMachineKey")->isEnabled());
    QTRY_VERIFY(widget<QPlainTextEdit>(page, "machineSetupLog")->toPlainText().contains("Setup complete"));
    QTRY_COMPARE(changed.size(), 1);
    QTRY_VERIFY(widget<QLabel>(page, "machineNotice")->text().contains("Darwin arm64"));
    QJsonArray setup;
    for (const auto &call : calls()) if (call.toArray().at(1).toString() == "setup") setup = call.toArray();
    QCOMPARE(setup, QJsonArray({"machine", "setup", "mac", "--source", QString::fromUtf8("/tmp/source дерево")}));
}
void TestMachinesPage::effectiveValuesNeverBecomeOverridesOrReplaceDrafts()
{
    MachinesPage page(FakeCli::program(m_script)); start(page); flag("resolve-delay");
    widget<QListWidget>(page, "machineProfiles")->setCurrentRow(1);
    auto *hostname = widget<QLineEdit>(page, "machineHostname"); auto *effective = widget<QLabel>(page, "effectiveMachineConnection");
    QCOMPARE(hostname->text(), "");
    widget<QLineEdit>(page, "machineUser")->setText("draft-user");
    QTRY_VERIFY(effective->text().contains("mac.example"));
    QVERIFY(effective->text().contains("Unsaved edits")); QVERIFY(effective->text().contains("jump.example"));
    QCOMPARE(hostname->text(), ""); QVERIFY(hostname->placeholderText().contains("mac.example"));
    QCOMPARE(widget<QLineEdit>(page, "machineUser")->text(), "draft-user");
    QCOMPARE(widget<QSpinBox>(page, "machinePort")->value(), 0);
    widget<QPushButton>(page, "saveMachine")->click();
    QTRY_VERIFY(widget<QPushButton>(page, "checkMachine")->isEnabled());
    QJsonObject saved;
    for (const auto &call : calls()) if (call.toArray().at(1).toString() == "set") saved = QJsonDocument::fromJson(call.toArray().at(4).toString().toUtf8()).object();
    QCOMPARE(saved["hostname"].toString(), ""); QCOMPARE(saved["user"].toString(), "draft-user");
    QVERIFY(saved["port"].isNull()); QCOMPARE(saved["identity_file"].toString(), "");
}
void TestMachinesPage::staleResolutionCannotPopulateAnotherMachine()
{
    flag("resolve-delay");
    QFile file(m_dir.filePath("data.json")); QVERIFY(file.open(QIODevice::ReadOnly)); auto data = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    auto machines = data["machines"].toArray(); machines.append(QJsonObject{{"alias","studio"},{"connection",QJsonObject{}},{"enabled",true},{"source","config"}}); data["machines"] = machines;
    QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(data).toJson()); file.close();
    MachinesPage page(FakeCli::program(m_script)); BoxState box; box.host = "arch"; box.ok = true; FleetState fleet; fleet.setLocal(box, 0); page.setFleet(fleet); page.show();
    auto *list = widget<QListWidget>(page, "machineProfiles"); QTRY_COMPARE(list->count(), 3);
    list->setCurrentRow(1); list->setCurrentRow(2);
    auto *effective = widget<QLabel>(page, "effectiveMachineConnection");
    QTRY_VERIFY(effective->text().contains("studio.example"));
    QTest::qWait(250); QVERIFY(!effective->text().contains("mac.example"));
    QCOMPARE(widget<QLineEdit>(page, "machineHostname")->text(), "");
    list->setCurrentRow(1); list->setCurrentRow(0); QTest::qWait(250);
    QVERIFY(effective->isHidden()); QCOMPARE(widget<QLineEdit>(page, "machineAlias")->text(), "arch");
}
void TestMachinesPage::colorsWorkForLocalAndPersistWithoutSavingConnection()
{
    MachinesPage page(FakeCli::program(m_script)); start(page);
    auto *color = widget<QPushButton>(page, "machineColor"); auto *reset = widget<QPushButton>(page, "resetMachineColor");
    QVERIFY(color->isEnabled()); QVERIFY(reset->isEnabled());
    const QColor automatic = MachineAppearance::color("arch");
    QSignalSpy changed(&page, &MachinesPage::appearanceChanged);
    QTimer::singleShot(0, &page, [&] {
        auto *dialog = page.findChild<QColorDialog *>(); QVERIFY(dialog);
        dialog->setCurrentColor(QColor("#e05999")); dialog->accept();
    });
    color->click(); QCOMPARE(changed.size(), 1); QCOMPARE(MachineAppearance::color("arch"), QColor("#e05999"));
    QVERIFY(!widget<QPushButton>(page, "saveMachine")->isEnabled());
    auto *style = widget<QComboBox>(page, "machineLabelStyle");
    QCOMPARE(style->currentIndex(), 0); style->setCurrentIndex(1);
    QCOMPARE(changed.size(), 2); QVERIFY(MachineAppearance::vivid("arch"));
    QCOMPARE(MachineAppearance::backgroundColor(MachineAppearance::color("arch"), true, true), QColor("#e05999"));
    for (const auto &hex : {"#ffee00", "#0000ff", "#ff0080", "#777777", "#00ff00"}) {
        const QColor bg(hex), fg = MachineAppearance::textColor(bg, true, true);
        const double a = MachineAppearance::luminance(bg), b = MachineAppearance::luminance(fg);
        QVERIFY((qMax(a, b) + .05) / (qMin(a, b) + .05) >= 4.5);
    }
    widget<QListWidget>(page, "machineProfiles")->setCurrentRow(1); QCOMPARE(style->currentIndex(), 0);
    widget<QListWidget>(page, "machineProfiles")->setCurrentRow(0); QCOMPARE(style->currentIndex(), 1);
    QCOMPARE(widget<QLabel>(page, "machineLabelPreview")->text(), "arch");
    reset->click(); QCOMPARE(changed.size(), 3); QCOMPARE(MachineAppearance::color("arch"), automatic);
    widget<QPushButton>(page, "addMachine")->click(); QVERIFY(!color->isEnabled());
    for (const auto &call : calls()) QVERIFY(call.toArray().at(1).toString() != "set");
}
void TestMachinesPage::colorPickerDismissalKeepsTheSavedColor_data()
{
    QTest::addColumn<QString>("action");
    for (const auto &action : {"outside-left", "outside-right", "escape", "cancel"}) QTest::newRow(action) << QString(action);
}
void TestMachinesPage::colorPickerDismissalKeepsTheSavedColor()
{
    QFETCH(QString, action);
    MachinesPage page(FakeCli::program(m_script)); start(page); page.resize(1100,760);QVERIFY(QTest::qWaitForWindowExposed(&page));
    const auto original=MachineAppearance::color("arch");
    QSignalSpy changed(&page,&MachinesPage::appearanceChanged);
    auto *outside=widget<QPushButton>(page,"addMachine");QSignalSpy outsideClicked(outside,&QPushButton::clicked);
    bool expired=false,handled=false;
    QTimer::singleShot(0,&page,[&]{
        auto *dialog=page.findChild<QColorDialog *>();QVERIFY(dialog);
        QTimer::singleShot(5000,dialog,[&,dialog]{expired=true;dialog->reject();});
        dialog->setCurrentColor(QColor("#e05999"));
        QTest::mouseClick(dialog,Qt::LeftButton,Qt::NoModifier,QPoint(8,8));QVERIFY(dialog->isVisible());
        if(action.startsWith("outside")){
            const auto global=outside->mapToGlobal(outside->rect().center());QVERIFY(!dialog->frameGeometry().contains(global));
            QVERIFY(PopupMouse::click(&page,action=="outside-right"?Qt::RightButton:Qt::LeftButton,page.mapFromGlobal(global)));
        }else if(action=="escape")QTest::keyClick(dialog,Qt::Key_Escape);
        else dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
        QTRY_VERIFY(!dialog->isVisible());handled=true;
    });
    widget<QPushButton>(page,"machineColor")->click();
    QVERIFY(handled);QVERIFY(!expired);QCoreApplication::processEvents();
    QCOMPARE(MachineAppearance::color("arch"),original);QCOMPARE(changed.size(),0);QCOMPARE(outsideClicked.size(),0);
    QVERIFY(!QSettings().contains("machines/colors/arch"));
    QCOMPARE(widget<QLineEdit>(page,"machineAlias")->text(),QString("arch"));
    QVERIFY(!widget<QPushButton>(page,"saveMachine")->isEnabled());
    for(const auto &call:calls())QVERIFY(call.toArray().at(1).toString()!="set");
    if(action.startsWith("outside")){
        QVERIFY(PopupMouse::click(&page,Qt::LeftButton,outside->mapTo(&page,outside->rect().center())));
        QTRY_COMPARE(outsideClicked.size(),1); // The native stimulus reaches the owner after dismissal.
    }
}
void TestMachinesPage::dashboardSelectionWaitsForProfiles()
{
    flag("list-delay"); MachinesPage page(FakeCli::program(m_script));
    BoxState local; local.host = "arch"; local.ok = true; FleetState fleet; fleet.setLocal(local, 0); page.setFleet(fleet);
    page.show(); page.selectMachine("mac");
    QTRY_COMPARE(widget<QLineEdit>(page, "machineAlias")->text(), "mac");
    QCOMPARE(widget<QListWidget>(page, "machineProfiles")->currentRow(), 1);
    page.selectMachine("arch"); QCOMPARE(widget<QListWidget>(page, "machineProfiles")->currentRow(), 0);
    page.selectMachine("mac"); page.selectMachine({}); QCOMPARE(widget<QListWidget>(page, "machineProfiles")->currentRow(), 0);
}
void TestMachinesPage::preview()
{
    const auto directory = qEnvironmentVariable("HGS_PREVIEW_DIR");
    if (directory.isEmpty()) QSKIP("Set HGS_PREVIEW_DIR to render the machines preview");
    QDir().mkpath(directory);
    MachinesPage page(FakeCli::program(m_script)); start(page); page.resize(1000, 680); page.setTheme(true);
    page.setStyleSheet("QWidget#machinesPage { background:#171e24; color:#e8edf4; } "
        "QLabel,QCheckBox { color:#e8edf4; } QLabel#muted { color:#a1adbb; } QLabel#heading { font-size:20px; } "
        "QLineEdit,QSpinBox,QListWidget,QPlainTextEdit { background:#1e262e; color:#e8edf4; border:1px solid #34414d; border-radius:6px; padding:6px; } "
        "QLineEdit:disabled,QSpinBox:disabled { color:#647386; } QListWidget::item { padding:10px; } "
        "QListWidget::item:selected { background:#233a35; } QPushButton { color:#e8edf4; background:#1e262e; border:1px solid #34414d; border-radius:6px; padding:7px 10px; } "
        "QPushButton:disabled { color:#647386; } QPushButton#saveMachine:enabled { background:#89d9bf; color:#142821; }");
    widget<QListWidget>(page, "machineProfiles")->setCurrentRow(1);
    MachineAppearance::setColor("mac", QColor("#ffda19"));
    widget<QComboBox>(page, "machineLabelStyle")->setCurrentIndex(1);
    widget<QLineEdit>(page, "machineHostname")->setText("studio-mac.local");
    widget<QLineEdit>(page, "machineUser")->setText("example");
    widget<QLineEdit>(page, "machineKey")->setText("~/.ssh/id_ed25519");
    QTest::qWait(30); QVERIFY(page.grab().save(directory + "/machines-dark.png"));
}
void TestMachinesPage::controlOnlyComputerIsAConsoleWithoutSessions()
{
    MachinesPage page(FakeCli::program(m_script)); start(page);
    QVERIFY(widget<QPushButton>(page, "machineSessions")->isVisible());
    BoxState local; local.host = "arch"; local.ok = true; local.localSessions = false;
    FleetState fleet; fleet.setLocal(local, 0); page.setFleet(fleet);
    auto *list = widget<QListWidget>(page, "machineProfiles");
    QCOMPARE(list->item(0)->text(), QString("arch\nThis computer: control console"));
    QVERIFY(!widget<QPushButton>(page, "machineSessions")->isVisible());
    QVERIFY(!widget<QPushButton>(page, "machineAccounts")->isVisible());
    bool console = false, counted = false;
    for (auto *label : page.findChildren<QLabel *>("muted")) {
        console |= label->text().contains("control console");
        // As on Overview, the console itself is not a machine of the fleet.
        counted |= label->text() == "0 connected, 1 machine";
    }
    QVERIFY(console); QVERIFY(counted);
    // It has no SSH connection to edit, test or remove; only its color.
    QVERIFY(!widget<QLineEdit>(page, "machineAlias")->isVisible());
    QVERIFY(!widget<QLineEdit>(page, "machineHostname")->isVisible());
    QVERIFY(!widget<QCheckBox>(page, "machineEnabled")->isVisible());
    for (const char *name : {"saveMachine", "checkMachine", "sshMachine", "installMachine", "resetMachine", "removeMachine"})
        QVERIFY2(!widget<QPushButton>(page, name)->isVisible(), name);
    QVERIFY(widget<QPushButton>(page, "machineColor")->isVisible());
    // Remote machines keep every action.
    list->setCurrentRow(1);
    QTRY_VERIFY(widget<QPushButton>(page, "machineSessions")->isVisible());
    QVERIFY(widget<QPushButton>(page, "machineAccounts")->isVisible());
    QVERIFY(widget<QLineEdit>(page, "machineHostname")->isVisible());
    QVERIFY(widget<QCheckBox>(page, "machineEnabled")->isVisible());
    for (const char *name : {"saveMachine", "checkMachine", "sshMachine", "installMachine", "removeMachine"})
        QVERIFY2(widget<QPushButton>(page, name)->isVisible(), name);
}

namespace {
// This computer as a Windows CLI reports it (spec D2): a control console until
// local sessions are enabled, with its session host.
FleetState windowsComputer(bool enabled, const QJsonObject &host, const QString &platform = "windows")
{
    BoxState local; local.host = "windows-pc"; local.ok = true; local.localSessions = enabled; local.localHost = host;
    if (!platform.isEmpty()) local.metrics = {{"platform", platform}};
    FleetState fleet; fleet.setLocal(local, 0); return fleet;
}
QJsonObject sessionHost(const QString &state)
{
    return {{"state", state}, {"pid", QJsonValue::Null}, {"protocol", QJsonValue::Null}, {"conpty", QJsonValue::Null},
            {"integrity", QJsonValue::Null}, {"windows_session", QJsonValue::Null}};
}
}

void TestMachinesPage::countsReadInSingularAndPlural()
{
    MachinesPage page(FakeCli::program(m_script)); start(page);
    auto *list = widget<QListWidget>(page, "machineProfiles");
    const auto summary = [&] {
        QStringList texts; for (auto *label : page.findChildren<QLabel *>("muted")) texts << label->text(); return texts;
    };
    QVERIFY2(summary().contains("1 connected, 2 machines"), qPrintable(summary().join(" | ")));
    QCOMPARE(list->item(1)->text(), QString("mac\nNot connected"));
    BoxState local; local.host = "arch"; local.ok = true;
    BoxState mac; mac.host = "mac"; mac.ok = true; mac.sessions.resize(1);
    FleetState fleet; fleet.setLocal(local, 0); fleet.setPeer(mac, QDateTime::currentMSecsSinceEpoch()); page.setFleet(fleet);
    QVERIFY2(summary().contains("2 connected, 2 machines"), qPrintable(summary().join(" | ")));
    QCOMPARE(list->item(1)->text(), QString("mac\nConnected, 1 session"));
    mac.sessions.resize(2); fleet.setPeer(mac, QDateTime::currentMSecsSinceEpoch()); page.setFleet(fleet);
    QCOMPARE(list->item(1)->text(), QString("mac\nConnected, 2 sessions"));
    // A control console leaves one machine of the fleet.
    local.localSessions = false; fleet.setLocal(local, 0); page.setFleet(fleet);
    QVERIFY2(summary().contains("1 connected, 1 machine"), qPrintable(summary().join(" | ")));
}
void TestMachinesPage::windowsConsoleEnablesLocalSessions()
{
    MachinesPage page(FakeCli::program(m_script)); start(page);
    page.setFleet(windowsComputer(false, sessionHost("not-installed")));
    auto *enable = widget<QPushButton>(page, "enableLocalSessions"); auto *notice = widget<QLabel>(page, "machineNotice");
    QVERIFY(enable->isVisible()); QVERIFY(enable->isEnabled()); QCOMPARE(enable->text(), QString("Enable local sessions"));
    QSignalSpy changed(&page, &MachinesPage::machinesChanged);
    const auto installs = [&] {
        int count = 0; for (const auto &call : calls()) count += call.toArray().at(0) == "session-host";
        return count;
    };
    // Enabling writes into the profile: it asks first, and Cancel is the default.
    const auto answer = [&](bool proceed) {
        bool shown = false; QTimer respond; respond.setInterval(5);
        connect(&respond, &QTimer::timeout, &page, [&] {
            auto *box = page.findChild<QMessageBox *>("enableLocalSessionsConfirm"); if (!box || !box->isVisible()) return;
            respond.stop(); shown = true;
            QCOMPARE(box->defaultButton(), qobject_cast<QPushButton *>(box->button(QMessageBox::Cancel)));
            QVERIFY(box->text().contains("hgs session-host uninstall")); QVERIFY(!box->text().contains("preview", Qt::CaseInsensitive));
            if (!proceed) { QTest::keyClick(box, Qt::Key_Escape); return; }
            for (auto *button : box->buttons()) if (box->buttonRole(button) == QMessageBox::AcceptRole) button->click();
        });
        respond.start(); enable->click(); respond.stop(); QVERIFY(shown);
    };
    answer(false); QTest::qWait(100); QCOMPARE(installs(), 0); QVERIFY(changed.isEmpty());
    // A failure is reported and changes nothing.
    flag("host-mode", "fail"); answer(true);
    QVERIFY(!enable->isEnabled()); QVERIFY(!widget<QListWidget>(page, "machineProfiles")->isEnabled());
    QTRY_VERIFY(notice->text().contains("access denied")); QVERIFY(notice->styleSheet().contains("color"));
    QVERIFY(enable->isEnabled()); QCOMPARE(installs(), 1); QVERIFY(changed.isEmpty());
    QFile::remove(m_dir.filePath("host-mode")); answer(true);
    QTRY_COMPARE(changed.size(), 1); QCOMPARE(installs(), 2);
    QVERIFY(notice->text().contains("Local sessions are enabled")); QVERIFY(notice->text().contains("next start"));
    QVERIFY(notice->text().contains("C:\\Users\\user\\.local\\bin\\hgs.exe")); QVERIFY(notice->styleSheet().isEmpty());
    for (const auto &call : calls())
        if (call.toArray().at(0) == "session-host") QCOMPARE(call.toArray(), QJsonArray({"session-host", "install", "--json"}));
    // The next poll reports an enabled computer: no second offer, and the notice
    // that the stable CLI follows from the next start stays.
    page.setFleet(windowsComputer(true, sessionHost("stopped")));
    QVERIFY(!enable->isVisible()); QVERIFY(widget<QPushButton>(page, "machineSessions")->isVisible());
    QVERIFY(notice->text().contains("next start"));
    widget<QListWidget>(page, "machineProfiles")->setCurrentRow(1); QTRY_VERIFY(!notice->text().contains("next start"));
}

void TestMachinesPage::enablingIsOfferedOnlyToAWindowsConsole()
{
    MachinesPage page(FakeCli::program(m_script)); start(page);
    auto *enable = widget<QPushButton>(page, "enableLocalSessions"); auto *host = widget<QLabel>(page, "localSessionHost");
    const auto origin = [&] {
        for (auto *label : page.findChildren<QLabel *>("muted"))
            if (label->isVisible() && label->text().startsWith("This computer is a control console")) return label->text();
        return QString();
    };
    const QString remote("This computer is a control console: agents run on the remote machines you add here, over SSH.");
    // Linux, macOS and an older Windows CLI without local_host offer nothing.
    QVERIFY(!enable->isVisible()); QVERIFY(!host->isVisible());
    page.setFleet(windowsComputer(false, sessionHost("not-installed"), "linux"));
    QVERIFY(!enable->isVisible()); QVERIFY(!host->isVisible()); QCOMPARE(origin(), remote);
    page.setFleet(windowsComputer(false, sessionHost("not-installed"), "macos"));
    QVERIFY(!enable->isVisible()); QVERIFY(!host->isVisible()); QCOMPARE(origin(), remote);
    page.setFleet(windowsComputer(false, {}));
    QVERIFY(!enable->isVisible()); QVERIFY(!host->isVisible()); QCOMPARE(origin(), remote);
    // Before the first answer names the platform, this GUI's own platform counts.
    page.setFleet(windowsComputer(false, sessionHost("not-installed"), {}));
    QCOMPARE(enable->isVisible(), MachinePlatform::build() == MachinePlatform::Style::Windows);
    page.setFleet(windowsComputer(false, sessionHost("not-installed")));
    QVERIFY(enable->isVisible()); QVERIFY(host->isVisible());
    // The offer is named where the console explains where agents run.
    QCOMPARE(origin(), remote + " Enable local sessions to run them on this computer too.");
    page.setFleet(windowsComputer(false, {})); QCOMPARE(origin(), remote);
    page.setFleet(windowsComputer(false, sessionHost("not-installed")));
    // An enabled computer shows its host without the offer; a remote machine neither.
    page.setFleet(windowsComputer(true, sessionHost("stopped")));
    QVERIFY(!enable->isVisible()); QVERIFY(host->isVisible());
    page.setFleet(windowsComputer(false, sessionHost("not-installed")));
    widget<QListWidget>(page, "machineProfiles")->setCurrentRow(1);
    QTRY_VERIFY(widget<QPushButton>(page, "machineSessions")->isVisible());
    QVERIFY(!enable->isVisible()); QVERIFY(!host->isVisible());
    // A Unix computer's page keeps its actions.
    page.setFleet(windowsComputer(true, {}, "linux")); widget<QListWidget>(page, "machineProfiles")->setCurrentRow(0);
    QVERIFY(!enable->isVisible()); QVERIFY(!host->isVisible());
    QVERIFY(widget<QPushButton>(page, "machineSessions")->isVisible()); QVERIFY(widget<QPushButton>(page, "machineAccounts")->isVisible());
}

void TestMachinesPage::localSessionHostStates_data()
{
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<QJsonObject>("host");
    QTest::addColumn<QString>("text");
    QTest::newRow("not installed") << false << sessionHost("not-installed") << "Session host: not installed";
    QTest::newRow("stopped") << true << sessionHost("stopped") << "Session host: stopped";
    QTest::newRow("running") << true << QJsonObject{{"state", "running"}, {"pid", 4242}, {"protocol", 1}, {"conpty", "bundled"},
        {"integrity", "medium"}, {"windows_session", 1}}
        << "Session host: running, PID 4242, bundled ConPTY, medium integrity, Windows session 1";
    QTest::newRow("inbox") << true << QJsonObject{{"state", "running"}, {"pid", 77}, {"protocol", 1}, {"conpty", "inbox"},
        {"integrity", "high"}, {"windows_session", 0}}
        << "Session host: running, PID 77, inbox ConPTY, high integrity, Windows session 0";
    auto unavailable = sessionHost("unavailable"); unavailable["error"] = "the session host on this computer runs elevated";
    QTest::newRow("unavailable") << true << unavailable << "Session host: unavailable: the session host on this computer runs elevated";
    QTest::newRow("console unavailable") << false << unavailable << "Session host: unavailable: the session host on this computer runs elevated";
}

void TestMachinesPage::localSessionHostStates()
{
    QFETCH(bool, enabled); QFETCH(QJsonObject, host); QFETCH(QString, text);
    MachinesPage page(FakeCli::program(m_script)); start(page);
    page.setFleet(windowsComputer(enabled, host));
    auto *label = widget<QLabel>(page, "localSessionHost");
    QVERIFY(label->isVisible()); QCOMPARE(label->text(), text);
    QCOMPARE(widget<QPushButton>(page, "enableLocalSessions")->isVisible(), !enabled);
    // Each poll shows the host as found.
    page.setFleet(windowsComputer(enabled, sessionHost("stopped"))); QCOMPARE(label->text(), QString("Session host: stopped"));
}

QTEST_MAIN(TestMachinesPage)
#include "test_machinespage.moc"
