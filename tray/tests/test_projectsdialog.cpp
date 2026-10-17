#include <QtTest>
#include "ProjectsDialog.h"
#include "PopupMouse.h"
#include "FakeCli.h"
#include <QInputDialog>
#include <QColorDialog>
#include <QCheckBox>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>
#include <QHeaderView>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QLineEdit>
#include <functional>
#include <QTimer>

class TestProjectsDialog : public QObject {
    Q_OBJECT
private slots:
    void worktreeOverviewIsVisibleWithoutSelectingAProjectFolder();
    void projectColorChangesRequireApply_data();
    void projectColorChangesRequireApply();
    void customProjectColorRemainsStaged_data();
    void customProjectColorRemainsStaged();
    void emptyListContextCreatesProject();
    void sortingKeepsFolderIdentityAndLaunchTarget();
    void folderTransfersKeepMenuTargetsAndPersist_data();
    void folderTransfersKeepMenuTargetsAndPersist();
    void folderTransferMenusRejectStaleChanges_data();
    void folderTransferMenusRejectStaleChanges();
    void folderTransferMenusRespectVisibleProjects();
    void folderSplitPrioritizesListAndRemembersAdjustment();
    void folderDialogTakesDriveFoldersOnlyOnLocalWindows_data();
    void folderDialogTakesDriveFoldersOnlyOnLocalWindows();
    void legacyAliasesOfALocalWindowsComputerBecomeFolders();
    void legacyImportReportsAliasesItCannotAdd();
    void acceptsPlainName();
    void rejectsSlashAndSpace();
    void rejectsEmpty();
    void warnsOnDotOrColon();
    void nameDefaultsToBasename();
};

void TestProjectsDialog::projectColorChangesRequireApply_data()
{
    QTest::addColumn<QString>("action");
    for(const auto &action:{"outside","escape","cancel","apply"})QTest::newRow(action)<<QString(action);
}
void TestProjectsDialog::projectColorChangesRequireApply()
{
    QFETCH(QString,action);
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
    QCoreApplication::setOrganizationName("hgs-project-color-test");QCoreApplication::setApplicationName("projects");
    SessionOrganization projects;const auto id=projects.createGroup("Work");
    const auto original=projects.group(id)->color;const bool vivid=projects.group(id)->vivid;
    HgsClient client("/nonexistent/hgs");ProjectsDialog view(&client,nullptr,false,&projects);view.resize(900,650);view.selectProject(id);view.show();QVERIFY(QTest::qWaitForWindowExposed(&view));
    QSignalSpy changed(&view,&ProjectsDialog::organizationChanged);bool expired=false,handled=false;
    QTimer::singleShot(0,&view,[&]{
        auto *dialog=view.findChild<QDialog *>("projectColorDialog");QVERIFY(dialog);
        QTimer::singleShot(5000,dialog,[&,dialog]{expired=true;dialog->reject();});
        dialog->findChild<QLineEdit *>("projectColorHex")->setText("#e05999");
        dialog->findChild<QComboBox *>("projectColorStyle")->setCurrentIndex(vivid?0:1);
        QCOMPARE(projects.group(id)->color,original);QCOMPARE(changed.size(),0);
        auto *buttons=dialog->findChild<QDialogButtonBox *>();
        if(action=="outside"){
            const QPoint point(view.width()-8,view.height()-8);QVERIFY(!dialog->frameGeometry().contains(view.mapToGlobal(point)));
            QVERIFY(PopupMouse::click(&view,Qt::LeftButton,point));
        }else if(action=="escape")QTest::keyClick(dialog,Qt::Key_Escape);
        else buttons->button(action=="apply"?QDialogButtonBox::Apply:QDialogButtonBox::Cancel)->click();
        QTRY_VERIFY(!dialog->isVisible());handled=true;
    });
    view.findChild<QPushButton *>("projectColor")->click();QVERIFY(handled);QVERIFY(!expired);
    QCOMPARE(projects.group(id)->color,action=="apply"?QString("#e05999"):original);
    QCOMPARE(projects.group(id)->vivid,action=="apply"?!vivid:vivid);QCOMPARE(changed.size(),action=="apply"?1:0);
    QVERIFY(view.isVisible());
}

