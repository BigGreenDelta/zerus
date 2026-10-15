#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <future>
#include "DownloadCache.h"

namespace {
bool write(const QString &path, const QByteArray &data) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
DownloadCache::Result copy(const QString &root, const QString &name, const QByteArray &data, qint64 saved) {
    auto prepared = DownloadCache::prepare(root);
    if (!prepared.error.isEmpty()) return {{}, prepared.error, {}};
    if (!write(prepared.pending->path, data)) return {{}, "fixture write failed", {}};
    return DownloadCache::publish(prepared.pending, name, saved);
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
#ifndef Q_OS_WIN
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
