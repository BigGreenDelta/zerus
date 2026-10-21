#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScreen>
#include <QSignalSpy>
#include <QSystemTrayIcon>
#include <QTemporaryDir>

#include <functional>
#include <utility>

#include "AttentionNotifier.h"
#include "ChildProcess.h"
#include "CommandBackend.h"
#include "ConsoleHost.h"
#include "FakeCli.h"
#include "SessionCommands.h"
#include "StartupNotice.h"
#include "SwarmWorkerSupervisor.h"
#include "TrayIcon.h"
#include "WindowsConsole.h"
#include "WindowsTerminalBackend.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

// Windows desktop pieces whose logic is portable: the Windows Terminal and console
// commands (Windows Terminal is never started and no console window is shown:
// that would open windows on the desktop; the console host runs hidden), terminal
// templates, tray-icon notifications, the supervised swarm worker and the note
// while the tray picks its hgs.
namespace {
class RecordingTrayIcon : public TrayIcon {
public:
    void setIcon(const QIcon &) override {}
    void setToolTip(const QString &) override {}
    void setContextMenu(QMenu *) override {}
    void showMessage(const QString &title, const QString &, QSystemTrayIcon::MessageIcon, int) override
    {
        titles << title;
        clicked = {};
    }
    void showClickableMessage(const QString &title, const QString &, QSystemTrayIcon::MessageIcon, int,
                              std::function<void()> callback) override
    {
        titles << title;
        clicked = std::move(callback);
    }
    void showClickableIconMessage(const QString &title, const QString &, const QIcon &icon, int,
                                  std::function<void()> callback) override
    {
        titles << title;
        pictures << icon;
        clicked = std::move(callback);
    }
    void show() override {}
    QStringList titles;
    QList<QIcon> pictures;
    std::function<void()> clicked;
};

QStringList lines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
}

QStringList jsonStrings(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QStringList result;
    for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) result << value.toString();
    return result;
}

// Everything a shell or Windows Terminal would parse, expand or drop.
QStringList awkwardArguments()
{
    return {"@mac", "a", "claude/zerus/fix \"login\" bug", "", "50% %OS% %PATH%", "$HOME $(exit 7) `whoami`",
            "a;b; c", "C:\\my dir\\", "dir\\", "two\\\\", "\\\"quoted\\\"", "tab\there", "line\nbreak",
            QString::fromUtf8("codex/проект/имя"), "it's", "&|<>^!", "--new"};
}
} // namespace

class TestWindowsDesktop : public QObject {
    Q_OBJECT
private slots:
    void terminalStartsTheProgramDirectly();
    void terminalRefusesArgumentsItWouldChange();
    void terminalOpensPowerShellInTheFolder();
    void consoleHostArgumentsCarryTheExactCommand();
    void consoleFallbackHandsTheExactArgumentsToTheHost();
    void shellFallbackStartsTheConsoleInTheFolder();
    void consoleHostRunsTheProgramWithItsExactArguments();
    void templateProgramGetsAConsoleWindow();
    void powerShellTemplateRunsTheExactCommand();
    void openModesMapToWindowsTerminal();
    void notificationClicksReachOnlyCurrentAttention();
    void workerRestartDelayGrowsAndResets();
    void workerIsRestartedAfterExitAndStopsWithTheTray();
    void stoppingEndsARunningWorker();
    void restartedWorkersStayInAKillOnCloseJob();
    void startupNoticeAppearsOnlyForASlowCheck();
};

