#include "ExternalFile.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include "ChildProcess.h"
#include "LocalFile.h"
#ifdef Q_OS_WIN
#include <QSettings>
#include <qt_windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thread>
#endif
#ifdef Q_OS_MACOS
// AssertMacros.h would otherwise define check() and verify() macros.
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <sys/xattr.h>
#endif
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#endif

namespace {
std::function<bool(const QString &)> &revealHandler()
{
    static std::function<bool(const QString &)> value;
    return value;
}

// Types whose default handler runs programs, scripts or installers: Chromium's
// dangerous download types for each platform, Firefox's executable extensions
// and the Windows Attachment Manager list.
const QSet<QString> &runnableSuffixes()
{
    static const QSet<QString> value{
        // Windows programs, scripts, shortcuts, installers, Office add-ins and mounted images.
        "exe", "com", "scr", "pif", "cpl", "msi", "msp", "mst", "msc", "msu", "msix", "msixbundle", "appx",
        "appxbundle", "application", "appref-ms", "appcontent-ms", "gadget", "xbap", "bat", "cmd", "ps1", "psm1",
        "psd1", "ps1xml", "ps2", "ps2xml", "psc1", "psc2", "msh", "msh1", "msh2", "mshxml", "msh1xml", "msh2xml",
        "vb", "vbs", "vbe", "vbscript", "js", "jse", "wsf", "wsh", "wsc", "ws", "sct", "hta", "htt", "lnk", "url",
        "website", "scf", "shb", "shs", "reg", "rgs", "inf", "inx", "ins", "isp", "isu", "iqy", "job", "chm", "hlp",
        "diagcab", "diagcfg", "diagpkg", "settingcontent-ms", "library-ms", "search-ms", "searchconnector-ms",
        "partial", "paf", "u3p", "spl", "cpi", "xrm-ms", "efi", "iso", "img", "vhd", "vhdx", "xll", "xla", "xlam",
        "ppa", "ppam", "dll", "ocx", "sys", "drv", "vxd", "cab", "cer", "crt", "der", "ade", "adp", "accda",
        "accdb", "accde", "accdr", "mad", "maf", "mag", "mam", "maq", "mar", "mas", "mat", "mau", "mav", "maw",
        "mda", "mdb", "mde", "mdt", "mdw", "mdz",
        // macOS applications, scripts, installers, disk images and file locations.
        "app", "command", "tool", "terminal", "pkg", "mpkg", "workflow", "wflow", "action", "caction", "definition",
        "scpt", "scptd", "applescript", "osas", "osax", "prefpane", "kext", "seplugin", "service", "fileloc",
        "inetloc", "webloc", "afploc", "atloc", "ftploc", "osx", "dylib", "xip", "mobileconfig", "configprofile",
        "internetconnect", "networkconnect", "cpgz", "pax", "dmg", "dmgpart", "cdr", "dart", "dc42", "diskcopy42",
        "dvdr", "imgpart", "ndif", "smi", "sparsebundle", "sparseimage", "toast", "udif",
        // Linux launchers, scripts, packages and images; scripts and Java run anywhere.
        "desktop", "kdelnk", "sh", "bash", "zsh", "csh", "tcsh", "ksh", "fish", "shar", "run", "bin", "out",
        "appimage", "deb", "rpm", "snap", "flatpak", "flatpakref", "flatpakrepo", "pet", "pup", "slp", "so", "apk",
        "crx", "oxt", "jar", "jnlp", "class", "py", "pyw", "pyc", "pyo", "pyd", "pyz", "pyzw", "pl", "rb", "php",
    };
    return value;
}

#ifndef Q_OS_WIN
// Programs and scripts by their first bytes: ELF, a PE image, Mach-O (also Java
// class files) and a shebang naming an interpreter path. MIME inheritance is not
// used: shared-mime-info makes JSON and most source files subtypes of
// application/x-executable.
bool programContent(const QFileInfo &info)
{
    QFile file(info.filePath());
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto head = file.read(64);
    static const QList<QByteArray> magic{QByteArray("\x7f" "ELF"), QByteArray("\xfe\xed\xfa\xce"), QByteArray("\xfe\xed\xfa\xcf"),
                                          QByteArray("\xce\xfa\xed\xfe"), QByteArray("\xcf\xfa\xed\xfe"), QByteArray("\xca\xfe\xba\xbe")};
    if (magic.contains(head.left(4))) return true;
    // "#!/bin/sh" runs; Rust's "#![no_std]" does not.
    static const QRegularExpression shebang(QStringLiteral("^#![ \t]*/"));
    if (shebang.match(QString::fromLatin1(head.left(16))).hasMatch()) return true;
    // A PE image points from offset 0x3c to its "PE\0\0" header; text may start with MZ.
    if (!head.startsWith("MZ") || head.size() < 64) return false;
    const quint32 offset = quint8(head[60]) | quint32(quint8(head[61])) << 8 | quint32(quint8(head[62])) << 16 | quint32(quint8(head[63])) << 24;
    return offset < 4096 && file.seek(offset) && file.read(4) == QByteArray("PE\0\0", 4);
}
#endif

// The name Windows resolves: it drops trailing dots and spaces from the last part.
QString resolvedName(const QString &name)
{
    auto result = name;
    while (result.endsWith(QLatin1Char('.')) || result.endsWith(QLatin1Char(' '))) result.chop(1);
    return result;
}

bool classify(const QFileInfo &info)
{
    const auto name = resolvedName(info.fileName());
    const auto dot = name.lastIndexOf(QLatin1Char('.'));
    const auto suffix = dot >= 0 ? name.mid(dot + 1).toLower() : QString();
    // Opening a folder shows it, except a macOS bundle (an .app, an installer, a plug-in).
    if (info.isDir()) {
#ifdef Q_OS_MACOS
        static const QSet<QString> packages{"app", "pkg", "mpkg", "prefpane", "kext", "workflow", "action", "scptd",
                                            "saver", "plugin", "bundle", "appex", "xpc", "osax", "qlgenerator", "mdimporter", "service"};
        return info.isBundle() || packages.contains(suffix);
#else
        return false;
#endif
    }
    if (!suffix.isEmpty() && runnableSuffixes().contains(suffix)) return true;
#ifdef Q_OS_WIN
    // ShellExecute may resolve a file name without an extension to name.exe or name.bat.
    if (suffix.isEmpty()) return true;
    return !suffix.isEmpty() && AssocIsDangerous(reinterpret_cast<LPCWSTR>((QStringLiteral(".") + suffix).utf16()));
#else
    if (!info.isFile()) return false;
    // Launchers run an executable file with no type of its own; a document type wins.
    if (suffix.isEmpty() && (info.permissions() & (QFileDevice::ExeUser | QFileDevice::ExeGroup | QFileDevice::ExeOther))) return true;
    return programContent(info);
#endif
}

QString translated(const char *text) { return QCoreApplication::translate("ExternalFile", text); }

#ifdef Q_OS_WIN
// The \\?\ form reaches paths longer than MAX_PATH; it needs a clean absolute path.
std::wstring longPath(const QString &path)
{
    const QFileInfo info(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    auto native = QDir::toNativeSeparators(QDir(info.path()).filePath(resolvedName(info.fileName())));
    if (native.startsWith(QStringLiteral("\\\\?\\"))) {}
    else if (native.startsWith(QStringLiteral("\\\\"))) native = QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    else native.prepend(QStringLiteral("\\\\?\\"));
    return native.toStdWString();
}
#endif
}

namespace ExternalFile {
bool runsPrograms(const QString &path)
{
    // Judge what the opener launches: "App.app/." and a link named Report.pdf both reach the target.
    const QFileInfo info(QDir::cleanPath(path));
    if (classify(info)) return true;
    const auto target = info.isSymLink() ? info.canonicalFilePath() : QString();
    return !target.isEmpty() && classify(QFileInfo(target));
}

bool markDownloaded(const QString &path)
{
    if (!LocalFile::readable(QFileInfo(path))) return false;
#if defined(Q_OS_WIN)
    // An administrator can ask Windows not to keep zone information.
    const QSettings policy(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Attachments"),
                           QSettings::NativeFormat);
    if (policy.value(QStringLiteral("SaveZoneInformation")).toInt() == 1) return false;
    // Mark of the Web for the Internet zone, as browsers write it.
    const auto stream = longPath(path) + L":Zone.Identifier";
    const HANDLE file = CreateFileW(stream.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    static const char zone[] = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=about:internet\r\n";
    DWORD written = 0;
    const bool ok = WriteFile(file, zone, DWORD(sizeof zone - 1), &written, nullptr) && written == DWORD(sizeof zone - 1)
        && SetEndOfFile(file);
    CloseHandle(file);
    return ok;
#elif defined(Q_OS_MACOS)
    const auto encoded = QFile::encodeName(QFileInfo(path).absoluteFilePath());
    const CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8 *>(encoded.constData()),
                                                                 encoded.size(), false);
    if (!url) return false;
    // An existing quarantine may record the user's approval; keep it.
    CFTypeRef current = nullptr;
    if (CFURLCopyResourcePropertyForKey(url, kCFURLQuarantinePropertiesKey, &current, nullptr) && current) {
        CFRelease(current); CFRelease(url); return true;
    }
    const void *keys[] = {kLSQuarantineTypeKey};
    const void *values[] = {kLSQuarantineTypeOtherDownload};
    const CFDictionaryRef properties = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks);
    const bool ok = properties && CFURLSetResourcePropertyForKey(url, kCFURLQuarantinePropertiesKey, properties, nullptr);
    if (properties) CFRelease(properties);
    CFRelease(url);
    return ok;
#else
    return false;
#endif
}

bool reveal(const QString &path)
{
    if (const auto &handler = revealHandler()) return handler(path);
    const QFileInfo info(QDir::cleanPath(path));
    if (!LocalFile::readable(info) && !info.isDir()) return false;
#if defined(Q_OS_WIN)
    // The shell call browsers use; it waits on Explorer, so it runs on its own STA thread.
    const auto file = QDir::toNativeSeparators(QDir::cleanPath(info.absoluteFilePath())).toStdWString();
    const auto folder = QDir::toNativeSeparators(QDir::cleanPath(info.absolutePath())).toStdWString();
    std::thread([file, folder] {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
        bool shown = false;
        if (PIDLIST_ABSOLUTE parent = ILCreateFromPathW(folder.c_str())) {
            if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(file.c_str())) {
                PCUITEMID_CHILD selected = ILFindLastID(item);
                shown = SUCCEEDED(SHOpenFolderAndSelectItems(parent, 1, &selected, 0));
                ILFree(item);
            }
            ILFree(parent);
        }
        if (!shown) ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (com) CoUninitialize();
    }).detach();
    return true;
#elif defined(Q_OS_MACOS)
    return ChildProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), info.absoluteFilePath()});
