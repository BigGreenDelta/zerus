// Windows part of FakeCli.h, compiled into every Windows test program.
#include "FakeCli.h"

#include <qt_windows.h>

bool FakeCli::processAlive(qint64 pid)
{
    if (pid <= 0) return false;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!process) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

void FakeCli::terminate(qint64 pid)
{
    if (pid <= 0) return;
    if (const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid))) {
        TerminateProcess(process, 1);
        CloseHandle(process);
    }
}
