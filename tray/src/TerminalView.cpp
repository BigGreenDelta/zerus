#include "TerminalView.h"
#include "ContentScale.h"
#include "WorkspaceStyle.h"
#include "PtyProcess.h"
#include "TerminalScreen.h"
#include "WorkspaceIcons.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QVBoxLayout>

TerminalView::TerminalView(QWidget *parent)
    : QWidget(parent), m_process(new PtyProcess(this)),
      m_screen(new TerminalScreen(this)), m_status(new QLabel(this)),
      m_connect(new QPushButton(this)),
      m_reconnect(new QPushButton(this)) {
  setObjectName(QStringLiteral("terminalView"));
  m_status->setObjectName(QStringLiteral("terminalStatus"));
  m_status->setTextFormat(Qt::PlainText);
  m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  m_connect->setObjectName(QStringLiteral("terminalConnect"));
  m_reconnect->setObjectName(QStringLiteral("terminalReconnect"));
  for (auto *button : {m_connect, m_reconnect}) {
    button->setFixedSize(30, 30);
    button->setIconSize(QSize(18, 18));
  }
  m_dark = palette().color(QPalette::Window).lightness() < 128;
  auto *toolbar = new QHBoxLayout;
  toolbar->setContentsMargins(10, 6, 10, 6);
  toolbar->setSpacing(8);
  toolbar->addWidget(m_status, 1);
  toolbar->addWidget(m_reconnect);
  toolbar->addWidget(m_connect);
  auto *layout = new QVBoxLayout(this);
  layout->setSpacing(0);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addLayout(toolbar);
  layout->addWidget(m_screen, 1);
  setFocusProxy(m_screen);
  connect(m_connect, &QPushButton::clicked, this, [this] {
    if (isConnected())
      disconnectSession();
    else
      connectSession();
  });
  connect(m_reconnect, &QPushButton::clicked, this,
          &TerminalView::reconnectSession);
  connect(m_process, &PtyProcess::output, m_screen, &TerminalScreen::feed);
  connect(m_screen, &TerminalScreen::output, m_process, &PtyProcess::write);
  connect(m_screen, &TerminalScreen::sizeChanged, m_process,
          &PtyProcess::resize);
  connect(m_screen, &TerminalScreen::titleChanged, this,
          &TerminalView::titleChanged);
  connect(m_process, &PtyProcess::errorOccurred, this,
          [this](const QString &message) {
            showStatus(message);
            emit errorOccurred(message);
          });
  connect(m_process, &PtyProcess::finished, this, [this](int code) {
    showStatus(code == 0 ? disconnectedStatus()
                         : tr("Attach client exited (%1) — session output is "
                              "preserved above")
                               .arg(code));
    updateButtons();
    emit connectedChanged(false);
  });
  showStatus(tr("Connect to use the session's live terminal"));
  updateButtons();
}
TerminalView::~TerminalView() { m_process->stop(); }
bool TerminalView::isConnected() const { return m_process->isRunning(); }
void TerminalView::setSession(const QString &path, const QString &host,
                              const QString &name, const QString &runId,
                              const QString &machine) {
  const bool sameEndpoint = m_hgsPath == path && m_host == host;
  const bool sameRun = !m_runId.isEmpty() && m_runId == runId;
  const bool changed =
      !sameEndpoint || (m_name != name && !sameRun) ||
      (!m_runId.isEmpty() && !runId.isEmpty() && m_runId != runId);
  if (changed) {
    ++m_connectionGeneration;
    disconnectSession();
    m_screen->reset();
  }
  m_hgsPath = path;
  m_host = host;
  m_name = name;
  // After disconnecting: that status names the machine of the previous session.
  m_machine = machine;
  // The first inspection may enrich an untracked target with its run token.
  // A transient omission must not erase an already established guard.
  if (changed || !runId.isEmpty())
    m_runId = runId;
  updateButtons();
}
void TerminalView::setAvailable(bool available, const QString &reason) {
  m_available = available;
  m_unavailableReason = reason;
  if (!available) {
    disconnectSession();
    showStatus(reason.isEmpty() ? tr("The session has no live terminal")
                                : reason);
  }
  updateButtons();
}
void TerminalView::updateSessionName(const QString &name) {
  m_name = name;
  updateButtons();
}
void TerminalView::connectSession() {
  if (isConnected() || !m_available || m_name.isEmpty() || m_hgsPath.isEmpty())
    return;
  ++m_connectionGeneration;
  QStringList arguments;
  if (!m_host.isEmpty())
    arguments << QStringLiteral("@") + m_host;
  arguments << QStringLiteral("a") << m_name << QStringLiteral("--existing");
  if (!m_runId.isEmpty())
    arguments << QStringLiteral("--run-id") << m_runId;
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.remove(QStringLiteral("TMUX"));
  environment.remove(QStringLiteral("TMUX_PANE"));
  environment.insert(QStringLiteral("HGS_TAB"), QStringLiteral("0"));
  environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
  environment.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
  m_screen->reset();
  showStatus(tr("Connecting to %1…").arg(m_name));
  if (m_process->start(m_hgsPath, arguments, environment,
                       m_screen->terminalSize())) {
    showStatus(tr("Connected: %1").arg(m_name));
    m_screen->setFocus(Qt::OtherFocusReason);
    emit connectedChanged(true);
  }
  updateButtons();
}
void TerminalView::disconnectSession() {
  if (!isConnected())
    return;
  ++m_connectionGeneration;
  m_process->stop();
  showStatus(disconnectedStatus());
  updateButtons();
  emit connectedChanged(false);
}
void TerminalView::reconnectSession() {
  disconnectSession();
  connectSession();
}
void TerminalView::pasteText(const QString &text) {
  if (isConnected())
    m_screen->pasteText(text);
}
QString TerminalView::disconnectedStatus() const {
  // tmux keeps Unix sessions, the session host Windows ones.
  return m_machine.isEmpty()
             ? tr("Disconnected — the session keeps running")
             : tr("Disconnected — the session continues on %1").arg(m_machine);
}
void TerminalView::showStatus(const QString &status) {
  m_status->setText(status);
  m_status->setToolTip(status);
  emit statusChanged(status);
}
void TerminalView::setTheme(bool dark) {
  m_dark = dark;
  // Local rules also apply when the terminal is used outside SessionsWindow.
  setStyleSheet(QStringLiteral(
      "QPushButton#terminalConnect, QPushButton#terminalReconnect {"
      " padding:0; min-height:0; border:1px solid transparent; border-radius:6px; background:transparent; }"
      "QPushButton#terminalConnect:hover, QPushButton#terminalReconnect:hover { background:%1; }"
      "QPushButton#terminalConnect:focus[keyboardFocus=\"true\"], QPushButton#terminalReconnect:focus[keyboardFocus=\"true\"] { border-color:%2; }")
      .arg(dark ? "#2b3540" : "#edf2f5", dark ? "#8bdfc0" : "#167357") + workspaceScrollbars(dark)
      // The workspace sets a 13px font on every widget. Cell metrics and the
      // PTY size follow the resulting FontChange.
      + (qFuzzyCompare(m_scale, 1.0) ? QString()
         : QStringLiteral(" QAbstractScrollArea#terminalScreen { font-size:%1px; }").arg(ContentScale::px(13, m_scale))));
  updateButtons();
}
void TerminalView::setContentScale(double scale) {
  scale = ContentScale::clamp(scale);
  if (qFuzzyCompare(scale, m_scale))
    return;
  m_scale = scale;
  setTheme(m_dark);
}
void TerminalView::updateButtons() {
  const bool ready = m_available && !m_hgsPath.isEmpty() && !m_name.isEmpty();
  const bool connected = isConnected();
  const QColor color(m_dark ? "#a2adbc" : "#627082");
  m_connect->setIcon(workspaceIcon(connected ? "disconnect" : "connect", color));
  m_reconnect->setIcon(workspaceIcon("refresh", color));
  m_connect->setAccessibleName(connected ? tr("Disconnect terminal") : tr("Connect terminal"));
  m_reconnect->setAccessibleName(tr("Reconnect terminal"));
  m_connect->setEnabled(isConnected() || ready);
  m_reconnect->setEnabled(ready);
  m_connect->setToolTip(connected ? tr("Disconnect terminal. The session keeps running")
                        : m_available ? tr("Connect terminal") : m_unavailableReason);
  m_reconnect->setToolTip(m_available ? tr("Reconnect terminal. The session keeps running") : m_unavailableReason);
  // Retain selection/copy of the last screen after disconnect. The PTY
  // silently discards input when there is no attached client.
}
void TerminalView::closeEvent(QCloseEvent *event) {
  disconnectSession();
  QWidget::closeEvent(event);
}
