#include "GraphWorkspace.h"
#include "GraphController.h"
#include "HgsClient.h"
#include <QFutureWatcher>
#include <QPromise>
#include <QRunnable>
#include <QThreadPool>
#include <QVBoxLayout>
#include <memory>

namespace {
QThreadPool &projectionPool() {
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(1);
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}
} // namespace

GraphWorkspace::GraphWorkspace(const QString &hgs, const QString &path, QWidget *parent)
    : QWidget(parent), m_client(new HgsClient(hgs, this)),
      m_controller(new GraphController(path, this)), m_page(new GraphPage(this)) {
    setObjectName("graphWorkspace");
    m_client->setObjectName("graphClient");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_page);
    connect(m_controller, &GraphController::requestPage, this,
            [this](quint64 id, const QString &route, bool changes, const QString &cursor) {
                m_request = id;
                m_route = route;
                m_transport = m_client->requestGraph(route, changes, cursor);
            });
    connect(m_client, &HgsClient::graphReady, this,
            [this](quint64 id, const QString &route, const QJsonObject &page) {
                if (id != m_transport || route != m_route)
                    return;
                m_transport = 0;
                m_controller->received(m_request, route, page);
            });
    connect(m_client, &HgsClient::graphFailed, this,
            [this](quint64 id, const QString &route, const QString &error) {
                if (id != m_transport || route != m_route)
                    return;
                m_transport = 0;
                m_controller->failed(m_request, route, error);
            });
    connect(m_controller, &GraphController::projectionChanged, this, [this] {
        ++m_generation;
        m_dirty = true;
        project();
    });
    connect(m_controller, &GraphController::statusChanged, this, &GraphWorkspace::status);
    connect(m_page, &GraphPage::refreshRequested, m_controller, &GraphController::refresh);
    connect(m_page, &GraphPage::openRequested, this, &GraphWorkspace::openRequested);
    status();
}
void GraphWorkspace::setRoutes(const QSet<QString> &routes) {
    if (m_configured && routes == m_routes)
        return;
    m_configured = true;
    m_routes = routes;
    ++m_generation;
    m_dirty = true;
    m_controller->setRoutes(routes);
    project();
}
void GraphWorkspace::setContext(const QSet<QString> &routes,
                                const QList<GraphProjection::Binding> &bindings, int omitted,
                                const QJsonObject &context) {
    m_context = context;
    m_bindings = bindings.first(qMin(bindings.size(), qsizetype(MaximumBindings)));
    m_bindingOmitted = qMax(0, omitted) + qMax(0, int(bindings.size()) - MaximumBindings);
    ++m_generation;
    m_dirty = true;
    setRoutes(routes);
    project();
}
void GraphWorkspace::setTheme(bool dark) { m_page->setTheme(dark); }
void GraphWorkspace::setFilterCatalog(const FleetState &fleet,
                                      const QList<SessionOrganization::Group> &projects) {
    m_page->setFilterCatalog(fleet, projects);
}
void GraphWorkspace::focusTarget(const QString &host, const QString &name, const QString &run,
                                 const QString &conversation, const QString &archive) {
    m_focusTarget = {{"host", host},
                     {"name", name},
                     {"run", run},
                     {"conversation", conversation},
                     {"archive", archive}};
    if (m_page->focusTarget(host, name, run, conversation, archive))
        m_focusTarget = {};
}
void GraphWorkspace::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    m_controller->setActive(true);
    project();
}
void GraphWorkspace::hideEvent(QHideEvent *event) {
    m_focusTarget = {};
    m_controller->setActive(false);
    QWidget::hideEvent(event);
}
void GraphWorkspace::project() {
    if (!isVisible() || !m_dirty || m_projecting)
        return;
    m_dirty = false;
    m_projecting = true;
    const auto generation = m_generation;
    QSet<QString> confirmed;
    for (const auto &route : m_controller->store().routes())
        if (m_controller->confirmed(route))
            confirmed.insert(route);
    auto promise = std::make_shared<QPromise<GraphProjection::Result>>();
    promise->start();
    auto *watcher = new QFutureWatcher<GraphProjection::Result>(this);
    connect(watcher, &QFutureWatcher<GraphProjection::Result>::finished, this,
            [this, watcher, generation] {
                const auto result = watcher->result();
                watcher->deleteLater();
                m_projecting = false;
                if (generation == m_generation && isVisible()) {
                    m_page->setProjection(result.nodes, result.relations, result.aliases);
                    if (!m_focusTarget.isEmpty() &&
                        m_page->focusTarget(m_focusTarget["host"].toString(),
                                            m_focusTarget["name"].toString(),
                                            m_focusTarget["run"].toString(),
                                            m_focusTarget["conversation"].toString(),
                                            m_focusTarget["archive"].toString()))
                        m_focusTarget = {};
                    m_omitted = result.omitted;
                    m_unresolved = result.unresolved;
                    status();
                } else
                    m_dirty = true;
                project();
            });
    watcher->setFuture(promise->future());
    projectionPool().start(
        QRunnable::create([promise, store = m_controller->store(), bindings = m_bindings, confirmed,
                           context = m_context] {
            promise->addResult(
                GraphProjection::compose(store, bindings, confirmed, MaximumBindings, context));
            promise->finish();
        }));
}
void GraphWorkspace::status() {
    auto summary = m_controller->summary();
    auto details = m_controller->details();
    if (m_omitted)
        summary += tr(" / %1 outside this view").arg(m_omitted);
    if (m_bindingOmitted)
        summary += tr(" / %1 current bindings outside this view").arg(m_bindingOmitted);
    if (m_unresolved)
        details += tr("\n%1 records lack a verified endpoint or relationship.").arg(m_unresolved);
    m_page->setFreshness(summary, details);
}
