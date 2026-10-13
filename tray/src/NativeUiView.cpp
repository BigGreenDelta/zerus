#include "NativeUiView.h"
#include "HgsClient.h"
#include "NativeBrowser.h"
#include "WorkspaceIcons.h"
#include <QCryptographicHash>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QString originOf(const QUrl &url) { return url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment).toString(); }
}

NativeUiView::NativeUiView(HgsClient *client, Factory factory, QWidget *parent)
    : QWidget(parent), m_client(client), m_factory(std::move(factory))
{
    setObjectName("nativeUiView");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 8, 0, 0); layout->setSpacing(8);
    auto *tools = new QHBoxLayout;
    m_title = new QLabel(tr("DeepSeek native workspace")); m_title->setTextFormat(Qt::PlainText);
    tools->addWidget(m_title, 1);
    m_response = new QPushButton(tr("Respond in Activity")); m_response->setObjectName("nativeUiResponse");
    m_response->setToolTip(tr("This session has a question or approval waiting in Activity."));
    m_response->hide(); tools->addWidget(m_response);
    connect(m_response, &QPushButton::clicked, this, &NativeUiView::responseRequested);
    m_reload = new QPushButton; m_reload->setObjectName("reloadNativeUi");
    m_reload->setIcon(workspaceIcon("refresh", palette().color(QPalette::Text)));
    m_reload->setToolTip(tr("Reload native UI")); m_reload->setAccessibleName(tr("Reload native UI"));
    m_reload->setFixedSize(32, 32); tools->addWidget(m_reload); layout->addLayout(tools);
    m_status = new QLabel; m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true);
    m_status->setObjectName("nativeUiStatus"); layout->addWidget(m_status);
    m_stack = new QStackedWidget; layout->addWidget(m_stack, 1);
    connect(m_reload, &QPushButton::clicked, this, [this] { connectUi(true); });
    connect(client, &HgsClient::nativeUiFailed, this, [this](quint64 request, const QString &error) {
        if (request != m_request) return;
        m_request = 0; m_reload->setEnabled(m_available); showProblem(tr("Could not open native DeepSeek UI. %1").arg(error));
    });
    connect(client, &HgsClient::nativeUiReady, this, [this](quint64 request, const QJsonObject &connection) {
        if (request != m_request || !m_available) return;
        m_request = 0; m_reload->setEnabled(true);
        const QString address = connection.value("url").toString();
        const QUrl url(address);
        static const QRegularExpression launch("^http://127\\.0\\.0\\.1:[0-9]+/\\?token=[A-Za-z0-9_%.-]+$");
        if (!launch.match(address).hasMatch() || url.port() <= 0 || url.port() > 65535) {
            showProblem(tr("The native UI returned an invalid connection address.")); return;
        }
        const QString origin = originOf(url);
        // Reused loopback ports must never revive another host generation's
        // page or cookies. Keep only a fingerprint of its launch credential.
        const QString key = m_host + '\n' + origin + '\n' + QString::fromLatin1(QCryptographicHash::hash(address.toUtf8(), QCryptographicHash::Sha256).toHex());
        NativeBrowser *browser = nullptr;
        for (const auto &entry : m_browsers) if (entry.key == key) browser = entry.browser.get();
        const bool fresh = !browser;
        if (!browser) {
            auto *factory = m_factory ? m_factory() : nullptr;
            if (!factory) { showProblem(tr("The embedded native UI is not installed. Open it in your browser.")); return; }
            browser = factory->create(origin, palette().color(QPalette::Window), nullptr);
            m_stack->addWidget(browser->widget()); m_browsers.push_back({key, std::unique_ptr<NativeBrowser>(browser)});
            QWidget *view = browser->widget();
            browser->loadFinished = [this, view, key](bool ok) {
                if (m_stack->currentWidget() != view || m_activeOrigin != key || !m_available) return;
                if (ok) { m_status->hide(); m_stack->show(); m_connected = true; }
                else showProblem(tr("Native UI could not load. Use Reload to reconnect, or open it in your browser."));
            };
            browser->rendererStopped = [this, view, key] {
                if (m_stack->currentWidget() == view && m_activeOrigin == key) showProblem(tr("The native UI renderer stopped. Use Reload to reopen it."));
            };
        }
        QWidget *view = browser->widget();
        m_activeOrigin = key; m_stack->setCurrentWidget(view); m_stack->show(); m_connected = true;
        if (fresh || m_forceReload) {
            m_status->setText(tr("Loading native DeepSeek UI…")); m_status->show(); browser->load(url);
        } else m_status->hide();
    });
}

NativeUiView::~NativeUiView()
{
    // Each browser deletes its view before its profile, ahead of the stack's
    // own child cleanup.
    m_browsers.clear();
}

void NativeUiView::setSession(const QString &host, const QString &name, const QString &run, const QString &machine, bool available)
{
    const QString identity = host + '\n' + name + '\n' + run;
    if (identity != m_identity || available != m_available) {
        m_request = 0; m_connected = false; m_identity = identity; m_activeOrigin.clear(); m_stack->hide();
        m_status->setText(available ? tr("Open the native workspace to manage DeepSeek sessions and settings.") : tr("Native UI is unavailable while this machine is offline.")); m_status->show();
        if (available && isVisible()) QTimer::singleShot(0, this, &NativeUiView::activate);
    }
    m_host = host; m_name = name; m_available = available && !name.isEmpty();
    m_title->setText(tr("DeepSeek workspace / %1").arg(machine)); m_reload->setEnabled(m_available && !m_request);
}

void NativeUiView::activate() { if (!m_connected && !m_request) connectUi(false); }
void NativeUiView::setResponsePending(bool pending) { m_response->setVisible(pending); }
void NativeUiView::showEvent(QShowEvent *event) { QWidget::showEvent(event); activate(); }
void NativeUiView::connectUi(bool reload)
{
    if (!m_available || m_request) return;
    m_forceReload = reload; m_connected = false; m_reload->setEnabled(false);
    m_status->setText(tr("Connecting to native DeepSeek UI…")); m_status->show();
    m_request = m_client->requestNativeUi(m_host, m_name);
}
void NativeUiView::showProblem(const QString &text)
{
    m_connected = false; m_status->setText(text); m_status->show(); m_stack->hide();
}
