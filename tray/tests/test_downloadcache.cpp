#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <fcntl.h>
#include <io.h>
#endif
#include <future>
#include "DownloadCache.h"

namespace {
bool write(const QString &path, const QByteArray &data) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
DownloadCache::Result copy(const QString &root, const QString &name, const QByteArray &data, qint64 saved, const DownloadCache::Source &source = {}) {
    auto prepared = DownloadCache::prepare(root);
    if (!prepared.error.isEmpty()) return {{}, prepared.error, {}};
    if (!write(prepared.pending->path, data)) return {{}, "fixture write failed", {}};
    return DownloadCache::publish(prepared.pending, name, saved, source);
}
}

class TestDownloadCache : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    }
    void init() { QSettings().clear(); }
    void defaultNeverClearAndFailedPreparation() {
        QCOMPARE(DownloadCache::limitBytes(), 0);
        QVERIFY(DownloadCache::directory().endsWith("/Zerus Files"));
        QTemporaryDir dir; const auto root = dir.filePath("Zerus Files");
        { auto prepared = DownloadCache::prepare(root); QVERIFY2(prepared.error.isEmpty(), qPrintable(prepared.error));
          QVERIFY(QFileInfo::exists(prepared.pending->path)); }
        QVERIFY(QDir(root).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        const auto file = dir.filePath("ordinary file"); QVERIFY(write(file, "user data"));
        QVERIFY(!DownloadCache::prepare(file).error.isEmpty()); QCOMPARE(read(file), QByteArray("user data"));
    }
    void names_data() {
        QTest::addColumn<QString>("remote"); QTest::addColumn<QString>("expected");
        QTest::newRow("literal") << "/remote/résumé ' $(literal).pdf" << "résumé ' $(literal).pdf";
        QTest::newRow("reserved") << "/remote/CON.txt" << "download-CON.txt";
        QTest::newRow("metadata") << "/remote/.zerus-files.json" << "download-.zerus-files.json";
        QTest::newRow("metadata-case-insensitive") << "/remote/.ZERUS-files.json" << "download-.ZERUS-files.json";
        QTest::newRow("invalid-characters") << "/remote/a:b?c.txt" << "a_b_c.txt";
        QTest::newRow("long") << "/remote/" + QString(200, 'a') + ".pdf" << QString(176, 'a') + ".pdf";
        QTest::newRow("empty") << "/" << "download";
    }
    void names() {
        QFETCH(QString, remote); QFETCH(QString, expected); QTemporaryDir dir;
        const auto result = copy(dir.path(), remote, "sample", 1000);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error)); QCOMPARE(QFileInfo(result.path).fileName(), expected);
        QCOMPARE(read(result.path), QByteArray("sample"));
    }
    void collisionsNeverOverwrite() {
        QTemporaryDir dir; QVERIFY(write(dir.filePath("report.pdf"), "user copy"));
        QVERIFY(QDir().mkdir(dir.filePath("report (1).pdf")));
        const auto first = copy(dir.path(), "/one/report.pdf", "one", 1000);
        const auto second = copy(dir.path(), "/two/report.pdf", "two", 2000);
        QVERIFY2(first.error.isEmpty(), qPrintable(first.error)); QVERIFY2(second.error.isEmpty(), qPrintable(second.error));
        QCOMPARE(QFileInfo(first.path).fileName(), QString("report (2).pdf"));
        QCOMPARE(QFileInfo(second.path).fileName(), QString("report (3).pdf"));
        QCOMPARE(read(dir.filePath("report.pdf")), QByteArray("user copy"));
        QCOMPARE(read(first.path), QByteArray("one")); QCOMPARE(read(second.path), QByteArray("two"));
    }
    void concurrentCopies() {
        QTemporaryDir dir;
        auto one = DownloadCache::prepare(dir.path()), two = DownloadCache::prepare(dir.path());
        QVERIFY(one.pending); QVERIFY(two.pending); QVERIFY(write(one.pending->path, "first")); QVERIFY(write(two.pending->path, "second"));
        auto first = std::async(std::launch::async, [&] { return DownloadCache::publish(one.pending, "same.txt", 1000); });
        auto second = std::async(std::launch::async, [&] { return DownloadCache::publish(two.pending, "same.txt", 1000); });
        const auto a = first.get(), b = second.get();
        QVERIFY2(a.error.isEmpty(), qPrintable(a.error)); QVERIFY2(b.error.isEmpty(), qPrintable(b.error)); QVERIFY(a.path != b.path);
        QCOMPARE(read(a.path), QByteArray("first")); QCOMPARE(read(b.path), QByteArray("second"));
        const auto usage = DownloadCache::usage(dir.path()); QVERIFY(usage.error.isEmpty()); QCOMPARE(usage.files, 2); QCOMPARE(usage.bytes, 11);
    }
    void budgetHonorsOpeningGraceAndNeverClear() {
        QTemporaryDir dir;
        const auto a = copy(dir.path(), "old.txt", "12345", 1000), b = copy(dir.path(), "middle.txt", "12345678", 2000),
            c = copy(dir.path(), "new.txt", "12345678901", 3000);
        QVERIFY(a.error.isEmpty()); QVERIFY(b.error.isEmpty()); QVERIFY(c.error.isEmpty());
        QVERIFY(write(dir.filePath("unrelated.txt"), QByteArray(100, 'u')));
        auto usage = DownloadCache::trim(dir.path(), 15, 6000); QCOMPARE(usage.files, 3); QCOMPARE(usage.bytes, 24);
        usage = DownloadCache::trim(dir.path(), 0, 100000); QCOMPARE(usage.files, 3); QCOMPARE(usage.bytes, 24);
        usage = DownloadCache::trim(dir.path(), 15, 100000); QVERIFY2(usage.error.isEmpty(), qPrintable(usage.error));
        QCOMPARE(usage.files, 1); QCOMPARE(usage.bytes, 11); QVERIFY(QFileInfo::exists(c.path));
        QVERIFY(!QFileInfo::exists(a.path)); QVERIFY(!QFileInfo::exists(b.path));
        QCOMPARE(read(dir.filePath("unrelated.txt")), QByteArray(100, 'u'));
        // Even one oversized copy is removed after the opening grace has elapsed.
        usage = DownloadCache::trim(dir.path(), 1, 100000); QCOMPARE(usage.files, 0); QVERIFY(!QFileInfo::exists(c.path));
    }
    void preservesEditedCopiesAndActiveTransfers() {
        QTemporaryDir dir;
        const auto edited = copy(dir.path(), "edited.txt", "original", 1000), other = copy(dir.path(), "other.txt", "old copy", 1000);
        QVERIFY(edited.error.isEmpty()); QVERIFY(other.error.isEmpty());
        QFile file(edited.path); QVERIFY(file.open(QIODevice::ReadWrite)); const auto modified = file.fileTime(QFileDevice::FileModificationTime);
        QCOMPARE(file.write("modified"), 8); QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime)); file.close();
        auto active = DownloadCache::prepare(dir.path()); QVERIFY(active.pending); QVERIFY(write(active.pending->path, "in flight"));
        const auto usage = DownloadCache::trim(dir.path(), 1, 100000); QVERIFY2(usage.error.isEmpty(), qPrintable(usage.error));
        QCOMPARE(read(edited.path), QByteArray("modified")); QVERIFY(!QFileInfo::exists(other.path));
        QCOMPARE(read(active.pending->path), QByteArray("in flight")); QCOMPARE(usage.files, 0);
    }
    void metadataChangesDoNotOrphanUnchangedCopies() {
        QTemporaryDir dir; const auto result = copy(dir.path(), "copy.txt", "unchanged", 1000);
        QVERIFY(result.error.isEmpty());
        QFile file(result.path); QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::fromMSecsSinceEpoch(5000000), QFileDevice::FileModificationTime)); file.close();
        const auto usage = DownloadCache::trim(dir.path(), 1, 100000);
        QVERIFY(usage.error.isEmpty()); QCOMPARE(usage.files, 0); QVERIFY(!QFileInfo::exists(result.path));
    }
    void invalidIndexFailsWithoutDeletingAnything() {
        QTemporaryDir dir; QVERIFY(write(dir.filePath("keep.txt"), "user copy"));
        QVERIFY(write(dir.filePath(".zerus-files.json"), "invalid index"));
        const auto result = copy(dir.path(), "keep.txt", "new copy", 1000); QVERIFY(!result.error.isEmpty());
        QVERIFY(!DownloadCache::trim(dir.path(), 1, 100000).error.isEmpty());
        QCOMPARE(read(dir.filePath("keep.txt")), QByteArray("user copy"));
        QVERIFY(!QFileInfo::exists(dir.filePath("keep (1).txt")));
    }
    void openFileIdentityRejectsAtomicPathReplacement() {
        QTemporaryDir dir; const auto path = dir.filePath("copy.txt");
        QVERIFY(write(path, "opened-A"));
        QFile opened;
#ifdef Q_OS_WIN
        // Ordinary QFile denies share-delete on Windows. Deliberately allow it
        // here so the same-object helper is tested against a real replacement.
        const auto native = QDir::toNativeSeparators(path);
        const auto handle = ::CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(handle != INVALID_HANDLE_VALUE);
        const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
        if (fd < 0) ::CloseHandle(handle);
        QVERIFY(fd >= 0);
        if (!opened.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            ::_close(fd); QFAIL("Cannot attach the owned shared file descriptor");
        }
#else
        opened.setFileName(path); QVERIFY(opened.open(QIODevice::ReadOnly));
#endif
        QVERIFY(DownloadCache::Detail::sameFile(opened, path));
        const auto modified = opened.fileTime(QFileDevice::FileModificationTime);
        QCOMPARE(opened.readAll(), QByteArray("opened-A"));
        QSaveFile replacement(path); replacement.setDirectWriteFallback(false);
        QVERIFY(replacement.open(QIODevice::WriteOnly)); QCOMPARE(replacement.write("edited-B"), 8);
        QVERIFY(replacement.commit());
        QFile current(path); QVERIFY(current.open(QIODevice::ReadWrite));
        QVERIFY(current.setFileTime(modified, QFileDevice::FileModificationTime)); current.close();
        QCOMPARE(opened.size(), QFileInfo(path).size());
        QCOMPARE(opened.fileTime(QFileDevice::FileModificationTime), modified);
        QCOMPARE(read(path), QByteArray("edited-B"));
        QVERIFY(!DownloadCache::Detail::sameFile(opened, path));
        QVERIFY(opened.seek(0)); QCOMPARE(opened.readAll(), QByteArray("opened-A"));
        QVERIFY(current.open(QIODevice::ReadOnly)); QVERIFY(DownloadCache::Detail::sameFile(current, path));
    }
    void sourceLookupPersistsAndRenewsOpeningGrace() {
        QTemporaryDir dir;
        const DownloadCache::Source source{"remote", "/folder/report.pdf"};
        const auto saved = copy(dir.path(), source.path, "cached snapshot", 1000, source);
        QVERIFY2(saved.error.isEmpty(), qPrintable(saved.error));
        // Every lookup reopens the persistent ledger; no in-memory file map.
        const auto found = DownloadCache::lookup(dir.path(), source, 100000);
        QVERIFY2(found.error.isEmpty(), qPrintable(found.error)); QCOMPARE(found.path, saved.path);
        QCOMPARE(DownloadCache::lookup(dir.path(), source, 100001).path, saved.path);
        QCOMPARE(DownloadCache::trim(dir.path(), 1, 100002).files, 1);
        QCOMPARE(read(saved.path), QByteArray("cached snapshot"));
        QCOMPARE(DownloadCache::trim(dir.path(), 1, 160002).files, 0);
        QVERIFY(DownloadCache::lookup(dir.path(), source, 160003).path.isEmpty());
    }
    void exactSourcesNeverAliasOrAdoptLegacyNames() {
        QTemporaryDir dir;
        const QList<DownloadCache::Source> sources{{"one", "/a/same.txt"}, {"two", "/a/same.txt"},
            {"one", "/b/same.txt"}, {"one|/a", "/same.txt"}, {"one", "/a|/same.txt"}};
        QSet<QString> paths;
        for (qsizetype i = 0; i < sources.size(); ++i) {
            const auto data = QByteArray::number(qlonglong(i));
            const auto saved = copy(dir.path(), sources[i].path, data, 1000, sources[i]);
            QVERIFY2(saved.error.isEmpty(), qPrintable(saved.error)); paths.insert(saved.path);
            QCOMPARE(DownloadCache::lookup(dir.path(), sources[i], 2000).path, saved.path);
        }
        QCOMPARE(paths.size(), sources.size());
        for (qsizetype i = 0; i < sources.size(); ++i) QCOMPARE(read(DownloadCache::lookup(dir.path(), sources[i], 3000).path), QByteArray::number(qlonglong(i)));
        const auto legacy = copy(dir.path(), "legacy.txt", "old v1 entry", 1000);
        QVERIFY(legacy.error.isEmpty());
        const auto missing = DownloadCache::lookup(dir.path(), {"one", "/legacy.txt"}, 3000);
        QVERIFY(missing.error.isEmpty()); QVERIFY(missing.path.isEmpty()); QCOMPARE(read(legacy.path), QByteArray("old v1 entry"));
        const auto partial = dir.filePath(".zerus-part-unattributed"); QVERIFY(write(partial, "partial"));
        QVERIFY(DownloadCache::lookup(dir.path(), {"one", "/.zerus-part-unattributed"}).path.isEmpty());
        QCOMPARE(read(partial), QByteArray("partial"));
    }
    void changedCurrentCopyNeverFallsBackToAnOlderSnapshot() {
        QTemporaryDir dir; const DownloadCache::Source source{"remote", "/same.txt"};
        const auto first = copy(dir.path(), source.path, "first", 1000, source);
        const auto latest = copy(dir.path(), source.path, "second", 2000, source);
        QVERIFY(first.error.isEmpty()); QVERIFY(latest.error.isEmpty()); QVERIFY(first.path != latest.path);
        QCOMPARE(read(first.path), QByteArray("first"));
        QCOMPARE(DownloadCache::lookup(dir.path(), source, 3000).path, latest.path);
        QFile file(latest.path); QVERIFY(file.open(QIODevice::ReadWrite)); const auto stamp = file.fileTime(QFileDevice::FileModificationTime);
        QCOMPARE(file.write("edited"), 6); QVERIFY(file.setFileTime(stamp, QFileDevice::FileModificationTime)); file.close();
        const auto edited = DownloadCache::lookup(dir.path(), source, 4000);
        QVERIFY(edited.error.isEmpty()); QVERIFY(edited.path.isEmpty());
        QVERIFY(DownloadCache::lookup(dir.path(), source, 5000).path.isEmpty());
        QCOMPARE(read(first.path), QByteArray("first")); QCOMPARE(read(latest.path), QByteArray("edited"));
        const auto fresh = copy(dir.path(), source.path, "fresh", 6000, source); QVERIFY(fresh.error.isEmpty());
        QVERIFY(QFile::remove(fresh.path));
        QVERIFY(DownloadCache::lookup(dir.path(), source, 7000).path.isEmpty());
        QVERIFY(DownloadCache::lookup(dir.path(), source, 8000).path.isEmpty());
        QCOMPARE(read(first.path), QByteArray("first")); QCOMPARE(read(latest.path), QByteArray("edited"));
    }
    void malformedSourceAttributionRefusesWithoutChangingFilesOrIndex() {
        QTemporaryDir dir; const DownloadCache::Source source{"remote", "/safe.txt"};
        const auto saved = copy(dir.path(), source.path, "keep", 1000, source); QVERIFY(saved.error.isEmpty());
        const auto index = dir.filePath(".zerus-files.json");
        auto document = QJsonDocument::fromJson(read(index)).object(); auto entries = document.value("files").toArray();
        auto entry = entries[0].toObject(); entry["source_sha256"] = QString(64, '0'); entries[0] = entry; document["files"] = entries;
        const auto malformed = QJsonDocument(document).toJson(QJsonDocument::Compact); QVERIFY(write(index, malformed));
        const auto found = DownloadCache::lookup(dir.path(), source, 2000);
        QVERIFY(!found.error.isEmpty()); QVERIFY(found.path.isEmpty());
        QVERIFY(!DownloadCache::prepare(dir.path()).error.isEmpty());
        QCOMPARE(read(index), malformed); QCOMPARE(read(saved.path), QByteArray("keep"));
    }
