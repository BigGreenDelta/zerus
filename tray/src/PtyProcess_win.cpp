#include "PtyProcess.h"
#include "WindowsCommandLine.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QWinEventNotifier>
#include <qt_windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The pseudoconsole API is looked up at run time: a static import would keep
// the whole tray from starting on Windows builds older than 10 1809, where only
// the embedded terminal is unavailable.
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

namespace {
constexpr qsizetype MaxQueuedInput = 4 * 1024 * 1024;
// How long Disconnect lets the client exit after the console closes.
constexpr DWORD GracefulStopMs = 1000;
using PseudoConsole = void *;
using CreatePseudoConsoleFn = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, PseudoConsole *);
using ResizePseudoConsoleFn = HRESULT(WINAPI *)(PseudoConsole, COORD);
using ClosePseudoConsoleFn = void(WINAPI *)(PseudoConsole);

struct PseudoConsoleApi {
  CreatePseudoConsoleFn create = nullptr;
  ResizePseudoConsoleFn resize = nullptr;
  ClosePseudoConsoleFn close = nullptr;
  bool available() const { return create && resize && close; }
};

const PseudoConsoleApi &pseudoConsoleApi() {
  static const PseudoConsoleApi api = [] {
    PseudoConsoleApi result;
    if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
      result.create = reinterpret_cast<CreatePseudoConsoleFn>(
          reinterpret_cast<void *>(GetProcAddress(kernel, "CreatePseudoConsole")));
      result.resize = reinterpret_cast<ResizePseudoConsoleFn>(
          reinterpret_cast<void *>(GetProcAddress(kernel, "ResizePseudoConsole")));
      result.close = reinterpret_cast<ClosePseudoConsoleFn>(
          reinterpret_cast<void *>(GetProcAddress(kernel, "ClosePseudoConsole")));
    }
    return result;
  }();
  return api;
}

COORD consoleSize(QSize cells) {
  return COORD{static_cast<SHORT>(std::clamp(cells.width(), 2, 1000)),
               static_cast<SHORT>(std::clamp(cells.height(), 2, 500))};
}

QString systemError(DWORD error) {
  wchar_t *text = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
      reinterpret_cast<wchar_t *>(&text), 0, nullptr);
  QString message = length ? QString::fromWCharArray(text, int(length)).trimmed()
                           : QStringLiteral("error %1").arg(error);
  if (text)
    LocalFree(text);
  return message;
}

// CreateProcess wants the block sorted by name, case-insensitively.
std::wstring environmentBlock(const QProcessEnvironment &environment) {
  QStringList entries = environment.toStringList();
  std::sort(entries.begin(), entries.end(), [](const QString &a, const QString &b) {
    return a.compare(b, Qt::CaseInsensitive) < 0;
  });
  std::wstring block;
  for (const QString &entry : std::as_const(entries)) {
    block += entry.toStdWString();
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}
} // namespace

// Shared by the tray and its reader/writer threads; the last owner closes the
// handles. owner is cleared under the mutex before PtyProcess goes away, so a
// thread never posts to a destroyed object.
struct PtyProcess::Console {
  PseudoConsole console = nullptr;
  HANDLE process = nullptr;
  HANDLE job = nullptr;
  HANDLE input = nullptr;
  HANDLE output = nullptr;
  std::mutex mutex;
  std::condition_variable wake;
  PtyProcess *owner = nullptr;
  std::deque<QByteArray> queue;
  std::atomic<qsizetype> queued{0};
  bool stopping = false;
  bool closing = false;

  ~Console() {
    if (console && !closing)
      pseudoConsoleApi().close(console);
    for (HANDLE handle : {process, job, input, output})
      if (handle)
        CloseHandle(handle);
  }

  // ClosePseudoConsole can wait until the output is drained (before Windows 11
  // 24H2), so it never runs on the GUI thread; the reader keeps draining.
  static void close(const std::shared_ptr<Console> &console) {
    if (std::exchange(console->closing, true))
      return;
    std::thread([console] { pseudoConsoleApi().close(console->console); }).detach();
  }

  void detach() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      owner = nullptr;
      stopping = true;
      queue.clear();
      queued = 0;
    }
    wake.notify_all();
  }
};

PtyProcess::PtyProcess(QObject *parent) : QObject(parent) {
  m_drainLimit.setSingleShot(true);
  connect(&m_drainLimit, &QTimer::timeout, this, [this] {
    if (m_exited)
      finish();
  });
}
PtyProcess::~PtyProcess() { stop(); }