void TestWindowsDesktop::terminalStartsTheProgramDirectly()
{
    using Backend = WindowsTerminalBackend;
    const QStringList arguments{"@mac", "a", QString::fromUtf8("codex/проект/my name")};
    const QString title = Backend::title(arguments);
    QCOMPARE(title, QString::fromUtf8("Zerus: @mac a codex/проект/my name"));
    QCOMPARE(Backend::terminalArguments(Backend::OpenMode::Auto, title, "C:/Program Files/Zerus/hgs.exe", arguments),
             (QStringList{"-w", "0", "new-tab", "--title", title, "--", "C:\\Program Files\\Zerus\\hgs.exe",
                          "@mac", "a", QString::fromUtf8("codex/проект/my name")}));
    QCOMPARE(Backend::terminalArguments(Backend::OpenMode::Window, title, "C:\\Zerus\\hgs.exe", arguments).mid(0, 3),
             (QStringList{"-w", "new", "new-tab"}));
    // ';' separates wt subcommands; an escaped one reaches the program as ';'.
    const auto escaped = Backend::terminalArguments(Backend::OpenMode::Tab, "Zerus: a;b", "C:\\hgs.exe", {"a", "x;y"});
    QCOMPARE(escaped.at(4), QString("Zerus: a\\;b"));
    QCOMPARE(escaped.last(), QString("x\\;y"));
    QCOMPARE(Backend::title(QStringList{QString(200, 'n')}).size(), 100);
}

void TestWindowsDesktop::terminalRefusesArgumentsItWouldChange()
{
    using Backend = WindowsTerminalBackend;
    for (const QString &argument : {QString("say \"hi\""), QString(), QString("tab\there"),
                                    QString("trailing \\"), QString("line\nbreak"), QString("a\\;b"),
                                    // Windows Terminal expands %NAME% in its command line.
                                    QString("fix %OS% check"), QString("deploy %USERNAME%"), QString("50%")})
        QVERIFY2(Backend::terminalArguments(Backend::OpenMode::Auto, "t", "C:\\hgs.exe", {"a", argument}).isEmpty(),
                 qPrintable(argument));
    // A pair across two arguments expands as well.
    QVERIFY(Backend::terminalArguments(Backend::OpenMode::Auto, "t", "C:\\hgs.exe", {"%OS", "x%"}).isEmpty());
    QVERIFY(Backend::terminalArguments(Backend::OpenMode::Auto, "t", "C:\\Users\\50%off\\hgs.exe", {"a"}).isEmpty());
    // A trailing backslash without spaces is not quoted and survives.
    QVERIFY(!Backend::terminalArguments(Backend::OpenMode::Auto, "t", "C:\\hgs.exe", {"C:\\dir\\"}).isEmpty());
}

void TestWindowsDesktop::terminalOpensPowerShellInTheFolder()
{
    using Backend = WindowsTerminalBackend;
    // PowerShell 7 when installed, otherwise Windows PowerShell without its banner.
    const QStringList pwsh{"C:/Program Files/PowerShell/7/pwsh.exe"};
    QCOMPARE(Backend::shell(pwsh.first()), pwsh);
    const QStringList windowsPowerShell = Backend::shell({});
    QCOMPARE(windowsPowerShell.size(), 2);
    QVERIFY(windowsPowerShell.first().endsWith("/System32/WindowsPowerShell/v1.0/powershell.exe"));
    QCOMPARE(windowsPowerShell.last(), QString("-NoLogo"));
    // The tab starts in the folder (native separators); no command line changes
    // into it, so nothing is quoted.
    QCOMPARE(Backend::shellArguments(Backend::OpenMode::Auto, "C:/work/my project", pwsh),
             (QStringList{"-w", "0", "new-tab", "-d", "C:\\work\\my project", "--title", "Zerus: C:\\work\\my project",
                          "--", "C:\\Program Files\\PowerShell\\7\\pwsh.exe"}));
    QCOMPARE(Backend::shellArguments(Backend::OpenMode::Window, "//server/share/dir", windowsPowerShell),
             (QStringList{"-w", "new", "new-tab", "-d", "\\\\server\\share\\dir", "--title", "Zerus: \\\\server\\share\\dir",
                          "--", QString(windowsPowerShell.first()).replace('/', '\\'), "-NoLogo"}));
    QCOMPARE(Backend::shellArguments(Backend::OpenMode::Tab, "c:\\", pwsh).mid(3, 2), (QStringList{"-d", "c:\\"}));
    // ';' separates wt subcommands, in the folder and the title as well.
    QCOMPARE(Backend::shellArguments(Backend::OpenMode::Auto, "C:/work/a;b", pwsh).mid(3, 4),
             (QStringList{"-d", "C:\\work\\a\\;b", "--title", "Zerus: C:\\work\\a\\;b"}));
    // Windows Terminal would expand %NAME% in the folder as well, or unescape "\;":
    // a console opens these.
    for (const QString &folder : {QString("C:/Users/user/50%off"), QString("C:/work/%OS%"), QString("C:/work/a\\;b"),
                                  QString()})
        QVERIFY2(Backend::shellArguments(Backend::OpenMode::Auto, folder, pwsh).isEmpty(), qPrintable(folder));
    QVERIFY(Backend::shellArguments(Backend::OpenMode::Auto, "C:/work", {"C:/Users/user/50%off/pwsh.exe"}).isEmpty());
    QVERIFY(Backend::shellArguments(Backend::OpenMode::Auto, "C:/work", {}).isEmpty());
}

