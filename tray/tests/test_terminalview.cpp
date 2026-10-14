#include "FakeCli.h"
#include "PtyProcess.h"
#include "TerminalScreen.h"
#include "TerminalView.h"
#include "WindowsCommandLine.h"

#include <QFile>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QProcess>
#include <QPaintEvent>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>
#include <cmath>

namespace {
class PaintRegions : public QObject {
public:
  QRegion region;
  bool eventFilter(QObject *, QEvent *event) override {
    if (event->type() == QEvent::Paint)
      region += static_cast<QPaintEvent *>(event)->region();
    return false;
  }
};
QByteArray combined(const QSignalSpy &spy) {
  QByteArray result;
  for (const auto &entry : spy)
    result += entry.at(0).toByteArray();
  return result;
}
QString quote(QString text) {
  text.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QLatin1Char('\'') + text + QLatin1Char('\'');
}
bool executable(const QString &path, const QByteArray &contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(contents) == contents.size() &&
         file.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                             QFile::ExeOwner);
}
// Child programs of the PTY tests: /bin/sh on Unix, the test helper's steps in a
// pseudoconsole on Windows (support/TestHelper.cpp).
struct Child {
  QString program;
  QStringList arguments;
};
Child child(const QString &script, const QStringList &steps) {
#ifdef Q_OS_WIN
  Q_UNUSED(script);
  return {QStringLiteral(ZERUS_TEST_HELPER), steps};
#else
  Q_UNUSED(steps);
  return {QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script}};
#endif
}
// What a terminal shows: ConPTY re-renders output, so Windows checks the screen.
QString rendered(const QByteArray &bytes) {
  TerminalScreen screen;
  screen.resize(1200, 600);
  screen.show();
  QTest::qWait(10);
  screen.feed(bytes);
  return screen.screenText();
}
Qt::KeyboardModifier physicalControl() {
#ifdef Q_OS_MACOS
  return Qt::MetaModifier;
#else
  return Qt::ControlModifier;
#endif
}
} // namespace

class TerminalViewTest : public QObject {
  Q_OBJECT
private:
  QTemporaryDir m_dir;
  QString m_socket;
  int m_socketId = 0;
  QByteArray tmux(const QStringList &arguments, int *code = nullptr) {
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("TMUX"));
    environment.insert(QStringLiteral("TERM"),
                       QStringLiteral("xterm-256color"));
    process.setProcessEnvironment(environment);
    process.start(QStandardPaths::findExecutable(QStringLiteral("tmux")),
                  QStringList{QStringLiteral("-S"), m_socket,
                              QStringLiteral("-f"),
                              QStringLiteral("/dev/null")} +
                      arguments);
    if (!process.waitForFinished(5000)) {
      process.kill();
      process.waitForFinished();
      if (code)
        *code = -1;
      return {};
    }
    if (code)
      *code = process.exitCode();
    return process.readAllStandardOutput().trimmed();
  }
  QString wrapper() {
    const QString path = m_dir.filePath(QStringLiteral("attach"));
    const QString script =
        QStringLiteral("#!/bin/sh\n[ \"$1\" = a ] && [ \"$3\" = --existing ] "
                       "|| exit 91\n[ -z \"$TMUX\" ] && [ -z \"$TMUX_PANE\" ] "
                       "&& [ \"$HGS_TAB\" = 0 ] || exit 92\nexec %1 -S %2 -f "
                       "/dev/null attach-session -t \"=$2\"\n")
            .arg(quote(QStandardPaths::findExecutable(QStringLiteral("tmux"))),
                 quote(m_socket));
    return executable(path, script.toUtf8()) ? path : QString();
  }