bool PtyProcess::start(const QString &program, const QStringList &arguments,
                       const QProcessEnvironment &environment, QSize cells) {
  stop();
  const auto &api = pseudoConsoleApi();
  if (!api.available()) {
    emit errorOccurred(tr("The embedded terminal needs Windows 10 version 1809 or later."));
    return false;
  }
  const QString executable = QFileInfo(program).isAbsolute()
                                 ? program
                                 : QStandardPaths::findExecutable(program);
  if (executable.isEmpty() || !QFileInfo(executable).isExecutable() ||
      !QFileInfo(executable).isFile()) {
    emit errorOccurred(tr("Attach executable is unavailable: %1").arg(program));
    return false;
  }
  const auto fail = [this](const char *call, DWORD error) {
    emit errorOccurred(tr("Cannot open terminal: %1: %2")
                           .arg(QLatin1String(call), systemError(error)));
    return false;
  };

  auto console = std::make_shared<Console>();
  HANDLE inputRead = nullptr, outputWrite = nullptr;
  if (!CreatePipe(&inputRead, &console->input, nullptr, 0))
    return fail("CreatePipe", GetLastError());
  if (!CreatePipe(&console->output, &outputWrite, nullptr, 0)) {
    const DWORD error = GetLastError();
    CloseHandle(inputRead);
    return fail("CreatePipe", error);
  }
  const HRESULT created = api.create(consoleSize(cells), inputRead, outputWrite, 0,
                                     &console->console);
  // The pseudoconsole keeps its own duplicates of its ends.
  CloseHandle(inputRead);
  CloseHandle(outputWrite);
  if (FAILED(created)) {
    console->console = nullptr;
    return fail("CreatePseudoConsole", DWORD(created));
  }

  SIZE_T attributeSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
  std::vector<char> attributeStorage(attributeSize);
  auto *attributes =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize))
    return fail("InitializeProcThreadAttributeList", GetLastError());
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                 console->console, sizeof(console->console),
                                 nullptr, nullptr)) {
    const DWORD error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    return fail("UpdateProcThreadAttribute", error);
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  // Null standard handles: the child must use the pseudoconsole, never the
  // tray's own (possibly redirected) stdio.
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.lpAttributeList = attributes;

  const std::wstring application =
      QDir::toNativeSeparators(executable).toStdWString();
  std::wstring commandLine =
      WindowsCommandLine::build(executable, arguments).toStdWString();
  std::wstring block;
  if (!environment.isEmpty())
    block = environmentBlock(environment);
  PROCESS_INFORMATION info{};
  const BOOL started = CreateProcessW(
      application.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
      block.empty() ? nullptr : block.data(), nullptr, &startup.StartupInfo, &info);
  const DWORD startError = GetLastError();
  DeleteProcThreadAttributeList(attributes);
  if (!started)
    return fail("CreateProcess", startError);
  console->process = info.hProcess;

  // Kill-on-close: stopping the terminal, or a tray crash, ends hgs.exe and the
  // ssh.exe it started. The session itself lives on the remote machine. The job
  // forbids breakaway, so what runs here must never create a session: a host it
  // started would stop in this job and be refused (D4). The terminal only runs
  // `hgs a --existing`, which attaches to a live run.
  console->job = CreateJobObjectW(nullptr, nullptr);
  if (console->job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(console->job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(console->job, info.hProcess)) {
      CloseHandle(console->job);
      console->job = nullptr;
    }
  }
  ResumeThread(info.hThread);
  CloseHandle(info.hThread);

  console->owner = this;
  m_console = console;
  m_pid = qint64(info.dwProcessId);
  m_exited = m_drained = false;
  m_exitCode = -1;

  std::thread([console] {
    std::vector<char> buffer(16384);
    for (;;) {
      DWORD count = 0;
      if (!ReadFile(console->output, buffer.data(), DWORD(buffer.size()), &count,
                    nullptr) ||
          count == 0)
        break;
      QByteArray bytes(buffer.data(), qsizetype(count));
      std::lock_guard<std::mutex> lock(console->mutex);
      if (PtyProcess *owner = console->owner)
        QMetaObject::invokeMethod(
            owner, [owner, console, bytes] { owner->consoleOutput(console, bytes); },
            Qt::QueuedConnection);
    }
    std::lock_guard<std::mutex> lock(console->mutex);
    if (PtyProcess *owner = console->owner)
      QMetaObject::invokeMethod(
          owner, [owner, console] { owner->consoleClosed(console); },
          Qt::QueuedConnection);
  }).detach();

  // A full input pipe must never block the GUI thread.
  std::thread([console] {
    for (;;) {
      QByteArray chunk;
      {
        std::unique_lock<std::mutex> lock(console->mutex);
        console->wake.wait(lock, [&] { return console->stopping || !console->queue.empty(); });
        if (console->stopping)
          return;
        chunk = std::move(console->queue.front());
        console->queue.pop_front();
      }
      const char *data = chunk.constData();
      qsizetype left = chunk.size();
      while (left > 0) {
        DWORD written = 0;
        if (!WriteFile(console->input, data, DWORD(std::min<qsizetype>(left, 65536)),
                       &written, nullptr)) {
          std::lock_guard<std::mutex> lock(console->mutex);
          console->queue.clear();
          console->queued = 0;
          return;
        }
        data += written;
        left -= written;
        console->queued -= qsizetype(written);
      }
    }
  }).detach();

  m_exit = new QWinEventNotifier(info.hProcess, this);
  // The handle must outlive the notifier, whichever is destroyed first.
  connect(m_exit, &QObject::destroyed, [console] {});
  connect(m_exit, &QWinEventNotifier::activated, this, [this] { processExited(); });
  return true;
}

