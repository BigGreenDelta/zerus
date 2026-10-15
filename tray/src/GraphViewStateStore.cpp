#include "GraphViewStateStore.h"
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QPromise>
#include <QRunnable>
#include <QThreadPool>
#include <memory>

namespace {
constexpr qsizetype StateLimit = 1024 * 1024;
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
                     [watcher, done = std::move(done)] {
                         const auto result = watcher->result();
                         watcher->deleteLater();
                         done(result);
                     });
    watcher->setFuture(promise->future());
    pool().start(QRunnable::create([promise, work = std::move(work)] {
        promise->addResult(work());
        promise->finish();
    }));
}
struct Loaded {
    QJsonObject state;
    QString error;
};
} // namespace

GraphViewStateStore::GraphViewStateStore(QString key, QObject *parent)
    : QObject(parent), m_key(std::move(key)), m_organization(QCoreApplication::organizationName()),
      m_application(QCoreApplication::applicationName()), m_format(QSettings::defaultFormat()) {
    m_delay.setSingleShot(true);
    m_delay.setInterval(500);
    connect(&m_delay, &QTimer::timeout, this, &GraphViewStateStore::flush);
    async<Loaded>(
        this,
        [key = m_key, organization = m_organization, application = m_application,
         format = m_format] {
            QSettings settings(format, QSettings::UserScope, organization, application);
            settings.setFallbacksEnabled(false);
            Loaded result;
            const auto bytes = settings.value(key).toByteArray();
            if (settings.status() != QSettings::NoError || bytes.size() > StateLimit) {
                result.error = QObject::tr("Saved layout could not be loaded");
                return result;
            }
            if (bytes.isEmpty())
                return result;
            const auto doc = QJsonDocument::fromJson(bytes);
            if (!doc.isObject() || doc.object().isEmpty())
                result.error = QObject::tr("Saved layout could not be loaded");
            else
                result.state = doc.object();
            return result;
        },
        [this](const Loaded &result) { emit loaded(result.state, result.error); });
}
GraphViewStateStore::~GraphViewStateStore() { write(true); }
void GraphViewStateStore::save(const QJsonObject &state) {
    if (state == m_pending && !m_failed)
        return;
    m_pending = state;
    m_dirty = true;
    m_delay.start();
}
void GraphViewStateStore::flush() { write(false); }
void GraphViewStateStore::write(bool final) {
    m_delay.stop();
    if (!m_dirty || (m_inFlight && !final))
        return;
    m_dirty = false;
    m_inFlight = true;
    async<QString>(
        this,
        [key = m_key, organization = m_organization, application = m_application, format = m_format,
         state = m_pending] {
            const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
            if (bytes.size() > StateLimit)
                return QObject::tr("Layout could not be saved on this device");
            QSettings settings(format, QSettings::UserScope, organization, application);
            settings.setFallbacksEnabled(false);
            settings.setAtomicSyncRequired(true);
            settings.setValue(key, bytes);
            settings.sync();
            return settings.status() == QSettings::NoError
                       ? QString()
                       : QObject::tr("Layout could not be saved on this device");
        },
        [this](const QString &error) {
            m_inFlight = false;
            m_failed = !error.isEmpty();
            emit saved(error);
            if (m_dirty)
                m_delay.start(0);
        });
}