void TestProjectsDialog::customProjectColorRemainsStaged_data()
{
    QTest::addColumn<bool>("acceptPicker");QTest::addColumn<bool>("applyProject");
    QTest::newRow("outside-picker-then-apply")<<false<<true;
    QTest::newRow("accept-picker-then-cancel")<<true<<false;
    QTest::newRow("accept-picker-then-apply")<<true<<true;
}
void TestProjectsDialog::customProjectColorRemainsStaged()
{
    QFETCH(bool,acceptPicker);QFETCH(bool,applyProject);
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
    QCoreApplication::setOrganizationName("hgs-project-color-test");QCoreApplication::setApplicationName("projects");
    SessionOrganization projects;const auto id=projects.createGroup("Work");const auto original=projects.group(id)->color;
    HgsClient client("/nonexistent/hgs");ProjectsDialog view(&client,nullptr,false,&projects);view.resize(900,650);view.selectProject(id);view.show();QVERIFY(QTest::qWaitForWindowExposed(&view));
    QSignalSpy changed(&view,&ProjectsDialog::organizationChanged);bool expired=false,handled=false;
    QTimer::singleShot(0,&view,[&]{
        auto *dialog=view.findChild<QDialog *>("projectColorDialog");QVERIFY(dialog);
        QTimer::singleShot(10000,dialog,[&,dialog]{expired=true;dialog->reject();});
        auto *hex=dialog->findChild<QLineEdit *>("projectColorHex");hex->setText("#2299aa");
        QTimer::singleShot(0,dialog,[&,dialog]{
            auto *picker=dialog->findChild<QColorDialog *>();QVERIFY(picker);
            QTimer::singleShot(5000,picker,[&,picker]{expired=true;picker->reject();});
            picker->setCurrentColor(QColor("#ee55aa"));
            if(acceptPicker)picker->accept();
            else{
                picker->move(dialog->frameGeometry().right()+15,dialog->frameGeometry().top());
                const QPoint point(8,8);QVERIFY(!picker->frameGeometry().contains(dialog->mapToGlobal(point)));
                QVERIFY(PopupMouse::click(dialog,Qt::LeftButton,point));
            }
            QTRY_VERIFY(!picker->isVisible());
        });
        dialog->findChild<QPushButton *>("projectCustomColor")->click();QVERIFY(!expired);QVERIFY(dialog->isVisible());
        QCOMPARE(hex->text(),acceptPicker?QString("#ee55aa"):QString("#2299aa"));
        QCOMPARE(projects.group(id)->color,original);QCOMPARE(changed.size(),0);
        dialog->findChild<QDialogButtonBox *>()->button(applyProject?QDialogButtonBox::Apply:QDialogButtonBox::Cancel)->click();handled=true;
    });
    view.findChild<QPushButton *>("projectColor")->click();QVERIFY(handled);QVERIFY(!expired);
    QCOMPARE(projects.group(id)->color,applyProject?(acceptPicker?QString("#ee55aa"):QString("#2299aa")):original);
    QCOMPARE(changed.size(),applyProject?1:0);
}

void TestProjectsDialog::worktreeOverviewIsVisibleWithoutSelectingAProjectFolder()
{
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
    QCoreApplication::setOrganizationName("hgs-project-worktree-test");QCoreApplication::setApplicationName("projects");
    SessionOrganization projects;HgsClient client("/nonexistent/hgs");ProjectsDialog dialog(&client,nullptr,false,&projects);
    dialog.resize(760,560);dialog.show();auto *button=dialog.findChild<QPushButton *>("projectWorktrees");QVERIFY(button);QVERIFY(button->isVisible());QVERIFY(button->isEnabled());
    bool opened=false;QTimer::singleShot(0,&dialog,[&]{auto *overview=dialog.findChild<QDialog *>("worktreesDialog");QVERIFY(overview);
        QVERIFY(overview->findChild<QLabel *>("worktreeOverviewStatus")->text().contains("Add a project folder"));opened=true;overview->reject();});
    button->click();QVERIFY(opened);
}

static QMenu *folderMenu(ProjectsDialog &dialog, const QString &id)
{
    auto *table = dialog.findChild<QTableWidget *>("projectFolders");
    for (int row = 0; row < table->rowCount(); ++row) if (table->item(row, 0)->data(Qt::UserRole).toString() == id) {
        table->customContextMenuRequested(table->visualItemRect(table->item(row, 0)).center());
        return dialog.findChild<QMenu *>("projectFolderMenu");
    }
    return nullptr;
}