void TestWindowsDesktop::consoleHostArgumentsCarryTheExactCommand()
{
    const QStringList arguments = awkwardArguments();
    const QStringList host = ConsoleHost::arguments("Zerus: it's", "C:\\Program Files\\Zerus\\hgs.exe", arguments);
    QCOMPARE(host.mid(0, 5), (QStringList{"--console-run", "--title", "Zerus: it's", "--", "C:\\Program Files\\Zerus\\hgs.exe"}));
    const auto request = ConsoleHost::parse(host);
    QVERIFY(request);
    QCOMPARE(request->title, QString("Zerus: it's"));
    QCOMPARE(request->program, QString("C:\\Program Files\\Zerus\\hgs.exe"));
    QCOMPARE(request->arguments, arguments);
    QVERIFY(ConsoleHost::parse(ConsoleHost::arguments("t", "hgs", {}))->arguments.isEmpty());
    for (const QStringList &malformed : {QStringList{"--console-run"}, QStringList{"--console-run", "--title", "t", "--"},
                                         QStringList{"--console-run", "--title", "t", "hgs", "ls"},
                                         QStringList{"--console-run", "--title", "t", "--", ""},
                                         QStringList{"--sessions", "--title", "t", "--", "hgs"}})
        QVERIFY2(!ConsoleHost::parse(malformed), qPrintable(malformed.join(' ')));
    QCOMPARE(ConsoleHost::failureMessage("C:\\Zerus\\hgs.exe", 3),
             QString("hgs exited with code 3. Press Enter to close this window."));
    QCOMPARE(ConsoleHost::failureMessage("C:/Zerus/hgs.EXE", 0xC000013AU),
             QString("hgs exited with code 0xC000013A. Press Enter to close this window."));
}

