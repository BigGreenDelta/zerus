// The embedded DeepSeek native UI: the only code that links Qt WebEngine. The
// GUI loads it on demand (NativeBrowserPlugin.cpp); without it the native UI
// opens in the system browser.
#include "NativeBrowser.h"
#include <QColor>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QObject>
#include <QPointer>
#include <QStandardPaths>
#include <QUrl>
#include <QWebEngineDownloadRequest>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>

namespace {
QString originOf(const QUrl &url) { return url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment).toString(); }
bool webLink(const QUrl &url) { return url.scheme() == "https" || url.scheme() == "http"; }

class NativePage final : public QWebEnginePage {
public:
    NativePage(QWebEngineProfile *profile, const QString &origin, QObject *parent)
        : QWebEnginePage(profile, parent), m_origin(origin) {
        connect(this, &QWebEnginePage::newWindowRequested, this, [this](QWebEngineNewWindowRequest &request) {
            const auto url = request.requestedUrl();
            if (originOf(url) == m_origin) request.openIn(this);
            else if (request.isUserInitiated() && webLink(url)) QDesktopServices::openUrl(url);
        });
    }
protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool mainFrame) override {
        if (!mainFrame || url == QUrl("about:blank") || originOf(url) == m_origin) return true;
        if (type == NavigationTypeLinkClicked && webLink(url)) QDesktopServices::openUrl(url);
        return false;
    }
    // Native JS diagnostics can contain an authenticated address. Keep them
    // out of Zerus logs; visible load failures never include the launch URL.
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &, int, const QString &) override {}
private:
    QString m_origin;
};

class WebBrowser final : public NativeBrowser {
public:
    WebBrowser(const QString &origin, const QColor &background, QWidget *parent)
        : m_profile(new QWebEngineProfile), m_view(new QWebEngineView(parent)) { // Off the record, including cookies and cache.
        m_view->setObjectName("nativeWebPage");
        auto *page = new NativePage(m_profile, origin, m_view); m_view->setPage(page);
        page->setBackgroundColor(background);
        page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
        QObject::connect(m_view, &QWebEngineView::loadFinished, m_view, [this](bool ok) { if (loadFinished) loadFinished(ok); });
        QObject::connect(m_view, &QWebEngineView::renderProcessTerminated, m_view, [this] { if (rendererStopped) rendererStopped(); });
        QObject::connect(m_profile, &QWebEngineProfile::downloadRequested, m_view, [view = m_view.data()](QWebEngineDownloadRequest *download) {
            const auto path = QFileDialog::getSaveFileName(view, QObject::tr("Save file"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + '/' + QFileInfo(download->suggestedFileName()).fileName());
            if (path.isEmpty()) { download->cancel(); return; }
            download->setDownloadDirectory(QFileInfo(path).absolutePath()); download->setDownloadFileName(QFileInfo(path).fileName()); download->accept();
        });
    }
    // A browser profile must outlive every page using it. The view may already
    // be gone with its parent.
    ~WebBrowser() override { delete m_view.data(); delete m_profile; }
    QWidget *widget() override { return m_view; }
    void load(const QUrl &url) override { m_view->load(url); }
private:
    QWebEngineProfile *m_profile;
    QPointer<QWebEngineView> m_view;
};
}

class NativeUiPlugin final : public QObject, public NativeBrowserFactory {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID NativeBrowserFactory_iid)
    Q_INTERFACES(NativeBrowserFactory)
public:
    NativeBrowser *create(const QString &origin, const QColor &background, QWidget *parent) override {
        return new WebBrowser(origin, background, parent);
    }
};

#include "NativeUiPlugin.moc"