static QAction *folderTarget(QMenu *menu, const QString &destination, bool copy)
{
    auto *targets = menu->findChild<QMenu *>(copy ? "copyProjectFolderTo" : "moveProjectFolderTo");
    if (targets) for (auto *action : targets->actions()) if (action->data().toString() == destination) return action;
    return nullptr;
}

void TestProjectsDialog::folderTransfersKeepMenuTargetsAndPersist_data()
{
    QTest::addColumn<bool>("copy"); QTest::addColumn<QString>("machine");
    QTest::newRow("move-local") << false << QString("local");
    QTest::newRow("copy-local") << true << QString("local");
    QTest::newRow("move-offline-remote") << false << QString("remote");
    QTest::newRow("copy-offline-remote") << true << QString("remote");
}

void TestProjectsDialog::folderTransfersKeepMenuTargetsAndPersist()
{
    QFETCH(bool, copy); QFETCH(QString, machine);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setOrganizationName("hgs-folder-transfer-test"); QCoreApplication::setApplicationName("projects");
#ifdef Q_OS_WIN
    // Project folders are POSIX paths on agent machines; a temporary folder of this
    // Windows computer cannot be one, so the files check below is Unix-only.
    const QString path = "/workspace/plain folder";
#else
    const auto path = directory.path() + "/plain folder"; QVERIFY(QDir().mkpath(path));
#endif
    SessionOrganization projects; const auto source = projects.createGroup("Source"), target = projects.createGroup("Work & docs");
    const auto original = projects.addFolder(source, machine, path, "Transfer this");
    const auto keep = projects.addFolder(source, machine, "/workspace/keep", "Keep this");
    projects.moveSession(projects.observe(machine, "agent/session", "run-1"), source);
    QSettings().setValue("workspace/organization", QJsonDocument(projects.toJson()).toJson(QJsonDocument::Compact));
    HgsClient client("/nonexistent/hgs"); ProjectsDialog dialog(&client); dialog.selectProject(source); dialog.show();
    auto *menu = folderMenu(dialog, original); QVERIFY(menu);
    QVERIFY(!folderTarget(menu, source, copy));
    auto *action = folderTarget(menu, target, copy); QVERIFY(action); QCOMPARE(action->text(), QString("Work && docs"));
    // A later selection and project navigation must not retarget the open menu.
    auto *table = dialog.findChild<QTableWidget *>("projectFolders");
    for (int row = 0; row < table->rowCount(); ++row)
        if (table->item(row, 0)->data(Qt::UserRole).toString() == keep) table->setCurrentCell(row, 0);
    dialog.selectProject(target);
    QSignalSpy changed(&dialog, &ProjectsDialog::organizationChanged); action->trigger(); QCOMPARE(changed.size(), 1);
    const SessionOrganization saved(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QCOMPARE(saved.group(source)->folders.size(), copy ? 2 : 1); QCOMPARE(saved.group(target)->folders.size(), 1);
    const auto transferred = saved.group(target)->folders.first();
    QCOMPARE(transferred.machine, machine); QCOMPARE(transferred.path, path); QCOMPARE(transferred.name, QString("Transfer this"));
    if (copy) QVERIFY(transferred.id != original); else QCOMPARE(transferred.id, original);
    QCOMPARE(saved.groupFor(machine + "\nagent/session"), source);
#ifndef Q_OS_WIN
    QVERIFY(QDir(path).exists());
#endif
    QCOMPARE(table->rowCount(), 1); QCOMPARE(table->item(0, 0)->data(Qt::UserRole).toString(), transferred.id);
    delete menu;
}

void TestProjectsDialog::folderTransferMenusRejectStaleChanges_data()
{
    QTest::addColumn<QString>("change"); QTest::addColumn<bool>("copy");
    for (const auto &change : {"edited-folder", "removed-folder", "removed-source", "removed-destination", "hidden-destination"}) {
        QTest::newRow((QByteArray("move-") + change).constData()) << QString(change) << false;
        QTest::newRow((QByteArray("copy-") + change).constData()) << QString(change) << true;
    }
}

void TestProjectsDialog::folderTransferMenusRejectStaleChanges()
{
    QFETCH(QString, change); QFETCH(bool, copy);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setOrganizationName("hgs-folder-stale-test"); QCoreApplication::setApplicationName("projects");
    SessionOrganization projects; const auto source = projects.createGroup("Source"), target = projects.createGroup("Target");
    const auto original = projects.addFolder(source, "offline", "/workspace/folder", "Original");
    HgsClient client("/nonexistent/hgs"); ProjectsDialog dialog(&client, nullptr, false, &projects);
    dialog.selectProject(source); dialog.show(); auto *menu = folderMenu(dialog, original); QVERIFY(menu);
    auto *action = folderTarget(menu, target, copy); QVERIFY(action);
    if (change == "edited-folder") QVERIFY(projects.editFolder(source, original, "offline", "/workspace/changed", "Changed"));
    else if (change == "removed-folder") projects.removeFolder(source, original);
    else if (change == "removed-source") QVERIFY(projects.removeGroup(source));
    else if (change == "removed-destination") QVERIFY(projects.removeGroup(target));
    else {
        auto data = projects.toJson(); auto groups = data["projects"].toArray();
        for (int i = 0; i < groups.size(); ++i) {
            auto group = groups[i].toObject();
            if (group["id"] == target) { group["accessible"] = false; groups[i] = group; }
        }
        data["projects"] = groups; projects = SessionOrganization(data);
    }
    const auto before = projects.toJson(); QSignalSpy changed(&dialog, &ProjectsDialog::organizationChanged);
    action->trigger(); QCOMPARE(changed.size(), 0); QCOMPARE(projects.toJson(), before); delete menu;
}

void TestProjectsDialog::folderTransferMenusRespectVisibleProjects()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setOrganizationName("hgs-folder-targets-test"); QCoreApplication::setApplicationName("projects");
    QJsonObject source{{"id", "source"}, {"name", "Source"}, {"folders", QJsonArray{QJsonObject{
        {"id", "folder"}, {"machine", "offline"}, {"path", "/workspace/folder"}}}}};
    const QJsonObject hidden{{"id", "hidden"}, {"name", "Hidden"}, {"accessible", false}};
    SessionOrganization projects({{"default_project", "source"}, {"projects", QJsonArray{source, hidden}}});
    HgsClient client("/nonexistent/hgs"); ProjectsDialog dialog(&client, nullptr, false, &projects);
    dialog.selectProject("source"); dialog.show(); auto *menu = folderMenu(dialog, "folder"); QVERIFY(menu);
    QVERIFY(!menu->findChild<QMenu *>("moveProjectFolderTo")->isEnabled());
    QVERIFY(!menu->findChild<QMenu *>("copyProjectFolderTo")->isEnabled()); delete menu;
    dialog.findChild<QCheckBox *>("allSwarmProjects")->setChecked(true);
    menu = folderMenu(dialog, "folder"); QVERIFY(menu); QVERIFY(folderTarget(menu, "hidden", false));
    folderTarget(menu, "hidden", true)->trigger(); QCOMPARE(projects.group("source")->folders.size(), 1);
    QCOMPARE(projects.group("hidden")->folders.size(), 1);
    // Repeating Copy reuses the target and does not publish a redundant catalog edit.
    QSignalSpy changed(&dialog, &ProjectsDialog::organizationChanged);
    folderTarget(menu, "hidden", true)->trigger(); QCOMPARE(changed.size(), 0);
    folderTarget(menu, "hidden", false)->trigger(); QCOMPARE(changed.size(), 1);
    QVERIFY(projects.group("source")->folders.isEmpty()); QCOMPARE(projects.group("hidden")->folders.size(), 1);
    delete menu;
}