#ifdef Q_OS_WIN
    void cachedShortcutUsesItsOwnBytesWithAMissingTarget() {
        QTemporaryDir dir; const auto root = dir.filePath("cache");
        const auto target = dir.filePath("target.txt"), shortcut = dir.filePath("fixture.lnk");
        QVERIFY(write(target, "the target is not the downloaded shortcut"));
        // QFile::link creates a real Windows .lnk, as in the existing
        // downloadsRequireExactSavedFileReceipts fixture. QFile reads its own bytes.
        QVERIFY(QFile::link(target, shortcut)); QVERIFY(QFileInfo(shortcut).isShortcut());
        const auto shortcutBytes = read(shortcut); QVERIFY(!shortcutBytes.isEmpty());
        const DownloadCache::Source source{"remote", "/remote/fixture.lnk"};
        const auto saved = copy(root, source.path, shortcutBytes, 1000, source);
        QVERIFY2(saved.error.isEmpty(), qPrintable(saved.error)); QVERIFY(QFileInfo(saved.path).isShortcut());
        QVERIFY(QFile::remove(target));
        const auto cached = DownloadCache::lookup(root, source, 2000);
        QVERIFY2(cached.error.isEmpty(), qPrintable(cached.error)); QCOMPARE(cached.path, saved.path);
        QCOMPARE(read(cached.path), shortcutBytes); QCOMPARE(DownloadCache::usage(root).files, 1);
        QCOMPARE(DownloadCache::lookup(root, source, 3000).path, saved.path);
    }
