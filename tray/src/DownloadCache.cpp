#include "DownloadCache.h"
#include "LocalFile.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#endif

namespace DownloadCache::Detail {
bool sameFile(const QFile &opened, const QString &path) {
    if (!opened.isOpen() || opened.handle() < 0) return false;
#ifdef Q_OS_WIN
    // QFile owns this CRT descriptor and its native handle; do not close either.
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(opened.handle()));
    BY_HANDLE_FILE_INFORMATION before{}, current{};
    constexpr DWORD unsafe = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    if (handle == INVALID_HANDLE_VALUE || !::GetFileInformationByHandle(handle, &before)
        || (before.dwFileAttributes & unsafe)) return false;
    auto native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (!native.startsWith(QStringLiteral("\\\\?\\")))
        native = native.startsWith(QStringLiteral("\\\\"))
            ? QStringLiteral("\\\\?\\UNC\\") + native.mid(2) : QStringLiteral("\\\\?\\") + native;
    const auto other = ::CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (other == INVALID_HANDLE_VALUE) return false;
    const bool matches = ::GetFileInformationByHandle(other, &current) && !(current.dwFileAttributes & unsafe)
        && before.dwVolumeSerialNumber == current.dwVolumeSerialNumber
        && before.nFileIndexHigh == current.nFileIndexHigh && before.nFileIndexLow == current.nFileIndexLow;
    const bool closed = ::CloseHandle(other);
    return matches && closed;
#else
    struct stat before{}, current{};
    const auto name = QFile::encodeName(path);
    return ::fstat(opened.handle(), &before) == 0 && S_ISREG(before.st_mode)
        && ::lstat(name.constData(), &current) == 0 && S_ISREG(current.st_mode)
        && before.st_dev == current.st_dev && before.st_ino == current.st_ino;
#endif
}
}