void TestProjectsDialog::sortingKeepsFolderIdentityAndLaunchTarget()
{
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
    QCoreApplication::setOrganizationName("hgs-folder-sort-test");QCoreApplication::setApplicationName("projects");
    HgsClient client("/nonexistent/hgs");SessionOrganization projects;const auto project=projects.createGroup("Work");
    const auto zulu=projects.addFolder(project,"arch","/workspace/zulu","Zulu");
    const auto beta=projects.addFolder(project,"mac","/workspace/beta","Beta");
    projects.addFolder(project,"arch","/workspace/alpha");
    const auto otherBeta=projects.addFolder(project,"arch","/workspace/other","beta");
    projects.markImported("arch");projects.markImported("mac");const auto original=projects.toJson();
    // POSIX folders on every GUI: a Windows GUI would show its own machine's natively.
    BoxState local;local.host="arch";local.ok=true;local.metrics["platform"]="linux";BoxState remote;remote.host="mac";remote.ok=true;
    FleetState fleet;fleet.setLocal(local,QDateTime::currentMSecsSinceEpoch());fleet.setPeer(remote,QDateTime::currentMSecsSinceEpoch());
    ProjectsDialog dialog(&client,nullptr,false,&projects);dialog.setFleet(fleet);dialog.selectProject(project);
    auto *table=dialog.findChild<QTableWidget *>("projectFolders");QVERIFY(table);QCOMPARE(table->rowCount(),4);
    QCOMPARE(table->item(0,0)->text(),QString("alpha"));QCOMPARE(table->item(3,0)->text(),QString("Zulu"));
    for(int row=0;row<table->rowCount();++row) {
        const auto id=table->item(row,0)->data(Qt::UserRole).toString();
        const auto folders=projects.group(project)->folders;
        const auto folder=std::find_if(folders.cbegin(),folders.cend(),[&](const auto &f){return f.id==id;});
        QVERIFY(folder!=folders.cend());QCOMPARE(table->item(row,1)->text(),folder->machine);QCOMPARE(table->item(row,2)->text(),folder->path);
        if(id==beta)table->setCurrentCell(row,0);
    }
    table->sortItems(0,Qt::DescendingOrder);dialog.refresh();
    QCOMPARE(table->item(table->currentRow(),0)->data(Qt::UserRole).toString(),beta);
    QCOMPARE(table->item(0,0)->data(Qt::UserRole).toString(),zulu);
    QCOMPARE(projects.toJson(),original);
    QVERIFY(projects.editFolder(project,beta,"mac","/workspace/beta","aardvark"));dialog.refresh();
    QCOMPARE(table->item(table->currentRow(),0)->data(Qt::UserRole).toString(),beta);
    QSignalSpy launch(&dialog,&ProjectsDialog::newSessionRequested);
    dialog.findChild<QPushButton *>("newProjectFolderSession")->click();QCOMPARE(launch.size(),1);
    QCOMPARE(launch[0],QVariantList({project,QString("mac"),beta}));
    table->sortItems(1,Qt::AscendingOrder);dialog.refresh();
    QCOMPARE(table->horizontalHeader()->sortIndicatorSection(),1);
    QCOMPARE(table->item(table->currentRow(),0)->data(Qt::UserRole).toString(),beta);
    projects.removeFolder(project,otherBeta);dialog.refresh();QCOMPARE(table->rowCount(),3);
    QCOMPARE(table->item(table->currentRow(),0)->data(Qt::UserRole).toString(),beta);
}

