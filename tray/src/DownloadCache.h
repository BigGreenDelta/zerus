#pragma once
#include <QFutureWatcher>
#include <QPromise>
#include <QRunnable>
#include <QThreadPool>
#include <QString>
#include <memory>

namespace DownloadCache {
constexpr qint64 OpeningGraceMs = 60000;
QString directory();
qint64 limitBytes(); // Zero means never clear.
void setLimitBytes(qint64 bytes);
struct Pending {
    QString root, canonicalRoot, path;
    ~Pending();
};
struct Prepared { std::shared_ptr<Pending> pending; QString error; };
struct Result { QString path, error, warning; };
struct Usage { qint64 bytes = 0; int files = 0; QString error; };
Prepared prepare(const QString &root);
Result publish(const std::shared_ptr<Pending> &pending, const QString &name, qint64 now = -1);
Usage trim(const QString &root, qint64 limit, qint64 now = -1);
Usage usage(const QString &root);

// Cache work never blocks the GUI thread. Serialize it within this process;
// the on-disk lock also protects against another Zerus using the same folder.
inline QThreadPool &pool() {
    static QThreadPool value;
    static const bool configured = [] { value.setMaxThreadCount(1); return true; }();
    Q_UNUSED(configured); return value;
}
template<class Result, class Work, class Done>
void async(QObject *owner, Work work, Done done) {
    auto promise = std::make_shared<QPromise<Result>>(); promise->start();
    auto *watcher = new QFutureWatcher<Result>(owner);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, owner, [watcher, done = std::move(done)]() mutable {
        const auto result = watcher->result(); watcher->deleteLater(); done(result);
    });
    watcher->setFuture(promise->future());
    pool().start(QRunnable::create([promise, work = std::move(work)]() mutable {
        promise->addResult(work()); promise->finish();
    }));
}
}