#elif defined(Q_OS_LINUX)
    // The file manager selects the file; without one, open its folder.
    const auto folder = QUrl::fromLocalFile(info.absolutePath());
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return QDesktopServices::openUrl(folder);
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
    message << QStringList{QString::fromUtf8(QUrl::fromLocalFile(info.absoluteFilePath()).toEncoded())} << QString();
    // The bus default timeout lets a file manager start cold without a second, fallback window.
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), QCoreApplication::instance());
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [watcher, folder] {
        if (watcher->isError()) QDesktopServices::openUrl(folder);
        watcher->deleteLater();
    });
    return true;
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
#endif
}

QByteArray downloadMarker(const QString &path)
{
#if defined(Q_OS_WIN)
    const auto stream = longPath(path) + L":Zone.Identifier";
    const HANDLE file = CreateFileW(stream.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    char buffer[1024]; DWORD read = 0;
    const bool ok = ReadFile(file, buffer, sizeof buffer, &read, nullptr);
    CloseHandle(file);
    return ok ? QByteArray(buffer, int(read)) : QByteArray();
#elif defined(Q_OS_MACOS)
    char buffer[1024];
    const auto size = getxattr(QFile::encodeName(path).constData(), "com.apple.quarantine", buffer, sizeof buffer, 0, 0);
    return size > 0 ? QByteArray(buffer, int(size)) : QByteArray();
#else
    Q_UNUSED(path);
    return {};
#endif
}

Outcome open(const QString &path, Origin origin, const std::function<bool(const QUrl &)> &opener)
{
    // Marking repeats on each open: Windows may clear it after a reputation check.
    if (origin == Origin::OtherMachine) markDownloaded(path);
    if (runsPrograms(path)) return reveal(path) ? Outcome::Revealed : Outcome::RevealFailed;
    const auto url = QUrl::fromLocalFile(path);
    return (opener ? opener(url) : QDesktopServices::openUrl(url)) ? Outcome::Opened : Outcome::OpenFailed;
}

QString revealedNotice(const QString &path)
{
    return translated("%1 can run programs, so it is shown in the file manager instead of being opened.").arg(QFileInfo(path).fileName());
}

QString revealFailedNotice(const QString &path)
{
    return translated("%1 can run programs, and the file manager could not show it. Its copy is at %2.")
        .arg(QFileInfo(path).fileName(), QDir::toNativeSeparators(path));
}

void setRevealHandler(std::function<bool(const QString &)> handler) { revealHandler() = std::move(handler); }
}
