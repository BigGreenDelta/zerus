#include "ComposerDraftStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <memory>

namespace {
const auto PrivateDirectory = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
const auto PrivateFile = QFile::ReadOwner | QFile::WriteOwner;
QString hash(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
bool atomicWrite(const QString &path, const QByteArray &bytes, QString &error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(PrivateFile)
        || file.write(bytes) != bytes.size() || !file.commit()) {
        error = QObject::tr("Draft not saved: %1. Keep this window open until saving succeeds.").arg(file.errorString());
        return false;
    }
    return true;
}

struct WriteResult { bool okay; ComposerDraft draft; };
class DraftWrites : public QObject {
    struct Work { std::function<void()> run; };
public:
    static QPointer<DraftWrites> &reference() { static QPointer<DraftWrites> value; return value; }
    static void flushExisting() { if (auto value = reference()) value->flush(); }
    static DraftWrites *instance() {
        Q_ASSERT(QCoreApplication::instance()
                 && QThread::currentThread() == QCoreApplication::instance()->thread());
        auto &value = reference();
        if (!value) value = new DraftWrites;
        return value;
    }
    DraftWrites() : QObject(QCoreApplication::instance()), m_worker(new QObject) {
        m_thread.setObjectName("zerus-draft-writer");
        m_worker->moveToThread(&m_thread);
        connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &DraftWrites::flush);
        m_thread.start();
    }
    ~DraftWrites() override { flush(); m_thread.quit(); m_thread.wait(); }
    void flush() { QMetaObject::invokeMethod(m_worker, [] {}, Qt::BlockingQueuedConnection); }
    void observer(std::function<void()> value) { QMutexLocker lock(&m_mutex); m_observer = std::move(value); }
    void enqueue(const QString &identity, bool coalesce, std::function<void()> run) {
        std::shared_ptr<Work> work;
        {
            QMutexLocker lock(&m_mutex);
            // A read or durability operation seals preceding snapshots. Later
            // edits cannot jump across it by replacing an earlier queue slot.
            if (!coalesce) m_pending.clear();
            if (coalesce) work = m_pending.value(identity);
            if (work) { work->run = std::move(run); return; }
            work = std::make_shared<Work>(); work->run = std::move(run);
            if (coalesce) m_pending.insert(identity, work);
        }
        QMetaObject::invokeMethod(m_worker, [this, identity, work] {
            std::function<void()> run;
            {
                QMutexLocker lock(&m_mutex);
                if (m_pending.value(identity) == work) m_pending.remove(identity);
                run = std::move(work->run);
            }
            run();
        }, Qt::QueuedConnection);
    }
    void write(const QString &identity, bool coalesce, std::function<WriteResult()> write,
               QObject *owner, std::function<void(bool, const ComposerDraft &)> done) {
        const QPointer<QObject> guard(owner);
        enqueue(identity, coalesce, [this, write = std::move(write), guard, done = std::move(done)] {
            std::function<void()> observer;
            { QMutexLocker lock(&m_mutex); observer = m_observer; }
            if (observer) observer();
            const auto result = write();
            QMetaObject::invokeMethod(this, [guard, done, result] {
                if (guard && done) done(result.okay, result.draft);
            }, Qt::QueuedConnection);
        });
    }
    WriteResult sync(std::function<WriteResult()> write) {
        { QMutexLocker lock(&m_mutex); m_pending.clear(); }
        WriteResult result{};
        QMetaObject::invokeMethod(m_worker, [&result, &write] { result = write(); }, Qt::BlockingQueuedConnection);
        return result;
    }
    void read(std::function<void()> run) { enqueue({}, false, std::move(run)); }
private:
    QThread m_thread;
    QObject *m_worker;
    QMutex m_mutex;
    QHash<QString, std::shared_ptr<Work>> m_pending;
    std::function<void()> m_observer;
};
}

QString ComposerDraftStore::directory()
{
    // QSettings also provides fixture isolation on Linux and macOS without
    // changing existing organization/application identifiers or user paths.
    return QSettings().fileName() + QStringLiteral(".drafts");
}

