#include "GraphController.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QPromise>
#include <QRunnable>
#include <QSaveFile>
#include <QThreadPool>
#include <algorithm>
#include <memory>

namespace {
constexpr qint64 CacheLimit = 32 * 1024 * 1024;
QThreadPool &pool() {
    static QThreadPool instance;
    static const bool configured = [] {
        instance.setMaxThreadCount(1);
        return true;
    }();
    Q_UNUSED(configured);
    return instance;
}
template <class Result, class Work, class Done> void async(QObject *owner, Work work, Done done) {
    auto promise = std::make_shared<QPromise<Result>>();
    promise->start();
    auto *watcher = new QFutureWatcher<Result>(owner);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, owner,
                     [watcher, done = std::move(done)]() mutable {
                         const auto result = watcher->result();
                         watcher->deleteLater();
                         done(result);
                     });
    watcher->setFuture(promise->future());
    pool().start(QRunnable::create([promise, work = std::move(work)]() mutable {
        promise->addResult(work());
        promise->finish();
    }));
}
struct Loaded {
    SessionGraphStore store;
    QString error;
};
struct Applied {
    SessionGraphStore store;
    SessionGraphStore::Result result = SessionGraphStore::Rejected;
    bool changed = false;
};
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
QString routeName(const QString &route) {
    return route.isEmpty() ? QObject::tr("This computer") : route;
}
} // namespace

GraphController::GraphController(QString path, QObject *parent)
    : QObject(parent), m_cachePath(std::move(path)) {
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &GraphController::tick);
    m_save.setSingleShot(true);
    m_save.setInterval(1000);
    connect(&m_save, &QTimer::timeout, this, &GraphController::persist);
    if (m_cachePath.isEmpty())
        return;
    m_loading = true;
    async<Loaded>(
        this,
        [path = m_cachePath] {
            Loaded result;
            const QFileInfo info(path);
            if (!info.exists())
                return result;
            if (!info.isFile() || info.isSymLink() || info.size() > CacheLimit) {
                result.error = QObject::tr("Saved graph cache is unavailable");
                return result;
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) ||
                !result.store.restore(QJsonDocument::fromJson(file.read(CacheLimit + 1)).object()))
                result.error = QObject::tr("Saved graph cache is unavailable");
            return result;
        },
        [this](const Loaded &result) {
            m_loading = false;
            m_store = result.store;
            m_cacheError = result.error;
            prune();
            if (m_configured && result.error.isEmpty())
                m_save.start();
            emit projectionChanged();
            emit statusChanged();
            tick();
        });
}
void GraphController::setRoutes(const QSet<QString> &routes) {
    if (routes.size() > 128)
        return;
    for (const auto &route : routes)
        if (route.size() > 256 || route.contains(QChar::Null) || route.contains('\n') ||
            route.contains('\r'))
            return;
    m_configured = true;
    QSet<QString> previous;
    for (auto it = m_routes.cbegin(); it != m_routes.cend(); ++it)
        previous.insert(it.key());
    if (previous == routes) {
        prune();
        m_save.start();
        return;
    }
    ++m_generation;
    for (const auto &route : m_routes.keys())
        if (!routes.contains(route)) {
            m_store.forget(route);
            m_routes.remove(route);
        }
    for (const auto &route : routes)
        if (!m_routes.contains(route))
            m_routes.insert(route, {});
    prune();
    m_save.start();
    emit projectionChanged();
    emit statusChanged();
    tick();
}
void GraphController::prune() {
    if (!m_configured)
        return;
    for (const auto &route : m_store.routes())
        if (!m_routes.contains(route))
            m_store.forget(route);
}
void GraphController::setActive(bool active) {
    m_active = active;
    if (active) {
        m_timer.start();
        tick();
    } else
        m_timer.stop();
}
void GraphController::refresh() {
    for (auto &route : m_routes)
        route.due = 0;
    tick();
}
bool GraphController::confirmed(const QString &route) const {
    return m_routes.value(route).confirmed;
}

