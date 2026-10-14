#pragma once

#include "TerminalBackend.h"

#include <QString>
#include <QStringList>

// Windows: a Windows Terminal (wt.exe) tab or window starts hgs.exe directly, and
// a console window is the fallback. That window is hgs-tray.exe itself acting as
// a console host (ConsoleHost.h): it starts hgs.exe with the exact argument
// vector, without a shell, and stays open when hgs reports an error; Windows
// Terminal keeps a failed tab open by itself (closeOnExit "automatic"). A local
// folder shell is PowerShell started in that folder, by either of them.
//
// The command builders are pure and compiled on every platform for the tests;
// only open(), openProgram() and openShell() start processes, and only on Windows.
class WindowsTerminalBackend : public TerminalBackend {
public:
    // auto: a tab, or the console without a word when wt.exe is missing or cannot
    // pass the arguments unchanged. tab/window: the same, but the fallback is
    // reported. console: always a console window.
    enum class OpenMode { Auto, Tab, Window, Console };

    // consoleHost: the hgs-tray.exe that runs console windows (--console-run),
    // by default this program.
    explicit WindowsTerminalBackend(OpenMode mode, QString windowsTerminal = locateWindowsTerminal(),
                                    QString consoleHost = {});

    bool open(const QString &command, QString *error) override;
    bool openProgram(const QString &program, const QStringList &arguments, QString *error) override;
    // PowerShell in a folder of this computer: a Windows Terminal tab that starts
    // there, or a console started there (the same fallback as openProgram).
    bool openShell(const QString &directory, QString *error) override;
    QString takeAdvisory() override;

    static OpenMode modeFromString(const QString &mode);
    // wt.exe arguments that start program with exactly these arguments, or empty
    // when Windows Terminal would change them: it re-joins its command line,
    // quoting only arguments with spaces, so quotes, tabs, empty arguments and
    // backslashes before a closing quote do not survive, and it expands %NAME%
    // environment references. ';' separates wt subcommands and is escaped.
    static QStringList terminalArguments(OpenMode mode, const QString &title, const QString &program,
                                         const QStringList &arguments);
    // wt.exe arguments that open shell (the program, then its arguments) in a
    // tab or window starting in directory, or empty when Windows Terminal would
    // change the folder or the shell's arguments: it also expands %NAME% in the
    // folder.
    static QStringList shellArguments(OpenMode mode, const QString &directory, const QStringList &shell);
    static QString title(const QStringList &arguments);
    static QString locateWindowsTerminal();
    // The PowerShell of a folder shell, program first: PowerShell 7 (pwsh) when
    // installed, otherwise Windows PowerShell without its banner.
    static QStringList shell(const QString &pwsh);
    static QStringList locateShell();

private:
    // Starts Windows Terminal unless the mode is console; otherwise, or when it
    // cannot, false, with the reason as the advisory of an explicit tab or window
    // ("... so <what> opened in a console window").
    bool startTerminal(const QStringList &arguments, const QString &refused, const QString &what);
    bool startShell(const QString &command, QString *error);
    bool startConsoleHost(const QString &title, const QString &program, const QStringList &arguments,
                          QString *error, const QString &directory = QString());
    OpenMode m_mode;
    QString m_terminal;
    QString m_consoleHost;
    QString m_advisory;
};