ComposerDraftStore::ComposerDraftStore() : m_root(directory()) {}
QString ComposerDraftStore::root() const { return m_root; }
QString ComposerDraftStore::folder(const QString &key) const { return root() + '/' + hash(key.toUtf8()); }

void ComposerDraftStore::flushWrites() { DraftWrites::flushExisting(); }
void ComposerDraftStore::setWriteObserver(std::function<void()> observer) { DraftWrites::instance()->observer(std::move(observer)); }

bool ComposerDraftStore::save(const QString &key, ComposerDraft &draft) const
{
    if (key.isEmpty()) return true;
    auto store = *this; store.m_root = root();
    auto result = DraftWrites::instance()->sync(
        [store, key, copy = draft]() mutable { const bool okay = store.saveDirect(key, copy); return WriteResult{okay, std::move(copy)}; });
    draft = std::move(result.draft); return result.okay;
}

void ComposerDraftStore::saveAsync(const QString &key, const ComposerDraft &draft, QObject *owner,
                                 std::function<void(bool, const ComposerDraft &)> done) const
{
    if (key.isEmpty()) return;
    auto store = *this; store.m_root = root();
    DraftWrites::instance()->write(store.m_root + QChar(0) + key, true,
        [store, key, copy = draft]() mutable { const bool okay = store.saveDirect(key, copy); return WriteResult{okay, std::move(copy)}; },
        owner, std::move(done));
}

ComposerDraft ComposerDraftStore::load(const QString &key) const
{
    flushWrites(); return loadDirect(key);
}

ComposerDraft ComposerDraftStore::loadDirect(const QString &key) const
{
    ComposerDraft draft;
    QFile file(folder(key) + "/draft.json");
    if (!file.exists()) return draft;
    auto invalid = [&draft] {
        draft.storageError = QObject::tr("Could not restore this draft completely. Saved files have been kept locally.");
    };
    if (!file.open(QIODevice::ReadOnly)) { invalid(); return draft; }
    QJsonParseError error;
    const auto object = QJsonDocument::fromJson(file.readAll(), &error).object();
    if (error.error != QJsonParseError::NoError || object.value("version").toInt() != 1 || object.value("key").toString() != key) {
        invalid(); return draft;
    }
    draft.text = object.value("text").toString();
    draft.label = object.value("label").toString(); draft.formState = object.value("form_state").toObject();
    draft.position = object.value("position").toInt(); draft.anchor = object.value("anchor").toInt();
    draft.nextAttachmentNumber = qMax(1, object.value("next_attachment_number").toInt(1));
    draft.uncertain = object.value("uncertain").toBool() || object.value("sending").toBool();
    draft.error = draft.uncertain;
    if (draft.uncertain) draft.notice = QObject::tr("Delivery was interrupted. Check Activity or Terminal before sending again.");
    qsizetype total = 0;
    for (const auto &value : object.value("attachments").toArray()) {
        const auto attachment = value.toObject();
        const auto digest = attachment.value("sha256").toString();
        if (digest.size() != 64 || digest.contains(QRegularExpression("[^0-9a-f]"))
            || draft.attachments.size() >= HgsClient::MaximumAttachments) { invalid(); break; }
        QFile bytesFile(folder(key) + '/' + digest + ".bin");
        if (!bytesFile.open(QIODevice::ReadOnly) || bytesFile.size() > HgsClient::MaximumAttachmentBytes) { invalid(); continue; }
        const auto bytes = bytesFile.read(HgsClient::MaximumAttachmentBytes + 1);
        total += bytes.size();
        if (bytes.isEmpty() || total > HgsClient::MaximumAttachmentsBytes || hash(bytes) != digest
            || bytes.size() != attachment.value("bytes").toInteger()) { invalid(); continue; }
        draft.attachments.append({attachment.value("name").toString(), attachment.value("mime").toString(), bytes, attachment.value("reference").toString()});
        draft.attachmentHashes.append(digest);
    }
    return draft;
}

