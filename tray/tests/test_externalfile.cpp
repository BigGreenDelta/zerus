#include <QtTest>
#include <QDesktopServices>
#include <QTemporaryDir>
#include "ExternalFile.h"

namespace {
QString write(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const auto path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return {};
    return path;
}

QByteArray marker(const QString &path) { return ExternalFile::downloadMarker(path); }
}

class TestExternalFile : public QObject {
    Q_OBJECT
    QList<QUrl> m_opened;
    QStringList m_revealed;
    bool m_revealResult = true;
private slots:
    void capture(const QUrl &url) { m_opened << url; }
    void init()
    {
        m_opened.clear(); m_revealed.clear(); m_revealResult = true;
        QDesktopServices::setUrlHandler("file", this, "capture");
        ExternalFile::setRevealHandler([this](const QString &path) { m_revealed << path; return m_revealResult; });
    }
    void cleanup() { QDesktopServices::unsetUrlHandler("file"); ExternalFile::setRevealHandler({}); }

    void classifiesProgramsByNameAndContent()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        // Names that run programs or scripts somewhere, including Windows' ignored
        // trailing dots and spaces, and content that has no telling name.
        for (const auto &name : {"setup.exe", "Setup.EXE.", "run.bat ", "notes.lnk", "deploy.ps1", "launch.desktop",
                                 "tool.jar", "build.sh", "Install.command", "disk.dmg", "page.url", "script.vbs"})
            QVERIFY2(ExternalFile::runsPrograms(write(dir, name, "content")), name);
        QVERIFY(ExternalFile::runsPrograms(write(dir, "tool", "#!/bin/sh\necho ready\n")));
        QVERIFY(ExternalFile::runsPrograms(write(dir, "program", QByteArray("\x7f" "ELF\x02\x01\x01\0\0\0\0\0\0\0\0\0\x02\0\x3e\0", 20))));
        QByteArray image(256, '\0'); image[0] = 'M'; image[1] = 'Z'; image[60] = char(0x80); image.replace(0x80, 4, QByteArray("PE\0\0", 4));
        QVERIFY(ExternalFile::runsPrograms(write(dir, "windows", image)));
#ifndef Q_OS_WIN
        // Program content counts behind a document name; an execute bit counts only
        // without a type of its own, as launchers open a +x document normally.
        QVERIFY(ExternalFile::runsPrograms(write(dir, "readme.txt", QByteArray("\x7f" "ELF\x02\x01\x01\0\0\0\0\0\0\0\0\0\x02\0\x3e\0", 20))));
        for (const auto &name : {"notes", "report.pdf"}) {
            const auto marked = write(dir, name, "plain words");
            QVERIFY(!ExternalFile::runsPrograms(marked));
            QVERIFY(QFile::setPermissions(marked, QFile::permissions(marked) | QFileDevice::ExeOwner));
            QCOMPARE(ExternalFile::runsPrograms(marked), QString(name) == "notes"); QVERIFY(QFile::remove(marked));
        }
        // A link runs what it points to, whatever its own name.
        QVERIFY(QFile::link(write(dir, "install.jar", "content"), dir.filePath("Summary.pdf")));
        QVERIFY(ExternalFile::runsPrograms(dir.filePath("Summary.pdf")));
        // Only an interpreter path makes a shebang, and MZ needs a PE header.
        QVERIFY(!ExternalFile::runsPrograms(write(dir, "lib.rs", "#![no_std]\n")));
        QVERIFY(!ExternalFile::runsPrograms(write(dir, "codes.csv", "MZ,Mozambique\nMW,Malawi\n")));
        QByteArray pe(256, '\0'); pe[0] = 'M'; pe[1] = 'Z'; pe[60] = char(0x80); pe.replace(0x80, 4, QByteArray("PE\0\0", 4));
        QVERIFY(ExternalFile::runsPrograms(write(dir, "picture.png", pe)));
#else
        // ShellExecute may resolve a name without an extension to a program.
        QVERIFY(ExternalFile::runsPrograms(write(dir, "README", "plain words")));
#endif
        // Documents and media keep opening with their applications; a cached
        // shortcut copy (.lnk_) is plain data.
        for (const auto &name : {"report.pdf", "notes.txt", "photo.png", "slides.pptx", "data.csv", "shortcut.lnk_",
                                 "data.json", "notebook.ipynb", "module.mjs", "page.html", "drawing.svg", "README.md"})
            QVERIFY2(!ExternalFile::runsPrograms(write(dir, name, "plain words")), name);
        // Folders open, even with names like example.com; only a macOS bundle runs,
        // also when reached through a trailing slash or dot.
        QVERIFY(QDir(dir.path()).mkpath("Tool.app/Contents"));
        for (const auto &folder : {"project", "example.com", "three.js"}) {
            QVERIFY(QDir(dir.path()).mkpath(folder)); QVERIFY2(!ExternalFile::runsPrograms(dir.filePath(folder)), folder);
        }
#ifdef Q_OS_MACOS
        for (const auto &path : {dir.filePath("Tool.app"), dir.filePath("Tool.app/"), dir.filePath("Tool.app/."),
                                 dir.filePath("Tool.app/Contents/..")})
            QVERIFY2(ExternalFile::runsPrograms(path), qPrintable(path));
#else
        QVERIFY(!ExternalFile::runsPrograms(dir.filePath("Tool.app/")));
#endif
    }

    void opensDocumentsAndRevealsPrograms()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto document = write(dir, "report.txt", "plain words"), program = write(dir, "setup.exe", "content");
        QCOMPARE(ExternalFile::open(document, ExternalFile::Origin::ThisComputer), ExternalFile::Outcome::Opened);
        QCOMPARE(m_opened, QList<QUrl>{QUrl::fromLocalFile(document)}); QVERIFY(m_revealed.isEmpty());
        QCOMPARE(ExternalFile::open(program, ExternalFile::Origin::OtherMachine), ExternalFile::Outcome::Revealed);
        QCOMPARE(m_revealed, QStringList{program}); QCOMPARE(m_opened.size(), 1);
        QVERIFY(ExternalFile::revealedNotice(program).contains("setup.exe"));
        // A custom opener and failures report themselves.
        QList<QUrl> custom;
        QCOMPARE(ExternalFile::open(document, ExternalFile::Origin::ThisComputer, [&](const QUrl &url) { custom << url; return false; }),
                 ExternalFile::Outcome::OpenFailed);
        QCOMPARE(custom, QList<QUrl>{QUrl::fromLocalFile(document)}); QCOMPARE(m_opened.size(), 1);
        m_revealResult = false;
        QCOMPARE(ExternalFile::open(program, ExternalFile::Origin::ThisComputer), ExternalFile::Outcome::RevealFailed);
        QVERIFY(ExternalFile::revealFailedNotice(program).contains(QDir::toNativeSeparators(program)));
    }

    void marksFilesFromOtherMachines()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto local = write(dir, "local.txt", "plain words"), remote = write(dir, "remote.txt", "plain words");
        const auto program = write(dir, "remote.exe", "content");
        QCOMPARE(ExternalFile::open(local, ExternalFile::Origin::ThisComputer), ExternalFile::Outcome::Opened);
        QCOMPARE(ExternalFile::open(remote, ExternalFile::Origin::OtherMachine), ExternalFile::Outcome::Opened);
        QCOMPARE(ExternalFile::open(program, ExternalFile::Origin::OtherMachine), ExternalFile::Outcome::Revealed);
        QVERIFY(marker(local).isEmpty());
        QVERIFY(!ExternalFile::markDownloaded(dir.filePath("missing.txt")));
#if defined(Q_OS_WIN)
        // Mark of the Web for the Internet zone, so SmartScreen and Office warn; a
        // repeated mark replaces the stream without leaving an old tail.
        for (const auto &path : {remote, program}) {
            QCOMPARE(marker(path), QByteArray("[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=about:internet\r\n"));
            QVERIFY(ExternalFile::markDownloaded(path)); QCOMPARE(marker(path), QByteArray("[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=about:internet\r\n"));
        }
#elif defined(Q_OS_MACOS)
        // Gatekeeper reads the quarantine; a later mark keeps it, with any user approval.
        for (const auto &path : {remote, program}) {
            const auto value = marker(path); QVERIFY2(!value.isEmpty(), qPrintable(path));
            QVERIFY(ExternalFile::markDownloaded(path)); QCOMPARE(marker(path), value);
        }
#else
        // Linux has no downloaded-file marker; programs are revealed instead.
        QVERIFY(!ExternalFile::markDownloaded(remote)); QVERIFY(marker(remote).isEmpty());
#endif
    }
};

QTEST_MAIN(TestExternalFile)
#include "test_externalfile.moc"