void GraphController::tick() {
    if (!m_active || m_loading || m_working)
        return;
    if (m_pending) {
        emit statusChanged();
        if (now() - m_started > 30000)
            failed(m_pending, m_route, tr("Graph request timed out"));
        return;
    }
    QStringList due;
    for (auto it = m_routes.cbegin(); it != m_routes.cend(); ++it)
        if (it->due <= now())
            due.append(it.key());
    if (due.isEmpty())
        return;
    due.sort();
    const auto after = std::find_if(due.cbegin(), due.cend(),
                                    [this](const QString &value) { return value > m_lastRoute; });
    m_route = !m_haveLastRoute || after == due.cend() ? due.first() : *after;
    m_lastRoute = m_route;
    m_haveLastRoute = true;
    auto &state = m_routes[m_route];
    const auto *host = m_store.host(m_route);
    m_changes = host && !state.snapshot;
    m_cursor = state.snapshot ? state.cursor : (host ? host->cursor : QString());
    m_pending = ++m_sequence;
    m_requestGeneration = m_generation;
    m_started = now();
    emit statusChanged();
    emit requestPage(m_pending, m_route, m_changes, m_cursor);
}
void GraphController::received(quint64 request, const QString &route, const QJsonObject &page) {
    if (!m_pending || request != m_pending || route != m_route)
        return;
    m_pending = 0;
    if (m_requestGeneration != m_generation || !m_routes.contains(route)) {
        tick();
        return;
    }
    const auto generation = m_generation;
    const bool changes = m_changes;
    const auto cursor = m_cursor;
    m_working = true;
    async<Applied>(
        this,
        [store = m_store, route, page, changes, cursor]() mutable {
            Applied value;
            const auto previous = store.host(route) ? store.host(route)->items : QJsonObject();
            value.result = store.apply(route, page, changes, cursor, now());
            value.changed = store.host(route) && store.host(route)->items != previous;
            value.store = std::move(store);
            return value;
        },
        [this, route, generation, changes](const Applied &result) {
            m_working = false;
            if (generation != m_generation || !m_routes.contains(route)) {
                tick();
                return;
            }
            auto &state = m_routes[route];
            const bool wasConfirmed = state.confirmed;
            if (result.result == SessionGraphStore::Rejected) {
                state.confirmed = false;
                state.error = tr("This computer returned an invalid graph page");
                state.due = now() + 30000;
                m_store.discardPending(route);
                state.snapshot = false;
                state.cursor.clear();
            } else if (result.result == SessionGraphStore::ResetRequired) {
                m_store.discardPending(route);
                state.snapshot = true;
                state.cursor.clear();
                state.confirmed = false;
                state.due = now() + (++state.resets > 2 ? 30000 : 0);
                state.error = state.resets > 2
                                  ? tr("Graph changed repeatedly; retaining the last snapshot")
                                  : QString();
            } else {
                m_store = result.store;
                state.resets = 0;
                state.error.clear();
                state.cursor = m_store.nextCursor(route);
                state.snapshot = !changes && result.result == SessionGraphStore::More;
                const auto *host = m_store.host(route);
                if (!state.snapshot && host) {
                    state.confirmed = true;
                    state.lastSuccess = now();
                    m_save.start();
                }
                const bool partial = host && !host->coverage.value("bindings_complete").toBool();
                state.due =
                    now() + (result.result == SessionGraphStore::More || partial ? 200 : 5000);
            }
            if (result.changed || wasConfirmed != state.confirmed)
                emit projectionChanged();
            emit statusChanged();
            tick();
        });
}
void GraphController::failed(quint64 request, const QString &route, const QString &error) {
    if (!m_pending || request != m_pending || route != m_route)
        return;
    m_pending = 0;
    if (m_requestGeneration != m_generation || !m_routes.contains(route)) {
        tick();
        return;
    }
    auto &state = m_routes[route];
    state.error = error.left(1000);
    state.confirmed = false;
    state.due = now() + 30000;
    state.snapshot = false;
    state.cursor.clear();
    m_store.discardPending(route);
    emit projectionChanged();
    emit statusChanged();
    tick();
}
void GraphController::persist() {
    if (m_cachePath.isEmpty() || m_loading)
        return;
    async<QString>(
        this,
        [path = m_cachePath, store = m_store] {
            const auto bytes = QJsonDocument(store.cache()).toJson(QJsonDocument::Compact);
            if (bytes.size() > CacheLimit || QFileInfo(path).isSymLink() ||
                !QDir().mkpath(QFileInfo(path).absolutePath()))
                return QObject::tr("Could not save the graph cache");
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                return QObject::tr("Could not save the graph cache");
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            if (file.write(bytes) != bytes.size() || !file.commit())
                return QObject::tr("Could not save the graph cache");
            return QString();
        },
        [this](const QString &error) {
            m_cacheError = error;
            emit statusChanged();
        });
}
QString GraphController::summary() const {
    if (m_loading)
        return tr("Loading saved graph…");
    int confirmedCount = 0;
    for (const auto &route : m_routes)
        if (route.confirmed)
            ++confirmedCount;
    if (m_pending && now() - m_started >= 2000)
        return tr("Loading connections from %1…").arg(routeName(m_route));
    if (m_routes.isEmpty())
        return tr("No configured computers");
    if (!confirmedCount)
        return m_store.routes().isEmpty() ? tr("Waiting for graph data")
                                          : tr("Last known graph · refreshing available computers");
    return tr("%1 of %2 computers refreshed · current states, recorded connections")
        .arg(confirmedCount)
        .arg(m_routes.size());
}
QString GraphController::details() const {
    QStringList result;
    auto routes = m_routes.keys();
    routes.sort();
    for (const auto &route : routes) {
        const auto &state = m_routes[route];
        const auto *host = m_store.host(route);
        QString text = routeName(route) + ": ";
        if (!state.error.isEmpty())
            text += state.error;
        else if (host) {
            text +=
                tr("last updated %1")
                    .arg(
                        QDateTime::fromMSecsSinceEpoch(host->sampledAt).toString("d MMM HH:mm:ss"));
            if (!host->coverage.value("bindings_complete").toBool())
                text += tr(" · partly indexed");
            if (host->coverage.value("communication") == "not_indexed")
                text += tr(" · communication history not indexed");
            if(host->coverage.value("communication")=="verified_zerus_sends")
                text += tr(" · verified Zerus deliveries (30 days); older unattributed exchanges unavailable");
            const auto delivery=host->coverage["delivery_recording"].toObject();
            if(delivery["unknown_deliveries"].toInt()>0)text+=tr(" · %1 send attempts lack verified endpoint metadata").arg(delivery["unknown_deliveries"].toInt());
            if(delivery["recording_gap"].toBool())text+=tr(" · some delivery metadata could not be recorded");
            if (host->coverage.value("source_errors").toInt() > 0)
                text += tr(" · some source metadata unavailable");
        } else
            text += tr("not loaded");
        result.append(text);
    }
    if (!m_cacheError.isEmpty())
        result.append(m_cacheError);
    return result.join('\n');
}