bool ComposerDraftStore::saveDirect(const QString &key, ComposerDraft &draft) const
{
    if (key.isEmpty()) return true;
    const auto path = folder(key);
    if (!QDir().mkpath(path) || !QFile::setPermissions(root(), PrivateDirectory) || !QFile::setPermissions(path, PrivateDirectory)) {
        draft.storageError = QObject::tr("Draft not saved: cannot create private draft storage. Keep this window open until saving succeeds.");
        return false;
    }
    if (draft.attachmentHashes.size() != draft.attachments.size()) {
        draft.attachmentHashes.clear();
        for (const auto &attachment : draft.attachments) draft.attachmentHashes.append(hash(attachment.data));
    }
    QJsonArray attachments;
    QSet<QString> retained;
    for (int i = 0; i < draft.attachments.size(); ++i) {
        const auto &attachment = draft.attachments[i]; const auto digest = draft.attachmentHashes[i];
        const auto name = digest + ".bin"; retained.insert(name);
        if (!QFile::exists(path + '/' + name) && !atomicWrite(path + '/' + name, attachment.data, draft.storageError)) return false;
        attachments.append(QJsonObject{{"name", attachment.name}, {"mime", attachment.mime}, {"reference", attachment.reference},
                                      {"sha256", digest}, {"bytes", attachment.data.size()}});
    }
    const QJsonObject object{{"version", 1}, {"key", key}, {"text", draft.text}, {"position", draft.position}, {"anchor", draft.anchor},
                            {"label", draft.label}, {"form_state", draft.formState},
                            {"next_attachment_number", draft.nextAttachmentNumber}, {"sending", draft.sending}, {"uncertain", draft.uncertain},
                            {"attachments", attachments}};
    if (!atomicWrite(path + "/draft.json", QJsonDocument(object).toJson(QJsonDocument::Compact), draft.storageError)) return false;
    draft.storageError.clear();
    // The committed manifest is authoritative, including an empty tombstone
    // after success. A crash during cleanup cannot resurrect sent attachments.
    for (const auto &name : QDir(path).entryList({"*.bin"}, QDir::Files))
        if (!retained.contains(name)) QFile::remove(path + '/' + name);
    return true;
}

QStringList ComposerDraftStore::keys() const
{
    flushWrites(); auto result = indexDirect().keys(); result.sort(); return result;
}

QHash<QString, ComposerDraftSummary> ComposerDraftStore::indexDirect() const
{
    QHash<QString, ComposerDraftSummary> result;
    for (const auto &name : QDir(root()).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile file(root() + '/' + name + "/draft.json");
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        const auto key = object.value("key").toString();
        const auto text = object.value("text").toString();
        const auto attachments = object.value("attachments").toArray();
        if (object.value("version").toInt() != 1 || name != hash(key.toUtf8()) || (text.trimmed().isEmpty() && attachments.isEmpty()))
            continue;
        ComposerDraftSummary summary;
        summary.label = object.value("label").toString();
        summary.modified = file.fileTime(QFileDevice::FileModificationTime);
        summary.attachments = attachments.size();
        for (const auto &line : text.split('\n')) if (!line.trimmed().isEmpty()) { summary.preview = line.simplified().left(200); break; }
        result.insert(key, summary);
    }
    return result;
}

