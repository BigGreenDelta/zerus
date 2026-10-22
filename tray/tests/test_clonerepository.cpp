#include <QtTest>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>
#include "CloneRepositoryDialog.h"
#include "HgsClient.h"
#include "FakeCli.h"

class TestCloneRepository : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings,m_fixture;
    QString program() const {return FakeCli::program(m_fixture.filePath("hgs"));}
    void flag(const QString &name){QFile f(m_fixture.filePath(name));QVERIFY(f.open(QIODevice::WriteOnly));}
    QList<QJsonArray> calls() const {
        QList<QJsonArray> rows;QFile f(m_fixture.filePath("calls"));if(!f.open(QIODevice::ReadOnly))return rows;
        for(const auto &line:f.readAll().split('\n'))if(!line.isEmpty())rows.append(QJsonDocument::fromJson(line).array());return rows;
    }
    int mutations() const {int count=0;for(const auto &row:calls())if(row[2]=="clone")++count;return count;}
    void fill(CloneRepositoryDialog &dialog) {
        dialog.findChild<QLineEdit *>("cloneRepositoryUrl")->setText("git@example.invalid:team/example.git");
        dialog.findChild<QLineEdit *>("cloneParentFolder")->setText("/alias");
    }
    SessionOrganization projects() const {SessionOrganization p;p.createGroup("Example");return p;}
    static void sourceContexts(){
        QTest::addColumn<QString>("firstHost");QTest::addColumn<QString>("secondHost");QTest::addColumn<bool>("differentCli");
        QTest::newRow("local-then-peer-local")<<QString()<<QString("local")<<false;
        QTest::newRow("peer-local-then-local")<<QString("local")<<QString()<<false;
        QTest::newRow("local-distinct-cli")<<QString()<<QString()<<true;
        QTest::newRow("peer-distinct-cli")<<QString("mac")<<QString("mac")<<true;
    }
    QString anotherProgram(bool different) const {
        if(!different)return program();
        QFile source(m_fixture.filePath("hgs"));if(!source.open(QIODevice::ReadOnly))return {};
        const auto script=m_fixture.filePath("hgs-alternate");
        return FakeCli::install(script,source.readAll())?FakeCli::program(script):QString();
    }
    void canonicalParent(const QByteArray &path){
        QFile file(m_fixture.filePath("canonical-parent"));QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(path),qint64(path.size()));
    }