void TestWindowsDesktop::consoleFallbackHandsTheExactArgumentsToTheHost()
{
#ifndef Q_OS_WIN
    QSKIP("Console windows exist only on Windows");
#else
    // A recording stand-in for hgs-tray.exe --console-run: no window opens.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString host = dir.filePath("host");
    QVERIFY(FakeCli::install(host, "#!/usr/bin/env python3\nimport json,sys\n"
        "open(__file__+'.argv','w',encoding='utf-8').write(json.dumps(sys.argv[1:]))\n"));
    const QStringList arguments = awkwardArguments();
    const QString program = "C:\\Program Files\\Zerus\\hgs.exe";
    using Backend = WindowsTerminalBackend;
    // Windows Terminal would change these arguments, so Auto quietly uses the console.
    Backend automatic(Backend::OpenMode::Auto, "C:\\missing\\wt.exe", FakeCli::program(host));
    QString error;
    QVERIFY2(automatic.openProgram(program, arguments, &error), qPrintable(error));
    QVERIFY(automatic.takeAdvisory().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(QFile(host + ".argv").size() > 0, 20000);
    QCOMPARE(jsonStrings(host + ".argv"), ConsoleHost::arguments(Backend::title(arguments), program, arguments));
    // An explicit tab without Windows Terminal says why a console opened.
    QFile::remove(host + ".argv");
    Backend tab(Backend::OpenMode::Tab, {}, FakeCli::program(host));
    QVERIFY2(tab.openProgram(program, {"@mac", "a", "codex/p"}, &error), qPrintable(error));
    QVERIFY(tab.takeAdvisory().contains("console window"));
    QTRY_VERIFY_WITH_TIMEOUT(QFile(host + ".argv").size() > 0, 20000);
    QCOMPARE(jsonStrings(host + ".argv").mid(4), (QStringList{program, "@mac", "a", "codex/p"}));
    // The detached hosts end right after recording; until then Windows keeps
    // their images, and so the folder, from being removed.
    QTest::qWaitFor([&] { return dir.remove(); }, 20000);
#endif
}

void TestWindowsDesktop::shellFallbackStartsTheConsoleInTheFolder()
{
#ifndef Q_OS_WIN
    QSKIP("Console windows exist only on Windows");
#else
    // A recording stand-in for hgs-tray.exe --console-run: no window opens.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString host = dir.filePath("host");
    QVERIFY(FakeCli::install(host, "#!/usr/bin/env python3\nimport json,os,sys\n"
        "open(__file__+'.cwd','w',encoding='utf-8').write(os.getcwd())\n"
        "open(__file__+'.argv','w',encoding='utf-8').write(json.dumps(sys.argv[1:]))\n"));
    const auto started = [&](const QString &folder) {
        if (!QTest::qWaitFor([&] { return QFile(host + ".argv").size() > 0; }, 20000)) return false;
        QFile cwd(host + ".cwd");
        return cwd.open(QIODevice::ReadOnly)
            && QFileInfo(QString::fromUtf8(cwd.readAll())).canonicalFilePath() == QFileInfo(folder).canonicalFilePath();
    };
    const QStringList shell = WindowsTerminalBackend::locateShell();
    using Backend = WindowsTerminalBackend;
    // Windows Terminal would expand %OS% in this folder, so Auto quietly uses the console.
    const QString folder = dir.filePath("50%OS% off");
    QVERIFY(QDir().mkpath(folder));
    Backend automatic(Backend::OpenMode::Auto, "C:\\missing\\wt.exe", FakeCli::program(host));
    QString error;
    QVERIFY2(automatic.openShell(folder, &error), qPrintable(error));
    QVERIFY(automatic.takeAdvisory().isEmpty());
    QVERIFY(started(folder));
    QCOMPARE(jsonStrings(host + ".argv"),
             ConsoleHost::arguments(Backend::title({QDir::toNativeSeparators(folder)}), shell.first(), shell.mid(1)));
    // An explicit tab without Windows Terminal says why a console opened.
    QFile::remove(host + ".argv"); QFile::remove(host + ".cwd");
    Backend tab(Backend::OpenMode::Tab, {}, FakeCli::program(host));
    QVERIFY2(tab.openShell(dir.path(), &error), qPrintable(error));
    QVERIFY(tab.takeAdvisory().contains("the shell opened in a console window"));
    QVERIFY(started(dir.path()));
    // A POSIX path is no folder of this computer.
    QVERIFY(!tab.openShell("/work", &error));
    QVERIFY(error.contains("/work"));
    // The detached hosts end right after recording; until then Windows keeps
    // their images and working folders from being removed.
    QTest::qWaitFor([&] { return dir.remove(); }, 20000);
#endif
}

void TestWindowsDesktop::consoleHostRunsTheProgramWithItsExactArguments()
{
#ifndef Q_OS_WIN
    QSKIP("hgs-tray.exe --console-run exists only on Windows");
#else
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString script = dir.filePath("hgs");
    QVERIFY(FakeCli::install(script, "#!/usr/bin/env python3\nimport json,sys\n"
        "open(__file__+'.argv','w',encoding='utf-8').write(json.dumps(sys.argv[1:]))\n"
        "sys.exit(3 if sys.argv[1:2]==['fail'] else 0)\n"));
    const auto host = [&](const QStringList &arguments) {
        auto *process = new QProcess(this);
        process->setProgram(QStringLiteral(ZERUS_TRAY_EXE));
        process->setArguments(ConsoleHost::arguments("Zerus test", FakeCli::program(script), arguments));
        // The console the host allocates stays out of sight.
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *create) {
            create->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            create->startupInfo->wShowWindow = SW_HIDE;
        });
        process->start();
        return process;
    };
    // No shell and no Windows Terminal in between: the program gets this argv.
    const QStringList arguments = awkwardArguments();
    QProcess *success = host(arguments);
    QVERIFY(success->waitForFinished(30000));
    QCOMPARE(success->exitStatus(), QProcess::NormalExit);
    QCOMPARE(success->exitCode(), 0);
    QCOMPARE(jsonStrings(script + ".argv"), arguments);
    // A failure keeps the window open until Enter.
    QFile::remove(script + ".argv");
    QProcess *failure = host({"fail", "x y"});
    QTRY_VERIFY_WITH_TIMEOUT(QFile(script + ".argv").size() > 0, 20000);
    QCOMPARE(jsonStrings(script + ".argv"), (QStringList{"fail", "x y"}));
    QTest::qWait(1500);
    QCOMPARE(failure->state(), QProcess::Running);
    failure->kill();
    QVERIFY(failure->waitForFinished(5000));
