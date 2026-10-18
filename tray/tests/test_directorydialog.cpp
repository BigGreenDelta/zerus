#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
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
#include <QRegularExpression>
#include <QScreen>
#include <QStyleOptionFrame>
#include <QTimer>
#include <QUuid>
#include "DirectoryDialog.h"
#include "FakeCli.h"

class TestDirectoryDialog : public QObject {
    Q_OBJECT
    QTemporaryDir m_fixture;
    QFont m_originalFont;
    QString program() const { return FakeCli::program(m_fixture.filePath("hgs")); }
    QString workspaceStyle(bool dark, double fontScale) const {
        // Use the actual ancestor rules, including item padding. A reduced
        // imitation can fit the outer widgets while clipping all editor ink.
        QFile source(QFINDTESTDATA("../src/SessionsWindow.cpp"));
        if (!source.open(QIODevice::ReadOnly)) return {};
        const auto contents = QString::fromUtf8(source.readAll());
        const QString begin = "setStyleSheet(QString(R\"(";
        const int start = contents.indexOf(begin);
        const int end = contents.indexOf(")\").arg(bg, m_fg", start);
        if (start < 0 || end < 0) return {};
        auto css = contents.mid(start + begin.size(), end - start - begin.size());
        const QStringList colors = dark
            ? QStringList{"#161b21", "#e8edf4", "#1c2229", "#333c47", "#8bdfc0", "#a2adbc", "#2b3540", "#12372b", "#aae9d2", "#233d35", "#151b21", "#f4ab9b"}
            : QStringList{"#f5f7f9", "#1a2733", "#ffffff", "#dce2e8", "#167357", "#627082", "#edf2f5", "#ffffff", "#115d46", "#e0f0e9", "#f3f6f8", "#a13224"};
        for (int i = colors.size(); i > 0; --i) css.replace('%' + QString::number(i), colors[i - 1]);
        // ContentScale deliberately leaves chooser chrome alone. Also exercise
        // enlarged system text without changing control padding or geometry.
        const QRegularExpression sizes("font-size:(\\d+)px");
        const auto original = css; qsizetype copied = 0; QString scaled;
        for (auto matches = sizes.globalMatch(original); matches.hasNext();) {
            const auto match = matches.next();
            scaled += original.mid(copied, match.capturedStart() - copied)
                + "font-size:" + QString::number(qRound(match.captured(1).toInt() * fontScale)) + "px";
            copied = match.capturedEnd();
        }
        return scaled + original.mid(copied);
    }
    QRect editorContents(QLineEdit *editor) const {
        QStyleOptionFrame option; option.initFrom(editor); option.rect = editor->rect();
        option.lineWidth = editor->hasFrame() ? editor->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, editor) : 0;
        auto rect = editor->style()->subElementRect(QStyle::SE_LineEditContents, &option, editor);
        return rect.marginsRemoved(editor->textMargins());
    }
    int editorInk(QLineEdit *editor) const {
        const auto image = editor->grab().toImage();
        const qreal dpr = image.devicePixelRatio();
        const auto contents = editorContents(editor);
        const QRect pixels(qRound(contents.x() * dpr), qRound(contents.y() * dpr),
                           qRound(contents.width() * dpr), qRound(contents.height() * dpr));
        const auto text = editor->palette().color(QPalette::Text);
        int ink = 0;
        for (int y = qMax(0, pixels.top()); y <= qMin(image.height() - 1, pixels.bottom()); ++y)
            for (int x = qMax(0, pixels.left()); x <= qMin(image.width() - 1, pixels.right()); ++x) {
                const auto color = image.pixelColor(x, y);
                if (qMax(qAbs(color.red() - text.red()), qMax(qAbs(color.green() - text.green()), qAbs(color.blue() - text.blue()))) < 60) ++ink;
            }
        return qRound(ink / (dpr * dpr));
    }
    void captureEditor(DirectoryDialog &browser, const QString &stage) const {
        const auto output = QString::fromLocal8Bit(qgetenv("ZERUS_DIRECTORY_QA_DIR"));
        if (output.isEmpty()) return;
        QVERIFY(QDir().mkpath(output));
        QVERIFY(browser.grab().save(output + "/inline-" + QTest::currentDataTag() + '-' + stage + ".png"));
    }
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
        newFolder->click(); browser.findChild<QPushButton *>("cancelInlineFolder")->click();
        QCOMPARE(list->count(), 1); QCOMPARE(mutations(), 0);
    }
    void inlineNameHasVisibleGlyphsAndKeyboardInput_data() {
        QTest::addColumn<bool>("dark"); QTest::addColumn<double>("fontScale");
        for (bool dark : {false, true}) for (double scale : {1.0, 2.0})
            QTest::newRow(qPrintable(QString("%1-font%2").arg(dark ? "dark" : "light").arg(scale))) << dark << scale;
    }
    void inlineNameHasVisibleGlyphsAndKeyboardInput() {
        QFETCH(bool, dark); QFETCH(double, fontScale);
        QWidget workspace; const auto css = workspaceStyle(dark, fontScale); QVERIFY(!css.isEmpty());
        workspace.setStyleSheet(css);
        DirectoryDialog browser(program(), "arch", "~", &workspace); browser.resize(500, 540);
        browser.show(); browser.activateWindow(); QApplication::setActiveWindow(&browser);
        auto *open = browser.findChild<QPushButton *>("newFolder"); QTRY_VERIFY(open->isEnabled());
        const auto minimum = browser.minimumSize();
        auto *list = browser.findChild<QListWidget *>("folderList"); const int listMinimum = list->minimumHeight();
        open->click();
        auto *name = browser.findChild<QLineEdit *>("inlineFolderName"); QVERIFY(name);
        QTRY_VERIFY(name->hasFocus()); QTest::keyClicks(name, "Wgj9-folder");
        QCOMPARE(name->text(), QString("Wgj9-folder"));
        captureEditor(browser, "typed");
        const auto contents = editorContents(name);
        QVERIFY2(contents.height() >= name->fontMetrics().height(), qPrintable(QString("Editor %1, contents %2, font height %3, hint %4")
            .arg(name->height()).arg(contents.height()).arg(name->fontMetrics().height()).arg(name->sizeHint().height())));
        QVERIFY2(editorInk(name) > name->fontMetrics().height() * 3, "Typed glyphs must paint inside the real styled editor, beyond a cursor alone");
        auto *create = browser.findChild<QPushButton *>("createInlineFolder");
        auto *cancel = browser.findChild<QPushButton *>("cancelInlineFolder"); QVERIFY(cancel);
        for (auto *control : QList<QWidget *>{name, create, cancel}) {
            const QRect rect(control->mapTo(list->viewport(), QPoint()), control->size());
            QVERIFY2(list->viewport()->rect().contains(rect), "The entire styled input and its actions must remain visible in the list viewport");
        }
        QVERIFY(!name->geometry().intersects(create->geometry()));
        QVERIFY(!create->geometry().intersects(cancel->geometry()));
        const QRect listRect(list->mapTo(&browser, QPoint()), list->size());
        for (auto *control : QList<QWidget *>{open, browser.findChild<QPushButton *>("newFolderOptions"),
                                             browser.findChild<QCheckBox *>(), browser.findChild<QPushButton *>("chooseFolder")}) {
            const QRect rect(control->mapTo(&browser, QPoint()), control->size());
            QVERIFY(browser.rect().contains(rect)); QVERIFY(!listRect.intersects(rect));
            QVERIFY(control->width() >= control->minimumSizeHint().width());
        }
        QVERIFY(browser.width() <= browser.screen()->availableGeometry().width());
        QVERIFY(browser.height() <= browser.screen()->availableGeometry().height());
        QTest::keyClick(name, Qt::Key_Backspace); QTest::keyClicks(name, "x");
        QCOMPARE(name->text(), QString("Wgj9-foldex"));
        QTest::keyClick(name, Qt::Key_Escape); QVERIFY(browser.isVisible()); QCOMPARE(mutations(), 0);
        QCOMPARE(browser.minimumSize(), minimum); QCOMPARE(list->minimumHeight(), listMinimum);
    }
    void inlinePasteAndEnterKeepExactName_data() {
        QTest::addColumn<QString>("host");
        QTest::newRow("local-fake") << QString(); QTest::newRow("remote-fake") << QString("arch");
    }
    void inlinePasteAndEnterKeepExactName() {
        // The offscreen clipboard belongs only to this private test process.
        // Do not read or overwrite a person's Cocoa/X11/Wayland clipboard.
        if (QGuiApplication::platformName() != "offscreen") QSKIP("Private clipboard interaction uses the offscreen platform only");
        QFETCH(QString, host);
        QWidget workspace; workspace.setStyleSheet(workspaceStyle(true, 1.0));
        DirectoryDialog browser(program(), host, "~", &workspace); browser.show(); browser.activateWindow(); QApplication::setActiveWindow(&browser);
        auto *open = browser.findChild<QPushButton *>("newFolder"); QTRY_VERIFY(open->isEnabled()); open->click();
        auto *name = browser.findChild<QLineEdit *>("inlineFolderName"); QVERIFY(name); QTRY_VERIFY(name->hasFocus());
        const auto leaf = QString::fromUtf8("новая ' $(literal)");
        QApplication::clipboard()->setText(leaf); QTest::keySequence(name, QKeySequence(QKeySequence::Paste));
        QCOMPARE(name->text(), leaf); QVERIFY(editorInk(name) > name->fontMetrics().height() * 3);
        QTest::keyClick(name, Qt::Key_Return);
        QTRY_COMPARE(browser.directory(), "/fixture/projects/" + leaf); QCOMPARE(mutations(), 1); QVERIFY(browser.isVisible());
        for (const auto &call : calls()) if (call.toObject()["args"].toArray().contains("--create")) {
            QCOMPARE(call.toObject()["host"].toString(), host);
            QCOMPARE(call.toObject()["args"].toArray().at(2).toString(), "/fixture/projects/" + leaf);
        }
        QApplication::clipboard()->clear();
    }
    void styledCreationControlsPreserveTheAlternateDialogAndCancel() {
        QWidget workspace; workspace.setStyleSheet(workspaceStyle(true, 1.0));
        DirectoryDialog browser(program(), "arch", "~", &workspace); browser.show();
        auto *open = browser.findChild<QPushButton *>("newFolder"); QTRY_VERIFY(open->isEnabled());
        auto *alternate = browser.findChild<QPushButton *>("newFolderOptions"); QVERIFY(alternate);
        QCOMPARE(alternate->accessibleName(), QString("New folder dialog…")); QVERIFY(!alternate->text().isEmpty());
        QTest::mouseClick(open, Qt::LeftButton);
        auto *name = browser.findChild<QLineEdit *>("inlineFolderName"); QVERIFY(name);
        QTest::keyClicks(name, "kept-name");
        bool checked = false;
        QTimer::singleShot(0, &browser, [&] {
            auto *dialog = browser.findChild<QDialog *>("createFolderDialog"); QVERIFY(dialog);
            QCOMPARE(dialog->findChild<QLineEdit *>("newFolderName")->text(), QString("kept-name"));
            checked = true; dialog->reject();
        });
        QTest::mouseClick(alternate, Qt::LeftButton); QVERIFY(checked); QCOMPARE(mutations(), 0);
        QTest::mouseClick(open, Qt::LeftButton);
        auto *cancel = browser.findChild<QPushButton *>("cancelInlineFolder"); QVERIFY(cancel);
        QTest::mouseClick(cancel, Qt::LeftButton);
        QTRY_VERIFY(!browser.findChild<QLineEdit *>("inlineFolderName")); QCOMPARE(mutations(), 0);
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