ComposerDraftOwner ComposerDraftStore::owner(const QString &key)
{
    static const QString Voice = QStringLiteral("voice-message-draft\n"), Question = QStringLiteral("question\n");
    static const QString Task = QStringLiteral("task-draft\n");
    for (const auto &[prefix, kind] : {std::pair{Voice, ComposerDraftOwner::Kind::Voice}, std::pair{Task, ComposerDraftOwner::Kind::Task}}) {
        if (!key.startsWith(prefix)) continue;
        auto result = owner(key.mid(prefix.size()));
        if (result.kind == ComposerDraftOwner::Kind::Message) { result.kind = kind; return result; }
    }
    if (key.startsWith(Question)) {
        // A question identity is a compact JSON array, so it holds no line break.
        const auto identity = QJsonDocument::fromJson(key.mid(Question.size()).section('\n', 0, 0).toUtf8()).array();
        auto result = owner(identity.at(0).toString());
        if (result.kind != ComposerDraftOwner::Kind::Unknown) { result.kind = ComposerDraftOwner::Kind::Question; return result; }
    }
    const auto parts = key.split('\n');
    ComposerDraftOwner result;
    if (parts.size() < 2 || parts.size() > 4 || parts[1].isEmpty()) return result;
    result.machine = parts[0]; result.session = parts[1];
    if (parts.size() == 4) {
        result.kind = ComposerDraftOwner::Kind::Subagent; result.conversation = parts[2]; result.subagent = parts[3];
        if (result.subagent.isEmpty()) return {};
    } else {
        result.kind = ComposerDraftOwner::Kind::Message;
        if (parts.size() == 3) result.archive = parts[2];
    }
    return result;
}

void ComposerDraftStore::saveDurableAsync(const QString &key, const ComposerDraft &draft, QObject *owner,
                                        std::function<void(bool, const ComposerDraft &)> done) const
{
    const auto store = *this;
    DraftWrites::instance()->write(root() + QChar(0) + key, false,
        [store, key, copy = draft]() mutable { const bool okay = store.saveDirect(key, copy); return WriteResult{okay, std::move(copy)}; },
        owner, std::move(done));
}

void ComposerDraftStore::loadAsync(const QString &key, QObject *owner, std::function<void(const ComposerDraft &)> done) const
{
    const auto store = *this; const QPointer<QObject> guard(owner); auto *queue = DraftWrites::instance();
    queue->read([store, key, guard, done = std::move(done), queue] {
        const auto result = store.loadDirect(key);
        QMetaObject::invokeMethod(queue, [guard, done, result] { if (guard) done(result); }, Qt::QueuedConnection);
    });
}

void ComposerDraftStore::indexAsync(QObject *owner, std::function<void(const QHash<QString, ComposerDraftSummary> &)> done) const
{
    const auto store = *this; const QPointer<QObject> guard(owner); auto *queue = DraftWrites::instance();
    queue->read([store, guard, done = std::move(done), queue] {
        const auto result = store.indexDirect();
        QMetaObject::invokeMethod(queue, [guard, done, result] { if (guard) done(result); }, Qt::QueuedConnection);
    });
}

void ComposerDraftStore::renameAsync(const QString &oldKey, const QString &newKey, const ComposerDraft &draft,
                                   QObject *owner, std::function<void(bool, const ComposerDraft &)> done) const
{
    const auto store = *this;
    DraftWrites::instance()->write(root() + QChar(0) + oldKey, false,
        [store, oldKey, newKey, copy = draft]() mutable {
            const auto destination = store.loadDirect(newKey);
            if (!destination.text.isEmpty() || !destination.attachments.isEmpty() || !destination.storageError.isEmpty()) {
                copy.storageError = QObject::tr("Another draft already exists here. Recover the previous one from Saved drafts.");
                return WriteResult{false, copy};
            }
            if (!store.saveDirect(newKey, copy)) return WriteResult{false, copy};
            ComposerDraft empty;
            // Retire only after the destination is durable. On failure both
            // drafts remain recoverable, and the caller keeps the old binding.
            const bool okay = store.saveDirect(oldKey, empty);
            if (!okay) copy.storageError = empty.storageError;
            return WriteResult{okay, copy};
        }, owner, std::move(done));
}

QString ComposerDraftStore::label(const QString &key) const
{
    flushWrites();
    QFile file(folder(key) + "/draft.json");
    if (file.open(QIODevice::ReadOnly)) {
        const auto label = QJsonDocument::fromJson(file.readAll()).object().value("label").toString();
        if (!label.isEmpty()) return label;
    }
    return QString(key).replace('\n', " / ");
}