void TestProjectsDialog::folderSplitPrioritizesListAndRemembersAdjustment()
{
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
    QCoreApplication::setOrganizationName("hgs-folder-split-test");QCoreApplication::setApplicationName("projects");
    HgsClient client("/nonexistent/hgs");SessionOrganization projects;const auto project=projects.createGroup("Work");
    projects.addFolder(project,"arch","/workspace/repo");
    ProjectsDialog dialog(&client,nullptr,true,&projects);dialog.resize(1100,1000);dialog.selectProject(project);dialog.show();
    auto *table=dialog.findChild<QTableWidget *>("projectFolders");auto *split=dialog.findChild<QSplitter *>("projectFolderSplitter");QVERIFY(split);
    table->setCurrentCell(0,0);QTRY_VERIFY(split->widget(1)->isVisible());QTest::qWait(30);
    QVERIFY(table->height()>230);QVERIFY(split->sizes()[0]>split->sizes()[1]);
    const auto initial=split->sizes();
    auto *handle=split->handle(1);const auto center=handle->rect().center();
    QTest::mousePress(handle,Qt::LeftButton,Qt::NoModifier,center);
    QTest::mouseMove(handle,center-QPoint(0,80));QTest::mouseRelease(handle,Qt::LeftButton,Qt::NoModifier,center);
    QTRY_VERIFY(split->sizes()[0]<initial[0]);
    const auto adjusted=split->sizes();QVERIFY(!QSettings().value("workspace/projectFolderSplit").toByteArray().isEmpty());
    dialog.refresh();QCOMPARE(split->sizes(),adjusted);
    table->clearSelection();QTRY_VERIFY(!split->widget(1)->isVisible());table->setCurrentCell(0,0);QTest::qWait(30);
    QVERIFY(qAbs(split->sizes()[1]-adjusted[1])<4);
    ProjectsDialog reopened(&client,nullptr,true,&projects);reopened.resize(dialog.size());reopened.selectProject(project);reopened.show();
    reopened.findChild<QTableWidget *>("projectFolders")->setCurrentCell(0,0);QTest::qWait(30);
    const auto restored=reopened.findChild<QSplitter *>("projectFolderSplitter")->sizes();
    QVERIFY(qAbs(restored[1]-adjusted[1])<4);
}