#endif
#ifndef Q_OS_WIN
    void replacedLinkNeverBecomesACachedSource() {
        QTemporaryDir dir; const DownloadCache::Source source{"remote", "/copy.txt"};
        const auto saved = copy(dir.path(), source.path, "content", 1000, source); QVERIFY(saved.error.isEmpty());
        const auto target = dir.filePath("personal.txt"); QVERIFY(write(target, "content"));
        QVERIFY(QFile::remove(saved.path)); QVERIFY(QFile::link(target, saved.path));
        const auto found = DownloadCache::lookup(dir.path(), source, 2000);
        QVERIFY(found.error.isEmpty()); QVERIFY(found.path.isEmpty());
        QVERIFY(QFileInfo(saved.path).isSymbolicLink()); QCOMPARE(read(target), QByteArray("content"));
        const auto alias = dir.filePath("linked-cache"); QVERIFY(QFile::link(dir.path(), alias));
        QVERIFY(!DownloadCache::lookup(alias, source).error.isEmpty());
    }
    void linksNeverBecomeDeletionOrOverwriteTargets() {
        QTemporaryDir dir; const auto root = dir.filePath("cache"); QVERIFY(QDir().mkdir(root));
        const auto user = dir.filePath("user.txt"); QVERIFY(write(user, "keep"));
        QVERIFY(QFile::link(user, QDir(root).filePath("report.txt")));
        const auto result = copy(root, "report.txt", "new", 1000);
        QVERIFY(result.error.isEmpty()); QCOMPARE(QFileInfo(result.path).fileName(), QString("report (1).txt"));
        QVERIFY(QFile::remove(result.path)); QVERIFY(QFile::link(user, result.path));
        QVERIFY(DownloadCache::trim(root, 1, 100000).error.isEmpty()); QCOMPARE(read(user), QByteArray("keep"));
        const auto linked = dir.filePath("linked cache"); QVERIFY(QFile::link(root, linked));
        QVERIFY(!DownloadCache::prepare(linked).error.isEmpty());
    }
#endif
};
QTEST_GUILESS_MAIN(TestDownloadCache)
#include "test_downloadcache.moc"