namespace {
const QString Index = QStringLiteral(".zerus-files.json");
QString text(const char *value) { return QCoreApplication::translate("DownloadCache", value); }
qint64 timeNow(qint64 now) { return now < 0 ? QDateTime::currentMSecsSinceEpoch() : now; }
bool ordinary(const QFileInfo &file) {
    return LocalFile::readable(file) && !file.isSymbolicLink() && !file.isJunction();
}
bool occupied(const QString &path) {
    const QFileInfo file(path);
    return file.exists() || file.isSymbolicLink() || file.isShortcut();
}
bool safeName(const QString &name) {
    return !name.isEmpty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\\')
        && !name.contains(':') && !name.startsWith(".zerus-", Qt::CaseInsensitive);
}
bool validSource(const DownloadCache::Source &source) {
    return !source.host.isEmpty() && !source.path.isEmpty()
        && source.host.toUtf8().size() <= 4096 && source.path.toUtf8().size() <= 65536
        && QString::fromUtf8(source.host.toUtf8()) == source.host && QString::fromUtf8(source.path.toUtf8()) == source.path;
}
QByteArray sourceDigest(const DownloadCache::Source &source) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << source.host << source.path;
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}
bool sameSource(const QJsonObject &entry, const DownloadCache::Source &source, const QByteArray &digest) {
    const auto saved = entry.value("source").toObject();
    return saved.value("host").toString() == source.host && saved.value("path").toString() == source.path
        && entry.value("source_sha256").toString().toLatin1() == digest;
}
QString filename(QString name) {
    name = name.section('/', -1);
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")), "_");
    while (name.endsWith('.') || name.endsWith(' ')) name.chop(1);
    if (name.isEmpty() || name == "." || name == "..") name = "download";
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|CONIN\\$|CONOUT\\$|COM[1-9¹²³]|LPT[1-9¹²³])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name).hasMatch() || name.startsWith(".zerus-", Qt::CaseInsensitive)) name.prepend("download-");
    // Leave room for collision suffixes without losing the application suffix.
    const int dot = name.lastIndexOf('.');
    const auto extension = dot > 0 && name.mid(dot).toUtf8().size() <= 32 ? name.mid(dot) : QString();
    auto stem = extension.isEmpty() ? name : name.left(dot);
    while (stem.toUtf8().size() + extension.toUtf8().size() > 180) stem.chop(stem.back().isLowSurrogate() ? 2 : 1);
    name = stem + extension;
    return name;
}
bool folder(const QString &root, bool create, QString &error) {
    const QFileInfo before(root);
    if (create && !before.exists() && !QDir().mkpath(root)) { error = text("Cannot create the downloaded files folder."); return false; }
    const QFileInfo current(root);
    if (!current.isDir() || current.isSymbolicLink() || current.isJunction()) { error = text("The downloaded files folder is unavailable or is a link."); return false; }
    if (!before.exists()) QFile::setPermissions(root, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return true;
}
struct Snapshot { qint64 size = -1, modified = -1; QByteArray digest; };
Snapshot snapshot(const QString &path, bool hash) {
    if (!ordinary(QFileInfo(path))) return {};
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    Snapshot result; result.size = file.size(); result.modified = file.fileTime(QFileDevice::FileModificationTime).toMSecsSinceEpoch();
    if (hash) {
        QCryptographicHash digest(QCryptographicHash::Sha256);
        if (!digest.addData(&file) || file.size() != result.size || file.fileTime(QFileDevice::FileModificationTime).toMSecsSinceEpoch() != result.modified) return {};
        result.digest = digest.result().toHex();
    }
    // The hash belongs to the still-open object, not a replacement at its name.
    if (!DownloadCache::Detail::sameFile(file, path)) return {};
    return result;
}
struct Ledger {
    QString root, error;
    QJsonArray entries;
    QLockFile lock;
    explicit Ledger(const QString &path) : root(QFileInfo(path).absoluteFilePath()), lock(QDir(root).filePath(".zerus-files.lock")) {
        if (!folder(root, false, error)) return;
        // A large copy may take longer than Qt's default stale-lock age to hash.
        lock.setStaleLockTime(0);
        if (QFileInfo(QDir(root).filePath(".zerus-files.lock")).isSymbolicLink() || !lock.tryLock(5000)) { error = text("The downloaded files cache is busy."); return; }
        QFile file(QDir(root).filePath(Index));
        if (!occupied(file.fileName())) return;
        if (!ordinary(QFileInfo(file)) || file.size() > 32 * 1024 * 1024 || !file.open(QIODevice::ReadOnly)) { error = text("Cannot read the downloaded files index."); return; }
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
        const auto object = document.object();
        if (parse.error != QJsonParseError::NoError || object.value("version") != 1 || !object.value("files").isArray()) { error = text("The downloaded files index is invalid. Existing files were preserved."); return; }
        entries = object.value("files").toArray();
        QSet<QString> names;
        for (const auto &value : entries) {
            const auto entry = value.toObject();
            const auto name = entry.value("name").toString();
            if (!safeName(name) || names.contains(name) || !entry.value("bytes").isDouble() || entry.value("bytes").toDouble() < 0
                || !entry.value("saved").isDouble()
                || !QRegularExpression("^[a-f0-9]{64}$").match(entry.value("sha256").toString()).hasMatch()) {
                error = text("The downloaded files index is invalid. Existing files were preserved."); return;
            }
            names.insert(name);
            if (entry.contains("source") || entry.contains("source_sha256")) {
                const auto source = entry.value("source").toObject();
                const DownloadCache::Source identity{source.value("host").toString(), source.value("path").toString()};
                if (!entry.value("source").isObject() || source.size() != 2 || !validSource(identity)
                    || entry.value("source_sha256").toString().toLatin1() != sourceDigest(identity)) {
                    error = text("The downloaded files index is invalid. Existing files were preserved."); return;
                }
            }
        }
    }
    bool save() {
        QSaveFile file(QDir(root).filePath(Index)); file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) { error = text("Cannot save the downloaded files index."); return false; }
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        const auto data = QJsonDocument(QJsonObject{{"version", 1}, {"files", entries}}).toJson(QJsonDocument::Compact);
        if (file.write(data) != data.size() || !file.commit()) { error = text("Cannot save the downloaded files index."); return false; }
        return true;
    }
    DownloadCache::Usage clean(qint64 limit, qint64 now) {
        QList<QJsonObject> retained;
        DownloadCache::Usage usage;
        for (const auto &value : entries) {
            const auto entry = value.toObject();
            const auto current = snapshot(QDir(root).filePath(entry.value("name").toString()), false);
            // Never remove files whose contents changed. Size is a cheap first
            // filter; every deletion also checks the full content hash below.
            // Download markers (Windows ADS) can change timestamps alone.
            if (current.size < 0 || current.size != entry.value("bytes").toInteger()) continue;
            retained.append(entry); usage.bytes += current.size;
        }
        std::stable_sort(retained.begin(), retained.end(), [](const auto &a, const auto &b) { return a.value("saved").toInteger() < b.value("saved").toInteger(); });
        entries = {};
        for (const auto &entry : retained) {
            if (limit > 0 && usage.bytes > limit && now - entry.value("saved").toInteger() >= DownloadCache::OpeningGraceMs) {
                const auto path = QDir(root).filePath(entry.value("name").toString());
                const auto current = snapshot(path, true);
                if (current.size != entry.value("bytes").toInteger() || current.digest != entry.value("sha256").toString().toLatin1()) {
                    usage.bytes -= entry.value("bytes").toInteger(); continue;
                }
                if (QFile::remove(path)) { usage.bytes -= current.size; continue; }
                usage.error = text("Some older downloaded copies could not be removed.");
            }
            entries.append(entry);
        }
        usage.files = entries.size(); return usage;
    }
};
}