#endif
}

void TestWindowsDesktop::templateProgramGetsAConsoleWindow()
{
    // terminal=powershell.exe -NoExit -Command {cmd}: {cmd} is the PowerShell command.
    QString error;
    const QString command = SessionCommands::command("C:/Zerus/hgs.exe", {"@mac", "a", "codex/p"},
                                                     SessionCommands::Shell::PowerShell);
    const QStringList argv = CommandBackend::buildArgv("powershell.exe -NoExit -Command {cmd}", command, &error);
    QCOMPARE(argv, (QStringList{"powershell.exe", "-NoExit", "-Command", "C:\\Zerus\\hgs.exe '@mac' a codex/p"}));
    QProcess process;
    CommandBackend::prepare(process, argv);
    QCOMPARE(process.program(), QString("powershell.exe"));
    QCOMPARE(process.arguments(), argv.mid(1));
    QCOMPARE(process.workingDirectory(), QDir::homePath());
#ifdef Q_OS_UNIX
    // Elsewhere the terminal inherits only stdio from the tray (ChildProcess.h).
    QCOMPARE(process.unixProcessParameters().flags.testFlag(QProcess::UnixProcessFlag::CloseFileDescriptors),
             ChildProcess::inheritsStdioOnly);
#endif
#ifdef Q_OS_WIN
    // What QProcess::startDetached hands the modifier in a GUI process, which has
    // no console: a hidden console and the tray's standard handles.
    const auto created = [](const QProcess &prepared) {
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        QProcess::CreateProcessArguments create{};
        create.flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
        create.startupInfo = &startup;
        if (const auto modify = prepared.createProcessArgumentsModifier()) modify(&create);
        return std::make_pair(create.flags, startup);
    };
    // A visible console of its own, which a GUI program such as wt.exe ignores.
    const auto [flags, startup] = created(process);
    QCOMPARE(flags, DWORD(CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT));
    QCOMPARE(startup.dwFlags, DWORD(0));
    // The console host (a GUI program) opens its console itself, shown normally.
    QProcess host;
    WindowsConsole::show(host, false);
    const auto [hostFlags, hostStartup] = created(host);
    QCOMPARE(hostFlags, DWORD(CREATE_UNICODE_ENVIRONMENT));
    QCOMPARE(hostStartup.dwFlags, DWORD(STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW));
    QCOMPARE(hostStartup.wShowWindow, WORD(SW_SHOWNORMAL));
#endif
}

void TestWindowsDesktop::powerShellTemplateRunsTheExactCommand()
{
#ifndef Q_OS_WIN
    QSKIP("Windows PowerShell exists only on Windows");
#else
    // The documented template without -NoExit, run hidden: PowerShell parses {cmd}
    // into the exact arguments, also through the Start-Process form.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString script = dir.filePath("hgs");
    // The fixture outlives Start-Process -Wait's setup: Windows PowerShell 5.1 fails
    // with "the process has exited" when the program ends before it is waited on.
    QVERIFY(FakeCli::install(script, "#!/usr/bin/env python3\nimport json,sys,time\n"
        "open(__file__+'.argv','w',encoding='utf-8').write(json.dumps(sys.argv[1:]))\ntime.sleep(.5)\n"));
    for (const QStringList &arguments : {QStringList{"@mac", "a", QString::fromUtf8("codex/проект/my name"), "it's"},
                                         awkwardArguments()}) {
        QFile::remove(script + ".argv");
        QString error;
        QStringList argv = CommandBackend::buildArgv(
            "powershell.exe -NoLogo -NoProfile -NonInteractive -Command {cmd}",
            SessionCommands::command(FakeCli::program(script), arguments, SessionCommands::Shell::PowerShell), &error);
        QVERIFY2(!argv.isEmpty(), qPrintable(error));
        QProcess process;
        process.start(argv.takeFirst(), argv);
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(60000));
        QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
        QCOMPARE(jsonStrings(script + ".argv"), arguments);
    }
