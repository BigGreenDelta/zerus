#pragma once

#include <QObject>
#include <QProcessEnvironment>
#include <QSize>
#include <QTimer>

#include <memory>

class QSocketNotifier;
class QWinEventNotifier;

// Owns only the attach client and its private controlling terminal. The tmux
// server, pane supervisor and agent remain outside this process group.
// On Windows the terminal is a pseudoconsole (ConPTY, PtyProcess_win.cpp) and a
// kill-on-close Job Object replaces the process group: stopping closes the
// pseudoconsole (CTRL_CLOSE_EVENT, the hang-up), then ends hgs.exe and its
// ssh.exe, never anything on the remote machine.
class PtyProcess final : public QObject {
  Q_OBJECT
public:
  explicit PtyProcess(QObject *parent = nullptr);
  ~PtyProcess() override;
  bool start(const QString &program, const QStringList &arguments,
             const QProcessEnvironment &environment, QSize cells);
  void stop();
  void write(const QByteArray &bytes);
  void resize(QSize cells);
  bool isRunning() const { return m_pid > 0; }
  qint64 processId() const { return m_pid; }

signals:
  void output(const QByteArray &bytes);
  void finished(int exitCode);
  void errorOccurred(const QString &message);

private:
#ifdef Q_OS_WIN
  struct Console;
  void consoleOutput(const std::shared_ptr<Console> &console, const QByteArray &bytes);
  void consoleClosed(const std::shared_ptr<Console> &console);
  void processExited();
  void finish();
  std::shared_ptr<Console> m_console;
  QWinEventNotifier *m_exit = nullptr;
  QTimer m_drainLimit;
  bool m_exited = false, m_drained = false;
  int m_exitCode = -1;
  qint64 m_pid = -1;
#else
  void readReady();
  void writeReady();
  void reap();
  void closeMaster();
  int m_master = -1;
  int m_pid = -1;
  QByteArray m_pending;
  QSocketNotifier *m_reader = nullptr;
  QSocketNotifier *m_writer = nullptr;
  QTimer m_reaper;
#endif
};