private slots:
    void initTestCase(){
        QVERIFY(m_settings.isValid());QCoreApplication::setOrganizationName("hgs-tests");QCoreApplication::setApplicationName("repository-clone");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,m_settings.path());
    }
    void init(){
        QSettings().clear();for(const auto &n:{"calls","old","fail-check","uncertain","wrong","hold-check","release","check-started","wrong-check","canonical-parent"})QFile::remove(m_fixture.filePath(n));
        QVERIFY(FakeCli::install(m_fixture.filePath("hgs"),R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
a=sys.argv[1:];host=''
if a and a[0].startswith('@'):host=a.pop(0)[1:]
if a==['--help']:
 print('old client' if (root/'old').exists() else 'repository-clone-v1');sys.exit(0)
with open(root/'calls','a') as f:f.write(json.dumps([host]+a)+'\n')
def get(k):return a[a.index(k)+1]
if a[:2]==['repository','check']:
 if (root/'hold-check').exists():
  (root/'check-started').touch()
  for _ in range(100):
   if (root/'release').exists():break
   time.sleep(.02)
 if (root/'fail-check').exists():sys.stderr.write('Access unavailable');sys.exit(1)
 parent=(root/'canonical-parent').read_text() if (root/'canonical-parent').exists() else '/canonical'
 print(json.dumps({'status':'ready','name':get('--name'),'parent':parent,'path':'/different' if (root/'wrong-check').exists() else parent+'/'+get('--name')}))
elif a[:2]==['repository','clone']:
 receipt={'status':'cloned','request_id':get('--request-id'),'name':get('--name'),'parent':get('--parent'),'path':get('--parent')+'/'+get('--name')}
 (root/'receipt').write_text(json.dumps(receipt))
 if (root/'wrong').exists():receipt['path']='/different/example'
 if (root/'uncertain').exists():receipt['status']='unconfirmed'
 print(json.dumps(receipt))
elif a[:2]==['repository','status']:print((root/'receipt').read_text())
)PY"));
    }
    void suggestedNameAndManualEdits(){
        QCOMPARE(CloneRepositoryDialog::suggestedName("https://example.invalid/team/thing.git"),"thing");
        QCOMPARE(CloneRepositoryDialog::suggestedName("git@example.invalid:team/thing.git"),"thing");
        QCOMPARE(CloneRepositoryDialog::suggestedName("ssh://git@example.invalid/team/thing.git/"),"thing");
        HgsClient client(program());auto p=projects();CloneRepositoryDialog d(&client,"mac","Mac",MachinePlatform::Style::Posix,p,{},false);fill(d);
        auto *name=d.findChild<QLineEdit *>("cloneFolderName");QCOMPARE(name->text(),"example");
        name->setFocus();name->selectAll();QTest::keyClicks(name,"custom");
        d.findChild<QLineEdit *>("cloneRepositoryUrl")->setText("git@example.invalid:team/other.git");QCOMPARE(name->text(),"custom");
        d.findChild<QComboBox *>("cloneProject")->setCurrentIndex(d.findChild<QComboBox *>("cloneProject")->count()-1);
        QCOMPARE(d.newProjectName(),"custom");
        QVERIFY(calls().isEmpty());
    }
    void exactComputerCanonicalParentAndExplicitClone(){
        HgsClient client(program());auto p=projects();CloneRepositoryDialog d(&client,"mac","Mac",MachinePlatform::Style::Posix,p,{},true);fill(d);d.show();
        QSignalSpy accepted(&d,&QDialog::accepted);QVERIFY(calls().isEmpty());
        d.findChild<QPushButton *>("cloneRepositoryConfirm")->click();QTRY_COMPARE(accepted.size(),1);
        QCOMPARE(d.createdPath(),"/canonical/example");QCOMPARE(mutations(),1);
        for(const auto &r:calls())QCOMPARE(r[0].toString(),"mac");
        const auto clone=calls().last().toVariantList();QCOMPARE(clone[clone.indexOf("--parent")+1].toString(),"/canonical");
        QVERIFY(!QUuid(clone[clone.indexOf("--request-id")+1].toString()).isNull());
    }
    void oldClientAndAccessFailureNeverClone(){
        auto p=projects();HgsClient client(program());flag("old");
        CloneRepositoryDialog old(&client,"mac","Mac",MachinePlatform::Style::Posix,p,{},false);fill(old);old.findChild<QPushButton *>("cloneRepositoryConfirm")->click();
        QTRY_VERIFY(old.findChild<QLabel *>("cloneRepositoryStatus")->text().contains("Update hgs"));QVERIFY(calls().isEmpty());
        QFile::remove(m_fixture.filePath("old"));flag("fail-check");
        CloneRepositoryDialog denied(&client,"mac","Mac",MachinePlatform::Style::Posix,p,{},false);fill(denied);denied.findChild<QPushButton *>("cloneRepositoryConfirm")->click();
        QTRY_VERIFY(denied.findChild<QLabel *>("cloneRepositoryStatus")->text().contains("Access unavailable"));QCOMPARE(mutations(),0);
    }
    void uncertainCloneRestoresAndReadsOriginalReceiptWithoutStarting(){
        auto p=projects();HgsClient client(program());flag("uncertain");
        {
            CloneRepositoryDialog first(&client,"mac","Mac",MachinePlatform::Style::Posix,p,{},true);fill(first);first.findChild<QPushButton *>("cloneRepositoryConfirm")->click();
            QTRY_VERIFY(first.findChild<QLabel *>("cloneRepositoryStatus")->text().contains("still running"));
            QVERIFY(!first.findChild<QPushButton *>("cloneRepositoryConfirm")->isEnabled());QCOMPARE(mutations(),1);
        }
        CloneRepositoryDialog restored(&client,"mac","Mac",MachinePlatform::Style::Posix,p,{},true);restored.show();QSignalSpy accepted(&restored,&QDialog::accepted);
        QCOMPARE(restored.findChild<QLineEdit *>("cloneFolderName")->text(),"example");
        restored.findChild<QPushButton *>("cloneCheckResult")->click();
        QTRY_VERIFY(restored.findChild<QPushButton *>("cloneRepositoryConfirm")->isEnabled());QCOMPARE(accepted.size(),0);QCOMPARE(mutations(),1);
        QCOMPARE(restored.createdPath(),"/canonical/example");restored.findChild<QPushButton *>("cloneRepositoryConfirm")->click();QCOMPARE(accepted.size(),1);QCOMPARE(mutations(),1);
    }
    void rememberedParentsAreScopedToExecutableAndHost_data(){sourceContexts();}
    void rememberedParentsAreScopedToExecutableAndHost(){
        QFETCH(QString,firstHost);QFETCH(QString,secondHost);QFETCH(bool,differentCli);
        const auto secondProgram=anotherProgram(differentCli);QVERIFY(!secondProgram.isEmpty());
        HgsClient firstClient(program()),secondClient(secondProgram);auto p=projects();
        QSettings().setValue("workspace/unrelated-clone-test","kept");
        {
            CloneRepositoryDialog first(&firstClient,firstHost,"First",MachinePlatform::Style::Posix,p,{},false);fill(first);
            QSignalSpy accepted(&first,&QDialog::accepted);first.findChild<QPushButton *>("cloneRepositoryConfirm")->click();QTRY_COMPARE(accepted.size(),1);
        }
        {
            CloneRepositoryDialog second(&secondClient,secondHost,"Second",MachinePlatform::Style::Posix,p,{},false);
            QCOMPARE(second.findChild<QLineEdit *>("cloneParentFolder")->text(),QString("~"));
            canonicalParent("/second-canonical");fill(second);
            QSignalSpy accepted(&second,&QDialog::accepted);second.findChild<QPushButton *>("cloneRepositoryConfirm")->click();QTRY_COMPARE(accepted.size(),1);
        }
        const auto before=mutations();
        CloneRepositoryDialog firstAgain(&firstClient,firstHost,"First",MachinePlatform::Style::Posix,p,{},false);
        CloneRepositoryDialog secondAgain(&secondClient,secondHost,"Second",MachinePlatform::Style::Posix,p,{},false);
        QCOMPARE(firstAgain.findChild<QLineEdit *>("cloneParentFolder")->text(),QString("/canonical"));
        QCOMPARE(secondAgain.findChild<QLineEdit *>("cloneParentFolder")->text(),QString("/second-canonical"));
        QCOMPARE(mutations(),before);QCOMPARE(QSettings().value("workspace/unrelated-clone-test").toString(),QString("kept"));
    }
    void pendingRecoveryIsScopedToExecutableAndHost_data(){sourceContexts();}
    void pendingRecoveryIsScopedToExecutableAndHost(){
        QFETCH(QString,firstHost);QFETCH(QString,secondHost);QFETCH(bool,differentCli);
        const auto secondProgram=anotherProgram(differentCli);QVERIFY(!secondProgram.isEmpty());
        HgsClient firstClient(program()),secondClient(secondProgram);auto p=projects();flag("uncertain");
        {
            CloneRepositoryDialog first(&firstClient,firstHost,"First",MachinePlatform::Style::Posix,p,{},true);fill(first);
            first.findChild<QPushButton *>("cloneRepositoryConfirm")->click();
            QTRY_VERIFY(first.findChild<QLabel *>("cloneRepositoryStatus")->text().contains("still running"));
        }
        {
            CloneRepositoryDialog second(&secondClient,secondHost,"Second",MachinePlatform::Style::Posix,p,{},true);
            QVERIFY(second.findChild<QLineEdit *>("cloneFolderName")->text().isEmpty());
            QVERIFY(second.findChild<QLineEdit *>("cloneRepositoryUrl")->isEnabled());
            QVERIFY(second.findChild<QLabel *>("cloneRepositoryStatus")->text().isEmpty());
            canonicalParent("/second-canonical");fill(second);second.findChild<QLineEdit *>("cloneFolderName")->setText("second");
            second.findChild<QPushButton *>("cloneRepositoryConfirm")->click();
            QTRY_VERIFY(second.findChild<QLabel *>("cloneRepositoryStatus")->text().contains("still running"));
        }
        const auto before=mutations();
        CloneRepositoryDialog firstAgain(&firstClient,firstHost,"First",MachinePlatform::Style::Posix,p,{},true);
        CloneRepositoryDialog secondAgain(&secondClient,secondHost,"Second",MachinePlatform::Style::Posix,p,{},true);
        QCOMPARE(firstAgain.findChild<QLineEdit *>("cloneParentFolder")->text(),QString("/canonical"));
        QCOMPARE(firstAgain.findChild<QLineEdit *>("cloneFolderName")->text(),QString("example"));
        QCOMPARE(secondAgain.findChild<QLineEdit *>("cloneParentFolder")->text(),QString("/second-canonical"));
        QCOMPARE(secondAgain.findChild<QLineEdit *>("cloneFolderName")->text(),QString("second"));
        for(auto *form:{&firstAgain,&secondAgain}){
            QVERIFY(!form->findChild<QLineEdit *>("cloneRepositoryUrl")->isEnabled());
            QVERIFY(form->findChild<QLabel *>("cloneRepositoryStatus")->text().contains("previous clone is unconfirmed"));
        }
        QCOMPARE(mutations(),before);
    }
    void inconsistentPreflightCannotClone(){
        auto p=projects();HgsClient client(program());flag("wrong-check");CloneRepositoryDialog d(&client,"mac","Mac",MachinePlatform::Style::Posix,p,{},true);fill(d);
        d.findChild<QPushButton *>("cloneRepositoryConfirm")->click();QTRY_VERIFY(d.findChild<QLabel *>("cloneRepositoryStatus")->text().contains("Invalid repository response"));QCOMPARE(mutations(),0);QVERIFY(d.createdPath().isEmpty());
    }
    void cancellingAccessCheckCannotStartClone(){
        auto p=projects();HgsClient client(program());flag("hold-check");
        CloneRepositoryDialog d(&client,"mac","Mac",MachinePlatform::Style::Posix,p,{},true);fill(d);d.show();
        QSignalSpy finished(&client,&HgsClient::repositoryFinished);d.findChild<QPushButton *>("cloneRepositoryConfirm")->click();
        QTRY_VERIFY(QFile::exists(m_fixture.filePath("check-started")));d.reject();flag("release");
        QTRY_COMPARE(finished.size(),1);QVERIFY(!d.findChild<QLabel *>("cloneRepositoryStatus")->text().contains("Cloning"));QCOMPARE(mutations(),0);QVERIFY(d.createdPath().isEmpty());QCOMPARE(d.result(),int(QDialog::Rejected));
    }
    void leavingUncertainClonePreservesFilesAndRequiresNewDestination(){
        auto p=projects();HgsClient client(program());flag("uncertain");CloneRepositoryDialog d(&client,"mac","Mac",MachinePlatform::Style::Posix,p,{},true);fill(d);d.show();
        d.findChild<QPushButton *>("cloneRepositoryConfirm")->click();QTRY_VERIFY(d.findChild<QLabel *>("cloneRepositoryStatus")->text().contains("still running"));
        d.findChild<QPushButton *>("cloneAnotherDestination")->click();QVERIFY(d.findChild<QLineEdit *>("cloneFolderName")->text().isEmpty());QVERIFY(d.findChild<QLineEdit *>("cloneRepositoryUrl")->isEnabled());
        QVERIFY(QFile::exists(m_fixture.filePath("receipt")));QCOMPARE(mutations(),1);QVERIFY(!d.findChild<QPushButton *>("cloneRepositoryConfirm")->isEnabled());
    }
    void mismatchedReceiptCannotBeUsed(){
        auto p=projects();HgsClient client(program());flag("wrong");CloneRepositoryDialog d(&client,"mac","Mac",MachinePlatform::Style::Posix,p,{},true);fill(d);QSignalSpy accepted(&d,&QDialog::accepted);
        d.findChild<QPushButton *>("cloneRepositoryConfirm")->click();QTRY_VERIFY(d.findChild<QLabel *>("cloneRepositoryStatus")->text().contains("does not match"));
        QCOMPARE(accepted.size(),0);QCOMPARE(mutations(),1);QVERIFY(d.createdPath().isEmpty());QVERIFY(!d.findChild<QPushButton *>("cloneRepositoryConfirm")->isEnabled());
    }
    void narrowLargeTextKeepsActionsInsideDialog(){
        auto p=projects();HgsClient client(program());CloneRepositoryDialog d(&client,"mac","Mac",MachinePlatform::Style::Posix,p,{},true);fill(d);
        auto font=d.font();font.setPointSizeF(font.pointSizeF()*1.4);d.setFont(font);d.resize(520,640);d.show();QVERIFY(QTest::qWaitForWindowExposed(&d));
        const auto *button=d.findChild<QPushButton *>("cloneRepositoryConfirm");QVERIFY(d.rect().contains(QRect(button->mapTo(&d,QPoint()),button->size())));
        QVERIFY(d.findChild<QLabel *>("cloneComputer")->text().contains("Mac"));
        if(const auto path=qEnvironmentVariable("ZERUS_PREVIEW_DIR");!path.isEmpty())d.grab().save(path+"/clone-repository.png");
    }
};
QTEST_MAIN(TestCloneRepository)
#include "test_clonerepository.moc"