namespace DownloadCache {
QString directory() {
    const auto downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const auto fallback = QDir(downloads.isEmpty() ? QDir::home().filePath("Downloads") : downloads).filePath("Zerus Files");
    return QFileInfo(QSettings().value("downloads/folder", fallback).toString()).absoluteFilePath();
}
qint64 limitBytes() { return qMax(qint64(0), QSettings().value("downloads/cacheBytes", 0).toLongLong()); }
void setLimitBytes(qint64 bytes) { QSettings().setValue("downloads/cacheBytes", qMax(qint64(0), bytes)); }
Pending::~Pending() {
    if (QFileInfo(root).canonicalFilePath() == canonicalRoot && QFileInfo(path).absolutePath() == root
        && QFileInfo(path).fileName().startsWith(".zerus-part-") && ordinary(QFileInfo(path))) QFile::remove(path);
}
Prepared prepare(const QString &directory) {
    QString error; const auto root = QFileInfo(directory).absoluteFilePath();
    if (!folder(root, true, error)) return {{}, error};
    QFileInfo index(QDir(root).filePath(Index));
    if (index.isSymbolicLink()) return {{}, text("The downloaded files index is a link.")};
    { Ledger ledger(root); if (!ledger.error.isEmpty()) return {{}, ledger.error}; }
    QTemporaryFile file(QDir(root).filePath(".zerus-part-XXXXXX"));
    if (!file.open()) return {{}, text("Cannot prepare the local download.")};
    file.setAutoRemove(false);
    auto pending = std::make_shared<Pending>(); pending->root = root; pending->canonicalRoot = QFileInfo(root).canonicalFilePath();
    pending->path = file.fileName(); file.close();
    return {pending, {}};
}
Result publish(const std::shared_ptr<Pending> &pending, const QString &originalName, qint64 now, const Source &source) {
    const bool attributed = !source.host.isEmpty() || !source.path.isEmpty();
    if (attributed && !validSource(source)) return {{}, text("The downloaded file source is invalid."), {}};
    const auto identityDigest = attributed ? sourceDigest(source) : QByteArray();
    if (!pending || QFileInfo(pending->root).canonicalFilePath() != pending->canonicalRoot
        || QFileInfo(pending->path).absolutePath() != pending->root || !QFileInfo(pending->path).fileName().startsWith(".zerus-part-")) return {{}, text("The prepared download is unavailable."), {}};
    const auto data = snapshot(pending->path, true);
    if (data.size < 0) return {{}, text("The downloaded copy could not be verified."), {}};
    Ledger ledger(pending->root); if (!ledger.error.isEmpty()) return {{}, ledger.error, {}};
    const auto name = filename(originalName); const int dot = name.lastIndexOf('.');
    const auto stem = dot > 0 ? name.left(dot) : name, extension = dot > 0 ? name.mid(dot) : QString();
    QString destination;
    for (int number = 0; number < 10000; ++number) {
        const auto candidate = QDir(ledger.root).filePath(number ? stem + QString(" (%1)").arg(number) + extension : name);
        if (occupied(candidate)) continue;
        QFile file(pending->path);
        if (file.rename(candidate)) { destination = candidate; break; }
        if (!occupied(candidate)) return {{}, text("Cannot save the downloaded copy in its cache folder."), {}};
    }
    if (destination.isEmpty()) return {{}, text("Cannot find an unused name for the downloaded copy."), {}};
    QJsonObject entry{{"name", QFileInfo(destination).fileName()}, {"bytes", data.size},
        {"sha256", QString::fromLatin1(data.digest)}, {"saved", timeNow(now)}};
    if (attributed) {
        // Only the newest completed snapshot is attributed. Earlier copies stay
        // in the cache and are never overwritten or inferred from their names.
        for (qsizetype i = 0; i < ledger.entries.size(); ++i) {
            auto prior = ledger.entries[i].toObject();
            if (sameSource(prior, source, identityDigest)) { prior.remove("source"); prior.remove("source_sha256"); ledger.entries[i] = prior; }
        }
        entry.insert("source", QJsonObject{{"host", source.host}, {"path", source.path}});
        entry.insert("source_sha256", QString::fromLatin1(identityDigest));
    }
    ledger.entries.append(entry);
    // Publish before cleanup: hashing older copies must not delay opening this one.
    ledger.clean(0, timeNow(now));
    const bool saved = ledger.save();
    return {destination, {}, saved ? QString() : ledger.error};
}
Result lookup(const QString &root, const Source &source, qint64 now) {
    if (!validSource(source)) return {{}, text("The downloaded file source is invalid."), {}};
    if (!occupied(root)) return {};
    Ledger ledger(root); if (!ledger.error.isEmpty()) return {{}, ledger.error, {}};
    const auto identityDigest = sourceDigest(source);
    qsizetype selected = -1;
    for (qsizetype i = 0; i < ledger.entries.size(); ++i) if (sameSource(ledger.entries[i].toObject(), source, identityDigest)) selected = i;
    if (selected < 0) return {};
    auto entry = ledger.entries[selected].toObject();
    const auto path = QDir(ledger.root).filePath(entry.value("name").toString());
    const auto current = snapshot(path, true);
    const bool valid = ordinary(QFileInfo(path))
        && current.size == entry.value("bytes").toInteger() && current.digest == entry.value("sha256").toString().toLatin1();
    // Never adopt an older snapshot after the current copy was edited/removed.
    // Retire only attribution; all files and legacy cache entries are preserved.
    for (qsizetype i = 0; i < ledger.entries.size(); ++i) {
        auto prior = ledger.entries[i].toObject();
        if (!sameSource(prior, source, identityDigest)) continue;
        // saved is the existing cache-recency/opening-grace timestamp, not a
        // remote modification time. Older v1 trimmers honor this renewal too.
        if (valid && i == selected) { prior.insert("saved", timeNow(now)); }
        else { prior.remove("source"); prior.remove("source_sha256"); }
        ledger.entries[i] = prior;
    }
    if (!ledger.save()) return {{}, ledger.error, {}};
    return valid ? Result{path, {}, {}} : Result{};
}
Usage trim(const QString &root, qint64 limit, qint64 now) {
    if (!QFileInfo::exists(root)) return {};
    Ledger ledger(root); if (!ledger.error.isEmpty()) return {0, 0, ledger.error};
    auto result = ledger.clean(limit, timeNow(now)); if (!ledger.save()) result.error = ledger.error; return result;
}
Usage usage(const QString &root) {
    if (!QFileInfo::exists(root)) return {};
    Ledger ledger(root); if (!ledger.error.isEmpty()) return {0, 0, ledger.error};
    return ledger.clean(0, QDateTime::currentMSecsSinceEpoch());
}
}