void TestProjectsDialog::folderDialogTakesDriveFoldersOnlyOnLocalWindows_data()
{
    QTest::addColumn<QString>("platform");
    QTest::newRow("windows") << QString("windows");
    QTest::newRow("linux") << QString("linux");
}

void TestProjectsDialog::folderDialogTakesDriveFoldersOnlyOnLocalWindows()
{
    QFETCH(QString, platform); const bool windows = platform == "windows";
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setOrganizationName("hgs-folder-platform-test"); QCoreApplication::setApplicationName("projects");
    SessionOrganization projects; const auto project = projects.createGroup("Work");
    // A peer is POSIX even when it reports Windows (Phase 2 has no Windows peers).
    BoxState local; local.host = "desktop"; local.ok = true; local.metrics["platform"] = platform;
    BoxState peer; peer.host = "mac"; peer.ok = true; peer.metrics["platform"] = "windows";
    FleetState fleet; fleet.setLocal(local, QDateTime::currentMSecsSinceEpoch()); fleet.setPeer(peer, QDateTime::currentMSecsSinceEpoch());
    HgsClient client("/nonexistent/hgs"); ProjectsDialog dialog(&client, nullptr, false, &projects);
    dialog.setFleet(fleet); dialog.selectProject(project); dialog.show();
    const auto edit = [&](const std::function<void(QDialog *, QComboBox *, QLineEdit *, QPushButton *)> &check) {
        bool opened = false;
        QTimer::singleShot(0, &dialog, [&] {
            auto *folder = dialog.findChild<QDialog *>("projectFolderDialog"); QVERIFY(folder);
            check(folder, folder->findChild<QComboBox *>("projectFolderMachine"), folder->findChild<QLineEdit *>("projectFolderPath"),
                  folder->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok));
            opened = true; if (folder->isVisible()) folder->reject();
        });
        dialog.findChild<QPushButton *>("addProjectFolder")->click(); QVERIFY(opened);
    };
    edit([&](QDialog *folder, QComboBox *machine, QLineEdit *path, QPushButton *ok) {
        QCOMPARE(machine->currentData().toString(), QString());
        for (const char *drive : {"C:\\work", "c:/work/", "C:\\work/sub\\dir", "\\\\server\\share\\dir"}) {
            path->setText(drive); QVERIFY2(ok->isEnabled() == windows, drive);
        }
        for (const char *other : {"\\work", "C:work", "\\\\?\\C:\\work", "work"}) { path->setText(other); QVERIFY2(!ok->isEnabled(), other); }
        path->setText("/workspace/project"); QCOMPARE(ok->isEnabled(), !windows);
        machine->setCurrentIndex(machine->findData("mac")); QVERIFY(path->text().isEmpty());
        path->setText("C:\\work"); QVERIFY(!ok->isEnabled());
        path->setText("/Users/test/work"); QVERIFY(ok->isEnabled());
        machine->setCurrentIndex(machine->findData(QString()));
        path->setText(windows ? "C:\\Work\\Project\\" : "/workspace/project/"); QVERIFY(ok->isEnabled()); folder->accept();
    });
    const auto stored = windows ? QString("C:/Work/Project") : QString("/workspace/project");
    QCOMPARE(projects.group(project)->folders.size(), 1); QCOMPARE(projects.group(project)->folders[0].path, stored);
    QCOMPARE(projects.group(project)->folders[0].machine, QString("desktop"));
    // Windows folders show native separators; the stored form stays Qt's.
    auto *table = dialog.findChild<QTableWidget *>("projectFolders");
    QCOMPARE(table->item(0, 2)->text(), windows ? QString("C:\\Work\\Project") : stored);
    QCOMPARE(table->item(0, 0)->text(), QString(windows ? "Project" : "project"));
    // The same folder in another case or form is a duplicate only on Windows.
    edit([&](QDialog *, QComboBox *, QLineEdit *path, QPushButton *ok) {
        path->setText(windows ? "c:/work/project" : "/workspace/Project"); QCOMPARE(ok->isEnabled(), !windows);
        QCOMPARE(path->toolTip().isEmpty(), !windows);
        path->setText(windows ? "C:\\Work\\Project" : "/workspace/project"); QVERIFY(!ok->isEnabled());
    });
    // Editing shows the folder as Windows writes it.
    bool edited = false;
    QTimer::singleShot(0, &dialog, [&] {
        auto *folder = dialog.findChild<QDialog *>("projectFolderDialog"); QVERIFY(folder);
        QCOMPARE(folder->findChild<QLineEdit *>("projectFolderPath")->text(), windows ? QString("C:\\Work\\Project") : stored);
        edited = true; folder->reject();
    });
    table->setCurrentCell(0, 0); dialog.findChild<QPushButton *>("editProjectFolder")->click(); QVERIFY(edited);
    QCOMPARE(projects.group(project)->folders[0].path, stored);
}

