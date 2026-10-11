#pragma once

#include <QProcess>

#ifdef Q_OS_WIN
#include <qt_windows.h>

// QProcess gives the console programs a GUI process starts (hgs-tray.exe has no
// console) a hidden console (CREATE_NO_WINDOW); a terminal must be a window. A
// console program gets a new console and uses it even when the tray has standard
// handles (started with redirected output); Windows ignores CREATE_NEW_CONSOLE
// for a GUI program such as wt.exe. A GUI program that opens a console itself
// (the console host) is shown as a normal window whatever state the tray was
// started in.
namespace WindowsConsole {

inline void show(QProcess::CreateProcessArguments *create, bool consoleProgram)
{
    create->flags &= ~DWORD(CREATE_NO_WINDOW);
    if (consoleProgram) {
        create->flags |= CREATE_NEW_CONSOLE;
        create->startupInfo->dwFlags &= ~DWORD(STARTF_USESTDHANDLES);
    } else {
        create->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        create->startupInfo->wShowWindow = SW_SHOWNORMAL;
    }
}

inline void show(QProcess &process, bool consoleProgram)
{
    process.setCreateProcessArgumentsModifier([consoleProgram](QProcess::CreateProcessArguments *create) {
        show(create, consoleProgram);
    });
}

} // namespace WindowsConsole
#endif
