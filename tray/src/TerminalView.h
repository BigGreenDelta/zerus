#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class PtyProcess;
class TerminalScreen;

class TerminalView final : public QWidget {
  Q_OBJECT
public:
  explicit TerminalView(QWidget *parent = nullptr);
  ~TerminalView() override;
  // Selection/refresh only updates the target. Connecting is an explicit UI
  // action. machine names the session's machine in the status text.
  void setSession(const QString &hgsPath, const QString &host,
                  const QString &name, const QString &expectedRunId = {},
                  const QString &machine = {});
  void setAvailable(bool available, const QString &reason = {});
  void updateSessionName(const QString &newName);
  void setTheme(bool dark);
  // Enlarges the terminal cells; the toolbar keeps the workspace size.
  void setContentScale(double scale);
  bool isConnected() const;
  // Changes even when reconnecting the same session, so delayed uploads cannot
  // paste into a replacement attachment or a different native input.
  quint64 connectionGeneration() const { return m_connectionGeneration; }
  TerminalScreen *terminalScreen() const { return m_screen; }

public slots:
  void connectSession();
  void disconnectSession();
  void reconnectSession();
  void pasteText(const QString &text);

signals:
  void connectedChanged(bool connected);
  void statusChanged(const QString &status);
  void titleChanged(const QString &title);
  void errorOccurred(const QString &message);

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  void showStatus(const QString &status);
  QString disconnectedStatus() const;
  void updateButtons();
  PtyProcess *m_process;
  TerminalScreen *m_screen;
  QLabel *m_status;
  QPushButton *m_connect;
  QPushButton *m_reconnect;
  QString m_hgsPath, m_host, m_name, m_runId, m_machine, m_unavailableReason;
  bool m_available = true;
  bool m_dark = false;
  double m_scale = 1.0;
  quint64 m_connectionGeneration = 0;
};
