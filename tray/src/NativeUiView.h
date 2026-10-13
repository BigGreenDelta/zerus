#pragma once
#include <QWidget>
#include <functional>
#include <memory>
#include <vector>

class HgsClient;
class NativeBrowser;
class NativeBrowserFactory;
class QLabel;
class QPushButton;
class QStackedWidget;

// The official web workspace. Each host origin has its own in-memory browser
// profile; switching Zerus sessions never navigates or overwrites its composer.
// Browsers come from the optional plugin (NativeBrowserPlugin.h); the factory
// is asked only when a browser is needed.
class NativeUiView : public QWidget {
    Q_OBJECT
public:
    using Factory = std::function<NativeBrowserFactory *()>;
    NativeUiView(HgsClient *client, Factory factory, QWidget *parent = nullptr);
    ~NativeUiView() override;
    void setSession(const QString &host, const QString &name, const QString &run,
                    const QString &machine, bool available);
    void activate();
    void setResponsePending(bool pending);
signals:
    void responseRequested();
protected:
    void showEvent(QShowEvent *event) override;
private:
    void connectUi(bool reload);
    void showProblem(const QString &text);
    struct Browser { QString key; std::unique_ptr<NativeBrowser> browser; };
    std::vector<Browser> m_browsers;
    HgsClient *m_client;
    Factory m_factory;
    QLabel *m_title, *m_status;
    QPushButton *m_reload, *m_response;
    QStackedWidget *m_stack;
    QString m_host, m_name, m_identity, m_activeOrigin;
    quint64 m_request = 0;
    bool m_available = false, m_connected = false, m_forceReload = false;
};