#endif
}

void TestWindowsDesktop::openModesMapToWindowsTerminal()
{
    using Backend = WindowsTerminalBackend;
    QCOMPARE(Backend::modeFromString("auto"), Backend::OpenMode::Auto);
    QCOMPARE(Backend::modeFromString("tab"), Backend::OpenMode::Tab);
    QCOMPARE(Backend::modeFromString("window"), Backend::OpenMode::Window);
    QCOMPARE(Backend::modeFromString("console"), Backend::OpenMode::Console);
    QCOMPARE(Backend::modeFromString("clipboard"), Backend::OpenMode::Auto);
}

void TestWindowsDesktop::notificationClicksReachOnlyCurrentAttention()
{
    RecordingTrayIcon tray;
    auto notifier = makeAttentionNotifier(tray);
    QSignalSpy activated(notifier.get(), &AttentionNotifier::activated), failed(notifier.get(), &AttentionNotifier::failed);
    notifier->post("attention-1", "Needs input", "codex/p");
    if (!QSystemTrayIcon::supportsMessages()) {
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toString(), QString("attention-1"));
        QVERIFY(tray.titles.isEmpty());
        return;
    }
    QCOMPARE(tray.titles, QStringList{"Needs input"});
    // The toast shows the program's own icon, not the generic Information glyph.
    QCOMPARE(tray.pictures.size(), 1);
    QVERIFY(tray.pictures[0].availableSizes().contains(QSize(256, 256)));
    QVERIFY(!tray.pictures[0].pixmap(QSize(48, 48)).isNull());
    QVERIFY(tray.clicked);
    tray.clicked();
    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated[0][0].toString(), QString("attention-1"));
    // Delivered messages cannot be retracted; a click after withdraw() does nothing.
    notifier->withdraw("attention-1");
    tray.clicked();
    QCOMPARE(activated.size(), 1);
    // A later plain message replaces the clickable one.
    notifier->post("attention-2", "Approval", "claude/p");
    tray.showMessage("Zerus: the terminal did not open", {}, QSystemTrayIcon::Warning, 1000);
    QVERIFY(!tray.clicked);
    // The click of a message whose notifier is gone is ignored.
    notifier->post("attention-3", "Approval", "claude/p");
    const auto click = tray.clicked;
    notifier.reset();
    click();
    QCOMPARE(activated.size(), 1);
}

void TestWindowsDesktop::workerRestartDelayGrowsAndResets()
{
    QCOMPARE(SwarmWorkerSupervisor::nextDelay(1000, 0, 0), 1000);
    QCOMPARE(SwarmWorkerSupervisor::nextDelay(1000, 1000, 500), 2000);
    QCOMPARE(SwarmWorkerSupervisor::nextDelay(1000, 2000, 500), 4000);
    QCOMPARE(SwarmWorkerSupervisor::nextDelay(1000, 4 * 60 * 1000, 500), 5 * 60 * 1000);
    QCOMPARE(SwarmWorkerSupervisor::nextDelay(1000, 5 * 60 * 1000, 500), 5 * 60 * 1000);
    QCOMPARE(SwarmWorkerSupervisor::nextDelay(1000, 64000, 60 * 1000), 1000);
}