void TestProjectsDialog::legacyAliasesOfALocalWindowsComputerBecomeFolders()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setOrganizationName("hgs-windows-import-test"); QCoreApplication::setApplicationName("projects");
    // `hgs project ls --json` of a Windows computer: drive paths as hgs.exe stores them.
    const auto hgs = directory.filePath("hgs");
    QVERIFY(FakeCli::install(hgs, R"PY(#!/usr/bin/env python3
import json
print(json.dumps([{"name": "DCB", "dir": "D:\\Work\\DCB", "src": "local", "exists": True},
                  {"name": "Tools", "dir": "D:\\Work\\Tools", "src": "local", "exists": True}]))
)PY"));
    SessionOrganization projects; const auto dcb = projects.createGroup("DCB");
    HgsClient client(FakeCli::program(hgs)); ProjectsDialog dialog(&client, nullptr, false, &projects);
    BoxState local; local.host = "desktop"; local.ok = true; local.localSessions = true; local.metrics["platform"] = "windows";
    FleetState fleet; fleet.setLocal(local, QDateTime::currentMSecsSinceEpoch());
    dialog.setFleet(fleet);
    QTRY_VERIFY(projects.hasImported("desktop"));
    // The alias joins the project of its name; another one starts its own project.
    QCOMPARE(projects.group(dcb)->folders.size(), 1);
    QCOMPARE(projects.group(dcb)->folders.first().path, QString("D:/Work/DCB"));
    QString tools; for (const auto &group : projects.groups()) if (group.name == "Tools") tools = group.id;
    QVERIFY(!tools.isEmpty());
    QCOMPARE(projects.group(tools)->folders.first().path, QString("D:/Work/Tools"));
}

void TestProjectsDialog::legacyImportReportsAliasesItCannotAdd()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setOrganizationName("hgs-import-report-test"); QCoreApplication::setApplicationName("projects");
    // One alias this Windows computer takes, one it does not (a relative path).
    const auto hgs = directory.filePath("hgs");
    QVERIFY(FakeCli::install(hgs, R"PY(#!/usr/bin/env python3
import json
open(__file__ + ".calls", "a").write("call\n")
print(json.dumps([{"name": "DCB", "dir": "D:\\Work\\DCB", "src": "local", "exists": True},
                  {"name": "Loose", "dir": "work\\loose", "src": "local", "exists": False}]))
)PY"));
    SessionOrganization projects; const auto dcb = projects.createGroup("DCB");
    HgsClient client(FakeCli::program(hgs)); ProjectsDialog dialog(&client, nullptr, false, &projects);
    BoxState local; local.host = "desktop"; local.ok = true; local.localSessions = true; local.metrics["platform"] = "windows";
    FleetState fleet; fleet.setLocal(local, QDateTime::currentMSecsSinceEpoch());
    dialog.setFleet(fleet);
    auto *status = dialog.findChild<QLabel *>("projectsStatus"); QVERIFY(status);
    QTRY_VERIFY(status->text().contains("Loose (work\\loose)"));
    QCOMPARE(projects.group(dcb)->folders.size(), 1);
    // The left-out alias starts no project, and the computer stays unimported so
    // that the next start tries again; until then no fleet update asks again.
    for (const auto &group : projects.groups()) QVERIFY(group.name != "Loose");
    QVERIFY(!projects.hasImported("desktop"));
    dialog.setFleet(fleet); QTest::qWait(300);
    QFile calls(hgs + ".calls"); QVERIFY(calls.open(QIODevice::ReadOnly));
    QCOMPARE(calls.readAll().count('\n'), 1);
}