void PtyProcess::stop() {
  m_drainLimit.stop();
  m_pid = -1;
  m_exited = m_drained = false;
  if (m_exit) {
    m_exit->setEnabled(false);
    m_exit->deleteLater();
    m_exit = nullptr;
  }
  auto console = std::exchange(m_console, {});
  if (!console)
    return;
  console->detach();
  // Hang up first, as on Unix: closing the pseudoconsole sends CTRL_CLOSE_EVENT
  // to the attached client (hgs.exe and ssh.exe). Current ConPTY ends clients
  // almost at once, so hgs.exe cannot rely on it to clean up and removes link
  // markers of killed clients on its next start. The job then ends whatever is
  // left.
  Console::close(console);
  WaitForSingleObject(console->process, GracefulStopMs);
  if (console->job)
    TerminateJobObject(console->job, 1);
  else
    TerminateProcess(console->process, 1);
  // Bounded: the job is gone right after TerminateJobObject, and callers such
  // as tests delete the executable next.
  WaitForSingleObject(console->process, 2000);
}

void PtyProcess::consoleOutput(const std::shared_ptr<Console> &console,
                               const QByteArray &bytes) {
  if (console == m_console)
    emit output(bytes);
}

void PtyProcess::consoleClosed(const std::shared_ptr<Console> &console) {
  if (console != m_console)
    return;
  m_drained = true;
  if (m_exited)
    finish();
}

void PtyProcess::processExited() {
  if (!m_console || m_exited)
    return;
  m_exited = true;
  m_exit->setEnabled(false);
  DWORD code = 0;
  m_exitCode = GetExitCodeProcess(m_console->process, &code) ? int(code) : -1;
  // Closing the pseudoconsole flushes its last frame and ends the output pipe;
  // finished() then follows the final output, as after waitpid on Unix.
  Console::close(m_console);
  if (m_drained)
    finish();
  else
    m_drainLimit.start(3000);
}

void PtyProcess::finish() {
  m_drainLimit.stop();
  auto console = std::exchange(m_console, {});
  m_pid = -1;
  if (m_exit) {
    m_exit->deleteLater();
    m_exit = nullptr;
  }
  if (console)
    console->detach();
  emit finished(m_exitCode);
}

void PtyProcess::write(const QByteArray &bytes) {
  if (!m_console || m_exited || bytes.isEmpty())
    return;
  if (bytes.size() > MaxQueuedInput - m_console->queued.load()) {
    emit errorOccurred(
        tr("Terminal input queue is full; wait before pasting more text."));
    return;
  }
  {
    std::lock_guard<std::mutex> lock(m_console->mutex);
    if (m_console->stopping)
      return;
    m_console->queue.push_back(bytes);
    m_console->queued += bytes.size();
  }
  m_console->wake.notify_one();
}

void PtyProcess::resize(QSize cells) {
  if (!m_console || m_console->closing)
    return;
  pseudoConsoleApi().resize(m_console->console, consoleSize(cells));
}