private slots:
  void init() {
    QVERIFY(m_dir.isValid());
    // A killed server can still be shutting down after kill-server returns.
    // Give each test its own endpoint rather than racing that old server.
    m_socket = m_dir.filePath(QStringLiteral("tmux-%1.sock").arg(++m_socketId));
  }
  void cleanup() {
    if (QFile::exists(m_socket))
      tmux({QStringLiteral("kill-server")});
  }

  void ansiColorsCursorAndUnicode() {
    TerminalScreen screen;
    screen.resize(640, 320);
    screen.show();
    QTest::qWait(10);
    screen.feed("\x1b[2J\x1b[H\x1b[38;2;12;34;56m\x1b[48;2;70;80;90mA\x1b[0m");
    const auto cell = screen.screenCell(0, 0);
    QCOMPARE(cell.chars[0], uint32_t('A'));
    QCOMPARE(int(cell.fg.rgb.red), 12);
    QCOMPARE(int(cell.fg.rgb.green), 34);
    QCOMPARE(int(cell.fg.rgb.blue), 56);
    QCOMPARE(int(cell.bg.rgb.red), 70);
    screen.feed(QString::fromUtf8("\x1b[2;3HПривет 界 é").toUtf8());
    QVERIFY(screen.screenText().contains(QString::fromUtf8("Привет 界 é")));
    QCOMPARE(screen.screenCell(1, 9).chars[0], uint32_t(0x754c));
    QCOMPARE(int(screen.screenCell(1, 9).width), 2);
    screen.feed("\x1b[2;3H\x1b[K");
    QVERIFY(!screen.screenText().contains(QString::fromUtf8("Привет")));
    QVERIFY(!screen.grab().isNull());
  }

  void incrementalPaintingMatchesFullRepaint() {
    TerminalScreen screen;screen.resize(1280,800);screen.show();QTest::qWait(20);
    screen.feed("\x1b[?12l\x1b[?25l");
    QByteArray fill;for(int row=1;row<=screen.terminalSize().height();++row)
      fill+="\x1b["+QByteArray::number(row)+";1H"+QByteArray(screen.terminalSize().width()-1,'X');
    screen.feed(fill);QTest::qWait(20);
    PaintRegions paints;screen.viewport()->installEventFilter(&paints);
    const QList<QByteArray> changes={
      "\x1b[2;2HA", // one changed cell in a large grid
      QString::fromUtf8("\x1b[4;5H界é").toUtf8(),
      "\x1b[4;6H ", // overwrite the trailing half of a wide glyph
      "\x1b[4;5H\x1b[K", // erase line including combining characters
      "\x1b[?25h\x1b[2;2H", // show and move cursor
      "\x1b[15;60H", // separated old/new cursor damage
      "\x1b[?5h", // reverse video invalidates the whole screen
      "\x1b[?5l\x1b[2;4r\x1b[2;1H\x1b[M", // scroll a partial region
      "\x1b[r\x1b[?1049hnew buffer",
      "\x1b[?1049l"
    };
    for(int index=0;index<changes.size();++index){
      QImage incremental=screen.viewport()->grab().toImage();paints.region={};
      screen.feed(changes[index]);QCoreApplication::sendPostedEvents(nullptr,QEvent::UpdateRequest);QApplication::processEvents();
      const auto dirty=paints.region;QVERIFY2(!dirty.isEmpty(),qPrintable(QString::number(index)));
      if(index==0||index==5){qint64 area=0;for(const auto &rect:dirty)area+=rect.width()*rect.height();QVERIFY(area<screen.viewport()->width()*screen.viewport()->height()/10);}
      screen.viewport()->render(&incremental,dirty.boundingRect().topLeft(),dirty,QWidget::DrawWindowBackground);
      const auto full=screen.viewport()->grab().toImage();QVERIFY2(incremental==full,qPrintable(QString("Incremental frame differs at change %1").arg(index)));
    }
    screen.viewport()->removeEventFilter(&paints);
  }

  void alternateScreenRestoresMainBuffer() {
    TerminalScreen screen;
    screen.feed("main\x1b[?1049h\x1b[Halternate");
    QVERIFY(screen.screenText().startsWith(QStringLiteral("alternate")));
    screen.feed("\x1b[?1049l");
    QVERIFY(screen.screenText().startsWith(QStringLiteral("main")));
    QVERIFY(!screen.screenText().contains(QStringLiteral("alternate")));
  }

  void parentFontChangesRefreshGridWithoutResetting() {
    QWidget parent;
    auto *layout = new QVBoxLayout(&parent);
    auto *screen = new TerminalScreen(&parent);
    layout->addWidget(screen);
    parent.resize(720, 360);
    parent.show();
    QTest::qWait(10);
    screen->feed("preserved context\x1b[?25l");
    const auto before = screen->terminalSize();
    parent.setStyleSheet(QStringLiteral("QWidget { font-size: 22px; }"));
    QTest::qWait(10);
    const QFontMetricsF metrics(screen->font(), screen->viewport());
    const int width =
        int(std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const int height = int(std::ceil(metrics.height())) + 2;
    QCOMPARE(screen->terminalSize(),
             QSize((screen->viewport()->width() - 16) / width,
                   (screen->viewport()->height() - 16) / height));
    QVERIFY(screen->terminalSize().width() < before.width());
    QVERIFY(screen->screenText().contains(QStringLiteral("preserved context")));
    screen->feed("\x1b[2;3H\x1b[48;2;17;101;203m \x1b[0m");
    const auto image = screen->viewport()->grab().toImage();
    const qreal scale = image.devicePixelRatio();
    const auto pixel = [&image, scale](int x, int y) {
      return image.pixelColor(int(x * scale), int(y * scale));
    };
    QCOMPARE(pixel(8 + 2 * width, 8 + height), QColor(17, 101, 203));
    QCOMPARE(pixel(8 + 3 * width - 1, 8 + 2 * height - 1),
             QColor(17, 101, 203));
    QVERIFY(pixel(8 + 3 * width, 8 + height) != QColor(17, 101, 203));
    parent.setStyleSheet(QStringLiteral("QWidget { font-size: 13px; }"));
    QTest::qWait(10);
    QVERIFY(screen->terminalSize().width() > before.width());
    QVERIFY(screen->screenText().contains(QStringLiteral("preserved context")));
    QEvent screenChange(QEvent::ScreenChangeInternal);
    QApplication::sendEvent(screen, &screenChange);
    QVERIFY(screen->screenText().contains(QStringLiteral("preserved context")));
  }

  void contentScaleEnlargesCellsOnly() {
    // SessionsWindow sets this base font on every workspace widget.
    QWidget workspace;
    workspace.setStyleSheet(QStringLiteral("QWidget { font-size:13px; }"));
    auto *layout = new QVBoxLayout(&workspace);
    auto *view = new TerminalView;
    layout->addWidget(view);
    view->setTheme(false);
    workspace.resize(720, 420);
    workspace.show();
    QTest::qWait(10);
    auto *screen = view->terminalScreen();
    auto *status = view->findChild<QWidget *>(QStringLiteral("terminalStatus"));
    QVERIFY(status);
    screen->feed("preserved context");
    QCOMPARE(QFontInfo(screen->font()).pixelSize(), 13);
    const auto before = screen->terminalSize();
    const auto viewportBefore = screen->viewport()->size();
    const int statusFont = QFontInfo(status->font()).pixelSize();
    QSignalSpy resized(screen, &TerminalScreen::sizeChanged);
    view->setContentScale(2.0);
    QTest::qWait(10);
    QCOMPARE(QFontInfo(screen->font()).pixelSize(), 26);
    QCOMPARE(screen->viewport()->size(), viewportBefore);
    // Native glyph advances round up to whole cells: a 13px cell can become
    // 25px at double font size, so the column count need not halve exactly.
    const QFontMetricsF metrics(screen->font(), screen->viewport());
    const int width = int(std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const int height = int(std::ceil(metrics.height())) + 2;
    QCOMPARE(screen->terminalSize(),
             QSize((screen->viewport()->width() - 16) / width,
                   (screen->viewport()->height() - 16) / height));
    QVERIFY(screen->terminalSize().width() < before.width());
    QVERIFY(screen->terminalSize().height() < before.height());
    QCOMPARE(resized.last().at(0).toSize(), screen->terminalSize());
    QCOMPARE(QFontInfo(status->font()).pixelSize(), statusFont);
    QVERIFY(screen->screenText().contains(QStringLiteral("preserved context")));
    view->setTheme(true);
    QCOMPARE(QFontInfo(screen->font()).pixelSize(), 26);
    view->setContentScale(1.0);
    QTest::qWait(10);
    QCOMPARE(QFontInfo(screen->font()).pixelSize(), 13);
    QCOMPARE(screen->terminalSize(), before);
  }
  void scrollbackAndResize() {
    TerminalScreen screen;
    screen.resize(420, 160);
    screen.show();
    QTest::qWait(10);
    QSignalSpy sizes(&screen, &TerminalScreen::sizeChanged);
    for (int line = 0; line < 40; ++line)
      screen.feed(QByteArray::number(line) + "\r\n");
    QVERIFY(screen.scrollbackLines() > 20);
    QCOMPARE(screen.verticalScrollBar()->value(),
             screen.verticalScrollBar()->maximum());
    screen.verticalScrollBar()->setValue(0);
    screen.feed("tail\r\n");
    QCOMPARE(screen.verticalScrollBar()->value(), 0);
    const auto original = screen.terminalSize();
    screen.resize(620, 240);
    QTest::qWait(10);
    QVERIFY(screen.terminalSize().width() > original.width());
    QVERIFY(!sizes.isEmpty());
    screen.reset();
    QCOMPARE(screen.scrollbackLines(), 0);
  }

  void nativeKeysAndShortcutOverride() {
    TerminalScreen screen;
    screen.show();
    screen.setFocus();
    QSignalSpy bytes(&screen, &TerminalScreen::output);
    QKeyEvent overrideEvent(QEvent::ShortcutOverride, Qt::Key_Escape,
                            Qt::NoModifier);
    overrideEvent.ignore();
    QApplication::sendEvent(&screen, &overrideEvent);
    QVERIFY(overrideEvent.isAccepted());
    QKeyEvent ctrlOverride(QEvent::ShortcutOverride, Qt::Key_R,
                           physicalControl());
    ctrlOverride.ignore();
    QApplication::sendEvent(&screen, &ctrlOverride);
    QVERIFY(ctrlOverride.isAccepted());
    QTest::keyClick(&screen, Qt::Key_C, physicalControl());
    QCOMPARE(combined(bytes), QByteArray("\x03", 1));
    bytes.clear();
    QTest::keyClick(&screen, Qt::Key_Up);
    QCOMPARE(combined(bytes), QByteArray("\x1b[A"));
    bytes.clear();
    screen.feed("\x1b[?1h");
    QTest::keyClick(&screen, Qt::Key_Up);
    QCOMPARE(combined(bytes), QByteArray("\x1bOA"));
    bytes.clear();
    QTest::keyClick(&screen, Qt::Key_F5);
    QCOMPARE(combined(bytes), QByteArray("\x1b[15~"));
    bytes.clear();
    QTest::keyClick(&screen, Qt::Key_Escape);
    QCOMPARE(combined(bytes), QByteArray("\x1b", 1));
    bytes.clear();
    QTest::keyClick(&screen, Qt::Key_Tab);
    QCOMPARE(combined(bytes), QByteArray("\t"));
    bytes.clear();
    QTest::keyClick(&screen, Qt::Key_Tab, Qt::ShiftModifier);
    QCOMPARE(combined(bytes), QByteArray("\x1b[Z"));
    bytes.clear();
    QTest::keyClick(&screen, Qt::Key_BracketLeft, physicalControl());
    QCOMPARE(combined(bytes), QByteArray("\x1b", 1));
  }

  void bracketedPasteAndInputMethod() {
    TerminalScreen screen;
    QSignalSpy bytes(&screen, &TerminalScreen::output);
    screen.feed("\x1b[?2004h");
    screen.pasteText(QString::fromUtf8("hello\nмир"));
    QCOMPARE(bytes.size(), 1);
    QCOMPARE(combined(bytes), QByteArray("\x1b[200~") +
                                  QString::fromUtf8("hello\rмир").toUtf8() +
                                  "\x1b[201~");
    bytes.clear();
    QInputMethodEvent input;
    input.setCommitString(QString::fromUtf8("界"));
    QApplication::sendEvent(&screen, &input);
    QCOMPARE(combined(bytes), QString::fromUtf8("界").toUtf8());
  }

  void pasteCannotEscapeItsFrame() {
    TerminalScreen screen;
    QSignalSpy bytes(&screen, &TerminalScreen::output);
    screen.feed("\x1b[?2004h");
    screen.pasteText(QString::fromUtf8("safe\x1b[201~\rhidden\x03\x7f\ttext") +
                     QChar(0x9b));
    QCOMPARE(bytes.size(), 1);
    QCOMPARE(combined(bytes),
             QByteArray("\x1b[200~safe[201~\rhidden\ttext\x1b[201~"));
  }

  void oversizedPasteIsRejectedAsAWhole() {
    TerminalScreen screen;
    PtyProcess process;
    QSignalSpy bytes(&process, &PtyProcess::output),
        errors(&process, &PtyProcess::errorOccurred);
    QSignalSpy framed(&screen, &TerminalScreen::output);
    connect(&screen, &TerminalScreen::output, &process, &PtyProcess::write);
    const auto noEcho = child(QStringLiteral("stty -echo; printf 'READY\\n'; read "
                                             "line; printf 'GOT:%s\\n' \"$line\""),
                              {"noecho", "line", "READY", "read", "got"});
    QVERIFY(process.start(noEcho.program, noEcho.arguments,
                          QProcessEnvironment::systemEnvironment(), QSize(80, 24)));
    QTRY_VERIFY(combined(bytes).contains("READY"));
    screen.feed("\x1b[?2004h");
    screen.pasteText(QString(4 * 1024 * 1024, QLatin1Char('x')));
    QCOMPARE(framed.size(), 1);
    QVERIFY(combined(framed).startsWith("\x1b[200~"));
    QVERIFY(combined(framed).endsWith("\x1b[201~"));
    QCOMPARE(errors.size(), 1);
    process.write("ok\r");
    QTRY_VERIFY(combined(bytes).contains("GOT:ok"));
    QVERIFY(!combined(bytes).contains("200~"));
    QVERIFY(!combined(bytes).contains("xxxx"));
  }

  void smoothWheelAccumulatesSmallDeltas() {
    TerminalScreen screen;
    screen.resize(420, 160);
    screen.show();
    QTest::qWait(10);
    for (int line = 0; line < 40; ++line)
      screen.feed("line\r\n");
    const int bottom = screen.verticalScrollBar()->value();
    auto wheel = [&screen](QPoint pixel, QPoint angle) {
      QWheelEvent event(QPointF(20, 20), QPointF(20, 20), pixel, angle,
                        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(screen.viewport(), &event);
    };
    for (int n = 0; n < 4; ++n)
      wheel({}, QPoint(0, 30));
    QCOMPARE(screen.verticalScrollBar()->value(), bottom - 3);
    for (int n = 0; n < 4; ++n)
      wheel(QPoint(0, 10), {});
    QCOMPARE(screen.verticalScrollBar()->value(), bottom - 6);
    QSignalSpy bytes(&screen, &TerminalScreen::output);
    screen.feed("\x1b[?1000h\x1b[?1006h");
    for (int n = 0; n < 3; ++n)
      wheel({}, QPoint(0, 30));
    QVERIFY(bytes.isEmpty());
    wheel({}, QPoint(0, 30));
    QVERIFY(combined(bytes).contains("\x1b[<64;"));
  }

  void selectionHasPlainUnicodeText() {
    TerminalScreen screen;
    screen.resize(640, 320);
    screen.show();
    QTest::qWait(10);
    screen.feed(QString::fromUtf8("Привет world").toUtf8());
    QTest::mouseDClick(screen.viewport(), Qt::LeftButton, Qt::NoModifier,
                       QPoint(12, 14));
    QCOMPARE(screen.selectedText(), QString::fromUtf8("Привет"));
    // We deliberately read the selection, never the real desktop clipboard.
  }

  void terminalTitleAndClipboardEscapeIgnored() {
    TerminalScreen screen;
    QSignalSpy titles(&screen, &TerminalScreen::titleChanged),
        bytes(&screen, &TerminalScreen::output);
    screen.feed("\x1b]2;test terminal\x07");
    QCOMPARE(titles.last().at(0).toString(), QStringLiteral("test terminal"));
    screen.feed("\x1b]52;c;aGVsbG8=\x07visible");
    QVERIFY(screen.screenText().startsWith(QStringLiteral("visible")));
    QVERIFY(bytes.isEmpty());
  }

  void ptyRoundTripAndResize() {
    PtyProcess process;
    QSignalSpy bytes(&process, &PtyProcess::output),
        finished(&process, &PtyProcess::finished);
    const auto probe = child(QStringLiteral("printf 'READY\\n'; read line; stty "
                                            "size; printf 'GOT:%s\\n' \"$line\""),
                             {"line", "READY", "read", "size", "got"});
    QVERIFY(process.start(probe.program, probe.arguments,
                          QProcessEnvironment::systemEnvironment(), QSize(81, 27)));
    QTRY_VERIFY(combined(bytes).contains("READY"));
    process.resize(QSize(93, 31));
    process.write("hello\r");
    QTRY_VERIFY(!finished.isEmpty());
    QCOMPARE(finished.last().at(0).toInt(), 0);
#ifdef Q_OS_WIN
    QVERIFY2(rendered(combined(bytes)).contains("31 93"),
             qPrintable(rendered(combined(bytes)) + QString::fromLatin1(combined(bytes).toPercentEncoding())));
    QVERIFY(rendered(combined(bytes)).contains("GOT:hello"));
    QVERIFY(!process.isRunning());
#else
    QVERIFY(combined(bytes).contains("31 93"));
    QVERIFY(combined(bytes).contains("GOT:hello"));
#endif
  }

  // The pseudoconsole command line must give the child the argv QProcess would.
  void windowsCommandLineMatchesQProcessQuoting() {
    using WindowsCommandLine::build;
    QCOMPARE(build("C:/Program Files/Zerus/hgs.exe", {"@mac", "a", "codex/p/my name"}),
             QString("\"C:\\Program Files\\Zerus\\hgs.exe\"  @mac a \"codex/p/my name\""));
    QCOMPARE(build("hgs.exe", {"", "say \"hi\"", "C:\\dir\\", "C:\\my dir\\", "a\\\"b"}),
             QString("hgs.exe  \"\" \"say \\\"hi\\\"\" C:\\dir\\ \"C:\\my dir\"\\ a\\\\\\\"b"));
  }

#ifdef Q_OS_WIN
  // Every argument byte reaches the attached program, as through QProcess.
  void pseudoconsoleArgumentsSurviveQuoting() {
    const QString script = m_dir.filePath(QStringLiteral("argv"));
    QVERIFY(FakeCli::install(script, "#!/usr/bin/env python3\nimport os,sys\n"
        "open(__file__+'.json','w').write(repr(sys.argv[1:]))\n"));
    const QStringList arguments{QString::fromUtf8("codex/проект/my name"), "say \"hi\"", "C:\\dir\\",
                                "C:\\my dir\\", "a\\\"b", "$HOME `x`", "tab\there"};
    PtyProcess process;
    QSignalSpy finished(&process, &PtyProcess::finished);
    QVERIFY(process.start(FakeCli::program(script), arguments,
                          QProcessEnvironment::systemEnvironment(), QSize(80, 24)));
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 20000);
    QFile result(script + QStringLiteral(".json"));
    QVERIFY(result.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(result.readAll()),
             QString::fromUtf8("['codex/проект/my name', 'say \"hi\"', 'C:\\\\dir\\\\', "
                               "'C:\\\\my dir\\\\', 'a\\\\\"b', '$HOME `x`', 'tab\\there']"));
  }

  // Disconnect ends the attach client and its descendants: PtyProcess's
  // kill-on-close job is the only thing that reaches this grandchild, which has
  // no console and runs in no job of the client's own.
  void stopEndsTheWholeClient() {
    const QString pidFile = m_dir.filePath(QStringLiteral("grandchild.pid"));
    QFile::remove(pidFile);
    PtyProcess process;
    QVERIFY(process.start(QStringLiteral(ZERUS_TEST_HELPER), {"spawn", pidFile, "sleep", "60"},
                          QProcessEnvironment::systemEnvironment(), QSize(80, 24)));
    QTRY_VERIFY_WITH_TIMEOUT(QFile(pidFile).size() > 0, 20000);
    QFile file(pidFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const qint64 grandchild = file.readAll().trimmed().toLongLong();
    const auto cleanup = qScopeGuard([grandchild] { FakeCli::terminate(grandchild); });
    const qint64 client = process.processId();
    QVERIFY(FakeCli::processAlive(grandchild));
    QVERIFY(FakeCli::processAlive(client));
    QTest::qWait(300);
    QVERIFY(FakeCli::processAlive(grandchild));
    process.stop();
    QVERIFY(!process.isRunning());
    QVERIFY(!FakeCli::processAlive(client));
    QTRY_VERIFY_WITH_TIMEOUT(!FakeCli::processAlive(grandchild), 5000);
  }
#endif

  void noAutomaticAttachAndGuardedArguments() {
    const QString script = m_dir.filePath(QStringLiteral("capture"));
    QVERIFY(FakeCli::install(
        script,
        "#!/usr/bin/env python3\nimport os,sys\nfor a in sys.argv[1:]:print('ARG:'+a)\n"
        "print('ENV:%s:%s:%s'%(os.environ.get('TMUX','unset'),os.environ.get('TMUX_PANE','unset'),"
        "os.environ.get('HGS_TAB','')),flush=True)\nsys.stdin.buffer.read()\n"));
    const QString path = FakeCli::program(script);
    TerminalView view;
    view.resize(700, 340);
    view.show();
    view.setSession(path, QStringLiteral("mac"),
                    QStringLiteral("codex/project/my name"),
                    QStringLiteral("run-token"));
    QVERIFY(!view.isConnected());
    view.setAvailable(false, QStringLiteral("Offline"));
    view.connectSession();
    QVERIFY(!view.isConnected());
    view.setAvailable(true);
    QVERIFY(!view.isConnected());
    view.connectSession();
    QVERIFY(view.isConnected());
    // The fixture prints its ENV line last, after every argument, so one snapshot
    // taken once that line is on screen holds all of them.
    QTRY_VERIFY(view.terminalScreen()->screenText().contains(
        QStringLiteral("ENV:")));
    const auto text = view.terminalScreen()->screenText();
    QVERIFY(text.contains(QStringLiteral("ARG:--existing")));
    QVERIFY(text.contains(QStringLiteral("ARG:@mac")));
    QVERIFY(text.contains(QStringLiteral("ARG:codex/project/my name")));
    QVERIFY(text.contains(QStringLiteral("ARG:--run-id")));
    QVERIFY(text.contains(QStringLiteral("ARG:run-token")));
    QVERIFY(text.contains(QStringLiteral("ENV:unset:unset:0")));
    view.setSession(path, QStringLiteral("mac"),
                    QStringLiteral("codex/project/renamed"),
                    QStringLiteral("run-token"));
    QVERIFY(view.isConnected());
    view.setSession(path, QStringLiteral("mac"),
                    QStringLiteral("codex/project/renamed"),
                    QStringLiteral("replacement"));
    QVERIFY(!view.isConnected());
  }

  void delayedRunTokenDoesNotDetach() {
    const QString script = m_dir.filePath(QStringLiteral("cat"));
    QVERIFY(FakeCli::install(script, "#!/usr/bin/env python3\nimport sys\nsys.stdin.buffer.read()\n"));
    const QString path = FakeCli::program(script);
    TerminalView view;
    view.setSession(path, {}, QStringLiteral("codex/p/name"));
    view.connectSession();
    QVERIFY(view.isConnected());
    view.setSession(path, {}, QStringLiteral("codex/p/name"),
                    QStringLiteral("newly-known"));
    QVERIFY(view.isConnected());
    view.setSession(path, {}, QStringLiteral("codex/p/name"));
    QVERIFY(view.isConnected());
    view.setAvailable(false, QStringLiteral("Ended"));
    QVERIFY(!view.isConnected());
  }

  void tmuxDetachReconnectPreservesAgent() {
    if (QStandardPaths::findExecutable(QStringLiteral("tmux")).isEmpty())
      QSKIP("tmux not installed");
    const QString name = QStringLiteral("codex/test/live");
    int code = -1;
    tmux({QStringLiteral("new-session"), QStringLiteral("-d"),
          QStringLiteral("-s"), name, QStringLiteral("exec cat")},
         &code);
    QCOMPARE(code, 0);
    const auto pid =
        tmux({QStringLiteral("display-message"), QStringLiteral("-p"),
              QStringLiteral("-t"), name, QStringLiteral("#{pane_pid}")});
    QVERIFY(!pid.isEmpty());
    const QString path = wrapper();
    QVERIFY(!path.isEmpty());
    {
      TerminalView view;
      view.resize(700, 360);
      view.show();
      view.setSession(path, {}, name);
      view.connectSession();
      QTRY_COMPARE(tmux({QStringLiteral("display-message"),
                         QStringLiteral("-p"), QStringLiteral("-t"), name,
                         QStringLiteral("#{session_attached}")}),
                   QByteArray("1"));
      view.pasteText(QStringLiteral("embedded hello\n"));
      QTRY_VERIFY(view.terminalScreen()->screenText().contains(
          QStringLiteral("embedded hello")));
      view.resize(820, 420);
      QTest::qWait(100);
      const int columns = view.terminalScreen()->terminalSize().width();
      QTRY_COMPARE(
          tmux({QStringLiteral("display-message"), QStringLiteral("-p"),
                QStringLiteral("-t"), name, QStringLiteral("#{window_width}")})
              .toInt(),
          columns);
      view.disconnectSession();
      QTRY_COMPARE(tmux({QStringLiteral("display-message"),
                         QStringLiteral("-p"), QStringLiteral("-t"), name,
                         QStringLiteral("#{session_attached}")}),
                   QByteArray("0"));
      QCOMPARE(
          tmux({QStringLiteral("display-message"), QStringLiteral("-p"),
                QStringLiteral("-t"), name, QStringLiteral("#{pane_pid}")}),
          pid);
      view.connectSession();
      QTRY_COMPARE(tmux({QStringLiteral("display-message"),
                         QStringLiteral("-p"), QStringLiteral("-t"), name,
                         QStringLiteral("#{session_attached}")}),
                   QByteArray("1"));
    }
    QTRY_COMPARE(tmux({QStringLiteral("display-message"), QStringLiteral("-p"),
                       QStringLiteral("-t"), name,
                       QStringLiteral("#{session_attached}")}),
                 QByteArray("0"));
    QCOMPARE(tmux({QStringLiteral("display-message"), QStringLiteral("-p"),
                   QStringLiteral("-t"), name, QStringLiteral("#{pane_pid}")}),
             pid);
  }

  void missingTargetCreatesNoSession() {
    if (QStandardPaths::findExecutable(QStringLiteral("tmux")).isEmpty())
      QSKIP("tmux not installed");
    tmux({QStringLiteral("new-session"), QStringLiteral("-d"),
          QStringLiteral("-s"), QStringLiteral("sentinel"),
          QStringLiteral("exec cat")});
    TerminalView view;
    view.setSession(wrapper(), {}, QStringLiteral("codex/test/missing"));
    view.connectSession();
    QTRY_VERIFY(!view.isConnected());
    QCOMPARE(tmux({QStringLiteral("list-sessions"), QStringLiteral("-F"),
                   QStringLiteral("#{session_name}")}),
             QByteArray("sentinel"));
    QVERIFY(view.terminalScreen()->screenText().contains(
        QStringLiteral("can't find session")));
  }
};

QTEST_MAIN(TerminalViewTest)
#include "test_terminalview.moc"