void TestProjectsDialog::emptyListContextCreatesProject()
{
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
    QCoreApplication::setOrganizationName("hgs-project-menu-test");QCoreApplication::setApplicationName("projects");
    HgsClient client("/nonexistent/hgs");SessionOrganization projects;
    ProjectsDialog dialog(&client,nullptr,false,&projects);dialog.show();QTest::qWait(30);
    auto *list=dialog.findChild<QListWidget*>("logicalProjects");QVERIFY(list);
    const auto selected=list->currentItem();const auto empty=list->viewport()->rect().bottomLeft()+QPoint(5,-5);QVERIFY(!list->itemAt(empty));
    list->customContextMenuRequested(empty);
    auto *menu=dialog.findChild<QMenu*>("projectListMenu");QVERIFY(menu);QCOMPARE(menu->actions().size(),1);QCOMPARE(list->currentItem(),selected);
    auto *add=menu->actions().first();QCOMPARE(add->objectName(),"addProjectAction");menu->hide();
    QTimer::singleShot(0,&dialog,[]{auto *input=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());QVERIFY(input);input->setTextValue("Created from empty space");input->accept();});
    add->trigger();
    QVERIFY(std::any_of(projects.groups().cbegin(),projects.groups().cend(),[](const auto &p){return p.name=="Created from empty space";}));
    auto *item=list->currentItem();QVERIFY(item);list->customContextMenuRequested(list->visualItemRect(item).center());
    menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());QVERIFY(menu);QCOMPARE(menu->actions().first()->objectName(),"addProjectAction");
    QVERIFY(menu->findChild<QAction*>("addProjectAction"));QCOMPARE(menu->actions().size(),4);
    QVERIFY(!menu->findChild<QAction*>("projectShowInGraph")); menu->close();
}

void TestProjectsDialog::acceptsPlainName()
{
    QString why;
    QVERIFY(ProjectsDialog::nameProblem(QStringLiteral("sample-project"), &why) == false);
    QVERIFY(why.isEmpty());
}

void TestProjectsDialog::rejectsSlashAndSpace()
{
    QString why;
    QVERIFY(!ProjectsDialog::nameProblem(QStringLiteral("a/b"), &why));
    QVERIFY(why.isEmpty());
    QVERIFY(!ProjectsDialog::nameProblem(QStringLiteral("a b"), &why));
}

void TestProjectsDialog::rejectsEmpty()
{
    QString why;
    QVERIFY(ProjectsDialog::nameProblem(QString(), &why));
}

void TestProjectsDialog::warnsOnDotOrColon()
{
    // Точка допустима (example.com — пример с точкой), но в имени сессии станет "_".
    QString why;
    QVERIFY(!ProjectsDialog::nameProblem(QStringLiteral("example.com"), &why));
    QVERIFY(ProjectsDialog::nameWarning(QStringLiteral("example.com")).isEmpty());
    QVERIFY(ProjectsDialog::nameWarning(QStringLiteral("plain")).isEmpty());
}

void TestProjectsDialog::nameDefaultsToBasename()
{
    QCOMPARE(ProjectsDialog::suggestName(QStringLiteral("/workspace/sample-tray")),
             QStringLiteral("sample-tray"));
    QCOMPARE(ProjectsDialog::suggestName(QStringLiteral("/workspace/sample-tray/")),
             QStringLiteral("sample-tray"));
    QCOMPARE(ProjectsDialog::suggestName(QStringLiteral("C:\\workspace\\sample-tray\\"), MachinePlatform::Style::Windows),
             QStringLiteral("sample-tray"));
    QCOMPARE(ProjectsDialog::suggestName(QStringLiteral("\\\\server\\share\\dir"), MachinePlatform::Style::Windows),
             QStringLiteral("dir"));
    QVERIFY(ProjectsDialog::suggestName(QStringLiteral("C:\\"), MachinePlatform::Style::Windows).isEmpty());
#ifndef Q_OS_WIN
    // A backslash is part of a POSIX name (a Windows build's QDir::cleanPath reads
    // it as a separator, as it did before machine styles).
    QCOMPARE(ProjectsDialog::suggestName(QStringLiteral("/workspace/a\\b")), QStringLiteral("a\\b"));
#endif
}

QTEST_MAIN(TestProjectsDialog)
#include "test_projectsdialog.moc"
