#include <QtTest>
#include <QAction>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include "DirectoryDialog.h"
#include "FakeCli.h"

class TestDirectoryDialog : public QObject {
    Q_OBJECT
    QTemporaryDir m_fixture;
    QFont m_originalFont;
    QString program() const { return FakeCli::program(m_fixture.filePath("hgs")); }
    void flag(const QString &name, bool enabled = true) {
        QFile file(m_fixture.filePath(name));
        if (enabled) { QVERIFY(file.open(QIODevice::WriteOnly)); }
        else file.remove();
    }
    QJsonArray calls() const {
        QFile file(m_fixture.filePath("calls"));
        if (!file.open(QIODevice::ReadOnly)) return {};
        QJsonArray result;
        for (const auto &line : file.readAll().split('\n')) {
            const auto value = QJsonDocument::fromJson(line);
            if (value.isObject()) result.append(value.object());
        }
        return result;
    }
    int mutations() const {
        int count = 0;
        for (const auto &call : calls())
            if (call.toObject()["args"].toArray().contains(QStringLiteral("--create"))) ++count;
        return count;
    }
    void prompt(DirectoryDialog &browser, const QString &candidate, bool accept = true) {
        bool observed = false;
        QTimer::singleShot(0, &browser, [&] {
            auto *dialog = browser.findChild<QDialog *>("createFolderDialog");
            QVERIFY(dialog); observed = true;
            auto *path = dialog->findChild<QLineEdit *>("newFolderName"); QVERIFY(path);
            if (!candidate.isNull()) path->setText(candidate);
            if (!accept) { dialog->reject(); return; }
            auto *create = dialog->findChild<QPushButton *>("confirmCreateFolder");
            QVERIFY(create); QVERIFY(create->isEnabled()); create->click();
        });
        browser.findChild<QAction *>("newFolderDialog")->trigger();
        QVERIFY(observed);
    }
private slots:
    void init() {
        m_originalFont = qApp->font();
        for (const auto &name : {"calls", "created", "old", "hold", "entered", "wrong", "wrong-path", "wrong-parent", "fail", "read-fail"}) flag(name, false);
        QVERIFY(FakeCli::install(m_fixture.filePath("hgs"), R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
a=sys.argv[1:];host=''
if a and a[0].startswith('@'):host=a.pop(0)[1:]
with (root/'calls').open('a') as out:out.write(json.dumps({'host':host,'args':a})+'\n')
if a==['--help']:
 print('hgs dirs [path]' if (root/'old').exists() else 'hgs [@host] dirs --create <path> [--request-id UUID]')
 sys.exit(0)
if a and a[0]=='dirs':
 if len(a)==5 and a[1]=='--create' and a[3]=='--request-id':
  path=a[2];identity=a[4];(root/'entered').touch()
  until=time.monotonic()+10
  while (root/'hold').exists() and time.monotonic()<until:time.sleep(.01)
  if (root/'fail').exists():print('permission denied',file=sys.stderr);sys.exit(1)
  canonical=path.replace('/fixture/link/','/fixture/projects/',1)
  if canonical=='/fixture/projects/existing':print('File exists (os error 17)',file=sys.stderr);sys.exit(1)
  (root/'created').write_text(canonical)
  print(json.dumps({'status':'created','requested_path':path,
                    'path':'/fixture/projects/unrelated' if (root/'wrong-path').exists() else canonical,
                    'parent':'/other' if (root/'wrong-parent').exists() else '/fixture/projects',
                    'request_id':'wrong' if (root/'wrong').exists() else identity}))
  sys.exit(0)
 path=a[-1] if len(a)>1 else '~'
 path='/fixture/projects' if path=='~' else path
 path='/fixture/projects' if path=='/fixture/link' else path
 created=(root/'created').read_text() if (root/'created').exists() else ''
 if (root/'read-fail').exists() and path==created:print('read unavailable',file=sys.stderr);sys.exit(1)
 if path not in ('/fixture/projects','/fixture/projects/existing',created):
  print('No such file or directory',file=sys.stderr);sys.exit(1)
 folders=[{'name':'existing','path':'/fixture/projects/existing'}] if path=='/fixture/projects' else []
 print(json.dumps({'path':path,'parent':'/fixture/projects','home':'/fixture/projects','directories':folders}))
 sys.exit(0)
sys.exit(1)
)PY"));
    }
    void cleanup() { qApp->setFont(m_originalFont); }
    void openingAndCancellingAreReadOnly() {
        DirectoryDialog browser(program(), "arch", "~"); browser.show();
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        QCOMPARE(browser.directory(), QStringLiteral("/fixture/projects"));
        QCOMPARE(mutations(), 0);
        prompt(browser, "new-project", false);
        QCOMPARE(mutations(), 0); QCOMPARE(browser.directory(), QStringLiteral("/fixture/projects"));
        browser.reject(); QCOMPARE(browser.result(), int(QDialog::Rejected));
    }
    void createAndUseConfirmedRemoteFolder() {
        DirectoryDialog browser(program(), "arch", "~"); browser.show();
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        const QString path = QString::fromUtf8("/fixture/projects/new ' проект $(touch nothing)");
        prompt(browser, path.section('/', -1));
        QTRY_COMPARE(browser.directory(), path);
        auto *choose = browser.findChild<QPushButton *>("chooseFolder"); QTRY_VERIFY(choose->isEnabled());
        QCOMPARE(mutations(), 1); QCOMPARE(browser.result(), int(QDialog::Rejected));
        for (const auto &call : calls()) {
            QCOMPARE(call.toObject()["host"].toString(), QStringLiteral("arch"));
            const auto args = call.toObject()["args"].toArray();
            if (args.contains(QStringLiteral("--create"))) {
                QCOMPARE(args[2].toString(), path); QCOMPARE(args[3].toString(), QStringLiteral("--request-id"));
                QVERIFY(!QUuid(args[4].toString()).isNull());
            }
        }
        choose->click(); QCOMPARE(browser.result(), int(QDialog::Accepted)); QCOMPARE(browser.directory(), path);
    }
    void missingTypedPathIsCarriedIntoExplicitCreation() {
        const QString path = "/fixture/projects/new-project";
        DirectoryDialog browser(program(), "arch", path); browser.show();
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        QVERIFY(browser.directory().isEmpty()); QVERIFY(!browser.findChild<QPushButton *>("chooseFolder")->isEnabled());
        QCOMPARE(mutations(), 0);
        bool exact = false;
        QTimer::singleShot(0, &browser, [&] {
            auto *dialog = browser.findChild<QDialog *>("createFolderDialog"); QVERIFY(dialog);
            exact = dialog->findChild<QLineEdit *>("newFolderName")->text() == "new-project";
            dialog->findChild<QPushButton *>("confirmCreateFolder")->click();
        });
        browser.findChild<QPushButton *>("newFolder")->click(); QVERIFY(exact);
        QTRY_COMPARE(browser.directory(), path); QCOMPARE(mutations(), 1);
    }
    void symlinkParentSelectsCanonicalCreatedFolder() {
        DirectoryDialog browser(program(), "arch", "/fixture/link/new-project"); browser.show();
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        prompt(browser, "new-project");
        QTRY_COMPARE(browser.directory(), QStringLiteral("/fixture/projects/new-project"));
        QTRY_VERIFY(browser.findChild<QPushButton *>("chooseFolder")->isEnabled());
        QCOMPARE(mutations(), 1);
        bool literal = false;
        for (const auto &call : calls()) {
            const auto args = call.toObject()["args"].toArray();
            if (args.contains(QStringLiteral("--create"))) literal = args[2] == "/fixture/link/new-project";
        }
        QVERIFY(literal);
    }
    void heldCreationBlocksDuplicateNavigationAndRejectsStaleReplies() {
        flag("hold"); DirectoryDialog browser(program(), "arch", "~"); browser.show();
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        prompt(browser, "new-project");
        QTRY_VERIFY(QFile::exists(m_fixture.filePath("entered")));
        auto *create = browser.findChild<QPushButton *>("newFolder");
        QVERIFY(!create->isEnabled()); QVERIFY(!browser.findChild<QPushButton *>("folderGo")->isEnabled());
        QVERIFY(!browser.findChild<QLineEdit *>("folderPath")->isEnabled());
        QTest::mouseClick(create, Qt::LeftButton); QCOMPARE(mutations(), 1);
        browser.reject(); QVERIFY(browser.isVisible());
        auto *client = browser.findChild<HgsClient *>(); QVERIFY(client);
        const QJsonObject stale{{"path", "/stale"}, {"directories", QJsonArray{}}};
        client->directoriesReady(1, "arch", stale);
        client->directoryCreated(999, "arch", stale);
        client->directoryCreated(2, "other", stale);
        QVERIFY(!browser.findChild<QPushButton *>("chooseFolder")->isEnabled());
        QVERIFY(!create->isEnabled());
        flag("hold", false);
        QTRY_COMPARE(browser.directory(), QStringLiteral("/fixture/projects/new-project"));
        QTRY_VERIFY(create->isEnabled()); QCOMPARE(mutations(), 1);
    }
    void unconfirmedReceiptRecoversByReadingWithoutRetryingCreation() {
        flag("wrong"); DirectoryDialog browser(program(), "arch", "~"); browser.show();
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        prompt(browser, "new-project");
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        QTRY_COMPARE(browser.directory(), QStringLiteral("/fixture/projects/new-project"));
        QTRY_VERIFY(browser.findChild<QPushButton *>("chooseFolder")->isEnabled()); QCOMPARE(mutations(), 1);
    }
    void oldPeerDoesNotReceiveAnUnsupportedMutation() {
        flag("old"); DirectoryDialog browser(program(), "arch", "~"); browser.show();
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        prompt(browser, "new-project");
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        QCOMPARE(mutations(), 0); QVERIFY(browser.directory().isEmpty());
        browser.findChild<QLineEdit *>("folderPath")->setText("/fixture/projects");
        browser.findChild<QPushButton *>("folderGo")->click();
        QTRY_COMPARE(browser.directory(), QStringLiteral("/fixture/projects"));
        QVERIFY(browser.findChild<QPushButton *>("chooseFolder")->isEnabled());
    }
    void wrongCanonicalPathOrParentCannotBeSelected_data() {
        QTest::addColumn<QString>("wrong");
        QTest::newRow("wrong-leaf") << QStringLiteral("wrong-path");
        QTest::newRow("wrong-parent") << QStringLiteral("wrong-parent");
    }
    void wrongCanonicalPathOrParentCannotBeSelected() {
        QFETCH(QString, wrong); flag(wrong); flag("read-fail");
        DirectoryDialog browser(program(), "arch", "~"); browser.show();
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        prompt(browser, "new-project");
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        QVERIFY(browser.directory().isEmpty()); QVERIFY(!browser.findChild<QPushButton *>("chooseFolder")->isEnabled());
        QCOMPARE(mutations(), 1);
    }
    void failedCreationPreservesExactPathAndRecovers() {
        flag("fail"); DirectoryDialog browser(program(), "arch", "~"); browser.show();
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        prompt(browser, "new-project");
        QTRY_VERIFY(browser.findChild<QPushButton *>("newFolder")->isEnabled());
        QCOMPARE(browser.findChild<QLineEdit *>("folderPath")->text(), QStringLiteral("/fixture/projects/new-project"));
        QVERIFY(browser.directory().isEmpty()); QCOMPARE(mutations(), 1);
        browser.findChild<QPushButton *>("folderUp")->click();
        QTRY_COMPARE(browser.directory(), QStringLiteral("/fixture/projects"));
        QVERIFY(browser.findChild<QPushButton *>("chooseFolder")->isEnabled());
    }
    void existingEmptyFolderCanBeSelectedAfterCreationConflict() {
        DirectoryDialog browser(program(), "arch", "~"); browser.show();
        auto *choose = browser.findChild<QPushButton *>("chooseFolder"); QTRY_VERIFY(choose->isEnabled());
        prompt(browser, "existing");
        QTRY_COMPARE(browser.directory(), QStringLiteral("/fixture/projects/existing"));
        QTRY_VERIFY(choose->isEnabled());
        QCOMPARE(browser.findChild<QListWidget *>("folderList")->count(), 0);
        QCOMPARE(choose->text(), QString("Use this folder"));
        QCOMPARE(mutations(), 1); QVERIFY(browser.isVisible());
        choose->click(); QCOMPARE(browser.result(), int(QDialog::Accepted));
    }
    void inlineCreationUsesOnlyTheEnteredChildName() {
        DirectoryDialog browser(program(), "arch", "~"); browser.show();
        auto *newFolder = browser.findChild<QPushButton *>("newFolder"); QTRY_VERIFY(newFolder->isEnabled());
        newFolder->click();
        auto *name = browser.findChild<QLineEdit *>("inlineFolderName"); QVERIFY(name);
        QCOMPARE(name->text(), QString());
        auto *create = browser.findChild<QPushButton *>("createInlineFolder"); QVERIFY(create);
        QVERIFY(!create->isEnabled());
        for (const auto &invalid : {".", "..", "nested/child", "/elsewhere", "line\nbreak"}) {
            name->setText(invalid); QVERIFY(!create->isEnabled());
            QTest::keyClick(name, Qt::Key_Return); QCOMPARE(mutations(), 0); QVERIFY(browser.isVisible());
        }
        const QString leaf = QString::fromUtf8("новая ' $(literal)");
        name->setText(leaf); QVERIFY(create->isEnabled());
        QTest::keyClick(name, Qt::Key_Return);
        QTRY_COMPARE(browser.directory(), "/fixture/projects/" + leaf);
        QTRY_VERIFY(browser.findChild<QPushButton *>("chooseFolder")->isEnabled());
        QCOMPARE(mutations(), 1); QVERIFY(browser.isVisible());
        QCOMPARE(browser.findChild<QLineEdit *>("folderPath")->text(), browser.directory());
    }
    void cancellingOrNavigatingAwayFromInlineCreationIsReadOnly() {
        DirectoryDialog browser(program(), "arch", "~"); browser.show();
        auto *newFolder = browser.findChild<QPushButton *>("newFolder"); QTRY_VERIFY(newFolder->isEnabled());
        newFolder->click();
        auto *name = browser.findChild<QLineEdit *>("inlineFolderName"); QVERIFY(name);
        name->setText("unsaved"); QTest::keyClick(name, Qt::Key_Escape);
        QVERIFY(browser.isVisible()); QCOMPARE(mutations(), 0);
        auto *list = browser.findChild<QListWidget *>("folderList"); QCOMPARE(list->count(), 1);
        QVERIFY(browser.findChild<QPushButton *>("chooseFolder")->isEnabled());
        newFolder->click(); browser.findChild<QPushButton *>("folderGo")->click();
        QTRY_VERIFY(newFolder->isEnabled()); QCOMPARE(list->count(), 1); QCOMPARE(mutations(), 0);
        newFolder->click(); browser.findChild<QToolButton *>("cancelInlineFolder")->click();
        QCOMPARE(list->count(), 1); QCOMPARE(mutations(), 0);
    }
    void chooserAndCreateFormKeepControlsInside_data() {
        QTest::addColumn<int>("width"); QTest::addColumn<double>("fontScale");
        QTest::newRow("normal") << 660 << 1.0;
        QTest::newRow("narrow-large-font") << 500 << 1.8;
    }
    void chooserAndCreateFormKeepControlsInside() {
        QFETCH(int, width); QFETCH(double, fontScale);
        QWidget workspace;
        // The real ancestor style enlarges controls after they join the list.
        workspace.setStyleSheet("QPushButton { border:1px solid #45515e; padding:8px 13px; min-height:18px; }"
            "QLineEdit { border:1px solid #45515e; padding:9px 10px; }");
        DirectoryDialog browser(program(), "arch", "~", &workspace);
        auto font = browser.font(); font.setPointSizeF((font.pointSizeF() > 0 ? font.pointSizeF() : 12) * fontScale);
        qApp->setFont(font);
        browser.setFont(font); browser.resize(width, 540); browser.show();
        auto *newFolder = browser.findChild<QPushButton *>("newFolder"); QTRY_VERIFY(newFolder->isEnabled());
        auto *choose = browser.findChild<QPushButton *>("chooseFolder");
        QVERIFY(browser.rect().contains(QRect(newFolder->mapTo(&browser, QPoint()), newFolder->size())));
        QVERIFY(browser.rect().contains(QRect(choose->mapTo(&browser, QPoint()), choose->size())));
        newFolder->click();
        auto *inlineName = browser.findChild<QLineEdit *>("inlineFolderName"); QVERIFY(inlineName);
        auto *inlineCreate = browser.findChild<QPushButton *>("createInlineFolder"); QVERIFY(inlineCreate);
        QTest::qWait(30);
        auto *list = browser.findChild<QListWidget *>("folderList");
        const auto rowRect = list->visualItemRect(list->item(0));
        for (auto *control : QList<QWidget *>{inlineName, inlineCreate}) {
            const QRect rect(control->mapTo(list->viewport(), QPoint()), control->size());
            QVERIFY2(rowRect.contains(rect), qPrintable(QString("Row %1x%2 clips control at %3,%4 %5x%6")
                .arg(rowRect.width()).arg(rowRect.height()).arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height())));
        }
        const QString output = QString::fromLocal8Bit(qgetenv("ZERUS_DIRECTORY_QA_DIR"));
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(browser.grab().save(output + "/desktop-browser-" + QTest::currentDataTag() + ".png"));
        }
        bool checked = false;
        QTimer::singleShot(0, &browser, [&] {
            auto *dialog = browser.findChild<QDialog *>("createFolderDialog"); QVERIFY(dialog);
            auto *path = dialog->findChild<QLineEdit *>("newFolderName");
            auto *create = dialog->findChild<QPushButton *>("confirmCreateFolder");
            QVERIFY(path); QVERIFY(create);
            QVERIFY(path->text().isEmpty());
            path->setText("new-project");
            dialog->layout()->activate();
            QVERIFY(dialog->rect().contains(QRect(path->mapTo(dialog, QPoint()), path->size())));
            QVERIFY(dialog->rect().contains(QRect(create->mapTo(dialog, QPoint()), create->size())));
            QVERIFY(create->isEnabled());
            for (const QString invalid : {"", ".", "..", "a/b", "/absolute", "new\nfolder"}) {
                path->setText(invalid); QVERIFY(!create->isEnabled());
            }
            path->setText("new-project"); QVERIFY(create->isEnabled());
            if (!output.isEmpty()) QVERIFY(dialog->grab().save(output + "/desktop-create-" + QTest::currentDataTag() + ".png"));
            checked = true; dialog->reject();
        });
        browser.findChild<QAction *>("newFolderDialog")->trigger(); QVERIFY(checked); QCOMPARE(mutations(), 0);
    }
};
QTEST_MAIN(TestDirectoryDialog)
#include "test_directorydialog.moc"