void TestWindowsDesktop::workerIsRestartedAfterExitAndStopsWithTheTray()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString script = dir.filePath("hgs");
    QVERIFY(FakeCli::install(script, "#!/usr/bin/env python3\nimport json,sys\n"
        "open(__file__+'.starts','a').write(json.dumps(sys.argv[1:])+'\\n')\n"
        "print('swarm store is locked',file=sys.stderr)\nsys.exit(3)\n"));
    SwarmWorkerSupervisor worker(FakeCli::program(script), 50);
    QSignalSpy exited(&worker, &SwarmWorkerSupervisor::workerExited);
    worker.start();
    QTRY_VERIFY_WITH_TIMEOUT(lines(script + ".starts").size() >= 3, 20000);
    worker.stop();
    const int starts = lines(script + ".starts").size();
    QTest::qWait(600);
    QCOMPARE(lines(script + ".starts").size(), starts);
    QCOMPARE(lines(script + ".starts").first(), QString("[\"swarm\", \"worker\"]"));
    QVERIFY(!exited.isEmpty());
    QCOMPARE(exited[0][0].toInt(), 3);
    QCOMPARE(exited[0][1].toString(), QString("swarm store is locked"));
}

void TestWindowsDesktop::stoppingEndsARunningWorker()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString script = dir.filePath("hgs");
    QVERIFY(FakeCli::install(script, "#!/usr/bin/env python3\nimport time\n"
        "open(__file__+'.running','w').write('yes')\ntime.sleep(60)\n"));
    SwarmWorkerSupervisor worker(FakeCli::program(script), 50);
    QSignalSpy started(&worker, &SwarmWorkerSupervisor::workerStarted);
    worker.start();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(script + ".running"), 20000);
    QVERIFY(worker.isRunning());
    // The start notification can arrive after the child has already run (macOS).
    QTRY_COMPARE_WITH_TIMEOUT(started.size(), 1, 10000);
    const qint64 pid = started[0][0].toLongLong();
    QVERIFY(FakeCli::processAlive(pid));
    worker.stop();
    QVERIFY(!worker.isRunning());
    QVERIFY(!FakeCli::processAlive(pid));
}

void TestWindowsDesktop::restartedWorkersStayInAKillOnCloseJob()
{
#ifndef Q_OS_WIN
    QSKIP("Job objects exist only on Windows");
#else
    // The fixture shim joins a kill-on-close job of its own right away, as hgs.exe
    // does; a worker restarted after that must still end with the tray.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString script = dir.filePath("hgs");
    QVERIFY(FakeCli::install(script, "#!/usr/bin/env python3\nimport sys,time\ntime.sleep(0.5)\nsys.exit(3)\n"));
    SwarmWorkerSupervisor worker(FakeCli::program(script), 50);
    QList<bool> contained;
    connect(&worker, &SwarmWorkerSupervisor::workerStarted, this, [&] { contained << worker.workerContained(); });
    worker.start();
    QTRY_VERIFY_WITH_TIMEOUT(contained.size() >= 4, 30000);
    worker.stop();
    QVERIFY(!worker.workerContained());
    QCOMPARE(contained, QList<bool>(contained.size(), true));
#endif
}

void TestWindowsDesktop::startupNoticeAppearsOnlyForASlowCheck()
{
    // A quick check shows nothing.
    {
        StartupNotice notice(60000);
        notice.step("status");
        QTest::qWait(50);
        QVERIFY(!notice.isVisible());
    }
    StartupNotice notice(50);
    notice.step("status");
    QVERIFY(!notice.isVisible());
    QTRY_VERIFY(notice.isVisible());
    QCOMPARE(notice.text(), QString("Starting Zerus…"));
    // A passive note: no title bar, and the focus stays where it is.
    QCOMPARE(notice.windowType(), Qt::SplashScreen);
    QVERIFY(notice.windowFlags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(notice.testAttribute(Qt::WA_ShowWithoutActivating));
    // A refresh changes the visible note, which stays centred.
    notice.step("install");
    QVERIFY(notice.isVisible());
    QCOMPARE(notice.text(), QString("Starting Zerus…\nUpdating the hgs command line for local sessions"));
    const QPoint offset = notice.geometry().center() - notice.screen()->availableGeometry().center();
    QVERIFY2(offset.manhattanLength() <= 2, qPrintable(QString("%1,%2").arg(offset.x()).arg(offset.y())));
}

QTEST_MAIN(TestWindowsDesktop)
#include "test_windowsdesktop.moc"
