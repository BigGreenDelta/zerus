#include "AppConfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

namespace {
QString g_configPathOverride;  // пусто -- override не установлен, см. setConfigPathOverrideForTests
}

QString AppConfig::configPath()
{
    if (!g_configPathOverride.isEmpty())
        return g_configPathOverride;
    // Тот же каталог, что у самого hgs (~/.config/hgs), не XDG_CONFIG_HOME и не
    // QStandardPaths — hgs сам не смотрит ни на то, ни на другое (см. hgs:
    // HGS_CONFIG_DIR="$HOME/.config/hgs"), так что путь трея должен совпасть буквально.
    // On Windows QDir::homePath() is the profile directory and ignores HOME, while
    // hgs.exe prefers HOME; elsewhere both rules are the same.
    const QString home = qEnvironmentVariable("HOME");
    return (home.isEmpty() ? QDir::homePath() : QDir::fromNativeSeparators(home))
           + QStringLiteral("/.config/hgs/tray.conf");
}

QStringList AppConfig::hgsInstallDirs(const QString &home)
{
    // Same order in which hgs extends PATH for its children (src/platform/unix.rs).
    return {home + QStringLiteral("/.local/bin"), QStringLiteral("/opt/homebrew/bin"),
            QStringLiteral("/usr/local/bin")};
}

QString AppConfig::fallbackHgsPath(const QString &pathEnv, const QStringList &installDirs)
{
    const QStringList path = pathEnv.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (!path.isEmpty() && !QStandardPaths::findExecutable(QStringLiteral("hgs"), path).isEmpty())
        return {};
    return QStandardPaths::findExecutable(QStringLiteral("hgs"), installDirs);
}

void AppConfig::setConfigPathOverrideForTests(const QString &path)
{
    g_configPathOverride = path;
}

QString AppConfig::resolveHgsPath(const QString &configured, const QString &applicationDir)
{
    if (!configured.isEmpty()) {
        if (configured.contains(QLatin1Char('/')) || configured.contains(QLatin1Char('\\'))
            || QFileInfo(configured).isAbsolute())
            return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(configured).absoluteFilePath()));
        const QString found = QStandardPaths::findExecutable(configured);
        return found.isEmpty() ? configured : found;
    }
    const QFileInfo bundled(QDir(applicationDir).filePath(QStringLiteral("hgs.exe")));
    if (bundled.isFile())
        return bundled.absoluteFilePath();
    const QString found = QStandardPaths::findExecutable(QStringLiteral("hgs"));
    return found.isEmpty() ? QStringLiteral("hgs") : found;
}

AppConfig::Platform AppConfig::currentPlatform()
{
    // Единственный #ifdef во всём вопросе о режимах открытия -- см. AppConfig.h.
    // Условие именно на мак, а не на Linux: Linux здесь заодно значит "любой другой
    // юникс", где собирается тот же konsole-совместимый набор.
#ifdef Q_OS_MACOS
    return Platform::Mac;
#elif defined(Q_OS_WIN)
    return Platform::Windows;
#else
    return Platform::Linux;
#endif
}

QStringList AppConfig::openModes(Platform p)
{
    // Списки разные, но пересекаются ровно в одном пункте -- "clipboard": буфер обмена
    // есть на обеих платформах, в отличие от Konsole и Terminal.app. Всё остальное --
    // ответ на вопрос "каким терминалом открывать", и такой терминал у каждой платформы
    // свой. На маке терминальный режим ровно один: "tabby" отсюда удалён 2026-08-28,
    // потому что команды `tabby` на маке взять негде (что именно проверялось -- врезка
    // в MacBackend.h). Список из одного терминала -- не вырождение, а честный ответ:
    // подменю «Open in» показывает Terminal.app и буфер обмена, и оба работают.
    if (p == Platform::Mac)
        return {QStringLiteral("terminal"), QStringLiteral("clipboard")};
    // Windows: auto|tab -- a Windows Terminal tab, window -- a new Windows Terminal
    // window, console -- a classic console window (also the fallback without wt.exe).
    if (p == Platform::Windows)
        return {QStringLiteral("auto"), QStringLiteral("tab"), QStringLiteral("window"),
                QStringLiteral("console"), QStringLiteral("clipboard")};
    return {QStringLiteral("auto"), QStringLiteral("tab"), QStringLiteral("window"),
            QStringLiteral("clipboard")};
}

QString AppConfig::defaultOpenMode(Platform p)
{
    // Дефолт -- первый элемент списка, а не отдельная константа: два места, в которых
    // написано "auto"/"terminal", разъехались бы при первой же правке списка.
    return openModes(p).constFirst();
}

bool AppConfig::isValidOpenMode(const QString &v, Platform p)
{
    return openModes(p).contains(v);
}

namespace {

// Windows looks next to hgs-tray.exe first (resolveHgsPath); the Unix fallback
// directories do not exist there.
QString hgsHelp(AppConfig::Platform p)
{
    return p == AppConfig::Platform::Windows
        ? QStringLiteral("  --hgs <path>          path to hgs.exe; found next to hgs-tray.exe by\n"
                         "                        default, then in PATH\n")
        : QStringLiteral("  --hgs <path>          path to the hgs binary; found in PATH by default,\n"
                         "                        then in ~/.local/bin, /opt/homebrew/bin and\n"
                         "                        /usr/local/bin\n");
}

// Описание --open-mode для usage(): режимы у платформ разные, и печатать пользователю
// про Konsole на маке (или про Terminal.app на Linux) значит объяснять ему то, чего у
// него нет. Общий у обоих текстов только хвост -- clipboard и правила "флаг против меню".
QString openModeHelp(AppConfig::Platform p)
{
    const QString head =
        p == AppConfig::Platform::Windows
            ? QStringLiteral(
                  "  --open-mode <mode>    what a click on a session does:\n"
                  "                        auto|tab|window|console|clipboard [auto].\n"
                  "                        auto -- a Windows Terminal tab, or a console\n"
                  "                        window when Windows Terminal (wt.exe) is not\n"
                  "                        installed. tab -- the same, but the fallback is\n"
                  "                        reported. window -- a new Windows Terminal\n"
                  "                        window. console -- a classic console window that\n"
                  "                        stays open when hgs reports an error.\n")
        : p == AppConfig::Platform::Mac
            ? QStringLiteral(
                  "  --open-mode <mode>    what a click on a session does:\n"
                  "                        terminal|clipboard [terminal]. terminal --\n"
                  "                        Terminal.app via osascript: present on every Mac\n"
                  "                        and needs nothing installed, but it ALWAYS opens\n"
                  "                        a new window -- a tab in an already open window\n"
                  "                        cannot be created from plain AppleScript.\n")
            : QStringLiteral(
                  "  --open-mode <mode>    what a click on a session does:\n"
                  "                        auto|tab|window|clipboard [auto]. auto -- try a\n"
                  "                        Konsole tab, falling back to a window silently if\n"
                  "                        Konsole does not expose its D-Bus API (as now).\n"
                  "                        tab -- tab only; if the API is off, the tray still\n"
                  "                        opens a window (the click is not lost) but shows\n"
                  "                        a notification saying what to enable and where.\n"
                  "                        window -- always a new window, D-Bus is not\n"
                  "                        touched at all.\n");
    return head
           + QStringLiteral(
               "                        clipboard -- open nothing, put the attach command\n"
               "                        on the clipboard and say so with a notification;\n"
               "                        you paste it yourself into the tab you want.\n"
               "                        Every mode except clipboard is about WHICH terminal\n"
               "                        opens the session, and they are ignored when\n"
               "                        --terminal/terminal= is set; clipboard always works,\n"
               "                        on any platform.\n"
               "                        The set of modes is platform-dependent (Linux --\n"
               "                        auto|tab|window, macOS -- terminal): a value from\n"
               "                        the other platform in tray.conf does not break the\n"
               "                        tray -- a warning on stderr and the default mode.\n"
               "                        The flag beats the file AT STARTUP; the same mode is\n"
               "                        in the tray menu (\"Open in\"), and picking it there\n"
               "                        is a later explicit command: it applies at once,\n"
               "                        overriding the flag for the rest of this run, and is\n"
               "                        written to tray.conf (the next start with the same\n"
               "                        flag takes the flag again)\n");
}

} // namespace

QString AppConfig::usage()
{
    const AppConfig d;  // значения по умолчанию для текста — не дублировать их числами
    return QStringLiteral(
               "hgs-tray — system tray for hgs sessions (Claude Code, Codex, Kimi) fleet-wide\n"
               "\n"
               "Usage: hgs-tray [options]\n"
               "\n"
               "%4"
               "  --local-poll <ms>     how often this box is polled  [default %1]\n"
               "  --peer-poll <ms>      how often peers are polled    [default %2]\n"
               "  --terminal <tmpl>     terminal launch template, {cmd} becomes the command\n"
               "  --icon-color <color>  color of the tray mark and badge (e.g. \"#ff0000\"); taken\n"
               "                        from the application palette by default. A manual\n"
               "                        override for when the palette disagrees with the\n"
               "                        background the icon is drawn on: on Linux the panel\n"
               "                        does not report its background over the StatusNotifier\n"
               "                        protocol, and a light application on a dark panel (or\n"
               "                        the other way round) makes the auto-pick miss. On a\n"
               "                        Mac the menu bar and the application palette follow\n"
               "                        one system theme -- there this flag is a matter of\n"
               "                        taste\n"
               "%3"
               "  --sessions            open the session dashboard\n"
               "  --selftest            headless check: hgs is found and answers with valid\n"
               "                        JSON, then exit (no tray, no display)\n"
               "  -h, --help            this text\n"
               "  --version             version and exit\n"
               "\n"
               "Values not given on the command line are read from\n"
               "~/.config/hgs/tray.conf (format \"key=value\": hgs, local-poll,\n"
               "peer-poll, terminal, icon-color, open-mode). The command line always\n"
               "beats the file.\n")
        .arg(d.localPollMs)
        .arg(d.peerPollMs)
        .arg(openModeHelp(currentPlatform()))
        .arg(hgsHelp(currentPlatform()));
}

namespace {

// Вторая платформа -- нужна ровно для того, чтобы отличить "значение с другой машины"
// от опечатки: сообщение в этих двух случаях разное, а обходятся они одинаково.
// The first other platform on which the value is a mode; false when none is.
bool foreignPlatform(const QString &value, AppConfig::Platform p, AppConfig::Platform *found)
{
    for (const auto other : {AppConfig::Platform::Linux, AppConfig::Platform::Mac,
                             AppConfig::Platform::Windows}) {
        if (other != p && AppConfig::isValidOpenMode(value, other)) {
            *found = other;
            return true;
        }
    }
    return false;
}

QString platformName(AppConfig::Platform p)
{
    if (p == AppConfig::Platform::Windows)
        return QStringLiteral("Windows");
    return p == AppConfig::Platform::Mac ? QStringLiteral("macOS") : QStringLiteral("Linux");
}

// Читает ~/.config/hgs/tray.conf в те поля out, что ещё НЕ заданы командной строкой —
// командная строка всегда побеждает файл.
//
// Формат — тот же "ключ=значение", что у ~/.config/hgs/projects: пустая строка и строка,
// начинающаяся с '#', это разметка файла, а не данные, и пропускаются молча.
//
// В отличие от projects (там имя проекта — произвольная строка, любой ключ валиден),
// здесь фиксированный список из пяти ключей, так что строка без '=', пустой ключ,
// неизвестный ключ или нечисловое значение local-poll/peer-poll почти наверняка опечатка
// пользователя, а не будущая опция — предупреждаем в stderr и пропускаем строку. Файл в
// целом никогда не является фатальной ошибкой: плохой конфиг не должен мешать трею
// запуститься, поэтому load() у этой функции нет возврата ошибки.
void applyConfigFile(AppConfig *out, bool haveHgs, bool haveLocalPoll, bool havePeerPoll,
                      bool haveTerminal, bool haveIconColor, bool haveOpenMode, QTextStream &errOut)
{
    const QString path = AppConfig::configPath();
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;  // нет файла или не читается — не ошибка, работаем на дефолтах/CLI

    QTextStream in(&f);
    int lineNo = 0;
    while (!in.atEnd()) {
        const QString raw = in.readLine();
        ++lineNo;
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0) {
            errOut << QStringLiteral("hgs-tray: %1:%2: line without '=', skipped: %3\n")
                          .arg(path).arg(lineNo).arg(line);
            continue;
        }
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        if (key.isEmpty()) {
            errOut << QStringLiteral("hgs-tray: %1:%2: empty key, line skipped\n")
                          .arg(path).arg(lineNo);
            continue;
        }

        if (key == QLatin1String("hgs")) {
            if (!haveHgs)
                out->hgsPath = value;
        } else if (key == QLatin1String("local-poll")) {
            if (!haveLocalPoll) {
                bool ok = false;
                const int ms = value.toInt(&ok);
                if (ok && ms > 0)
                    out->localPollMs = ms;
                else
                    errOut << QStringLiteral("hgs-tray: %1:%2: bad local-poll value: '%3', "
                                              "keeping the default\n")
                                  .arg(path).arg(lineNo).arg(value);
            }
        } else if (key == QLatin1String("peer-poll")) {
            if (!havePeerPoll) {
                bool ok = false;
                const int ms = value.toInt(&ok);
                if (ok && ms > 0)
                    out->peerPollMs = ms;
                else
                    errOut << QStringLiteral("hgs-tray: %1:%2: bad peer-poll value: '%3', "
                                              "keeping the default\n")
                                  .arg(path).arg(lineNo).arg(value);
            }
        } else if (key == QLatin1String("terminal")) {
            if (!haveTerminal)
                out->terminalTemplate = value;
        } else if (key == QLatin1String("icon-color")) {
            if (!haveIconColor)
                out->iconColor = value;
        } else if (key == QLatin1String("open-mode")) {
            if (!haveOpenMode) {
                const AppConfig::Platform p = AppConfig::currentPlatform();
                const QString known = AppConfig::openModes(p).join(QLatin1Char('|'));
                AppConfig::Platform other = p;
                if (AppConfig::isValidOpenMode(value, p)) {
                    out->openMode = value;
                } else if (foreignPlatform(value, p, &other)) {
                    // tray.conf, приехавший с другой машины (общий dotfiles-репозиторий,
                    // rsync домашнего каталога, перенос настроек руками), -- не опечатка
                    // и тем более не повод трею не запуститься: режим просто не
                    // существует на этой платформе. Откатываемся к дефолту платформы --
                    // тем же правилом, каким разбирается любое незнакомое значение из
                    // файла (см. ветку ниже), -- и говорим, почему.
                    out->openMode = AppConfig::defaultOpenMode(p);
                    errOut << QStringLiteral("hgs-tray: %1:%2: open-mode='%3' -- that is a %4 "
                                              "mode, and this tray is built for %5; took the "
                                              "default mode '%6' (here it can be: %7)\n")
                                  .arg(path).arg(lineNo)
                                  .arg(value, platformName(other), platformName(p),
                                       AppConfig::defaultOpenMode(p), known);
                } else {
                    out->openMode = AppConfig::defaultOpenMode(p);
                    errOut << QStringLiteral("hgs-tray: %1:%2: bad open-mode value: '%3' "
                                              "(expected one of: %4), took the default mode '%5'\n")
                                  .arg(path).arg(lineNo)
                                  .arg(value, known, AppConfig::defaultOpenMode(p));
                }
            }
        } else {
            errOut << QStringLiteral("hgs-tray: %1:%2: unknown key '%3', line skipped\n")
                          .arg(path).arg(lineNo).arg(key);
        }
    }
}

} // namespace

QStringList AppConfig::restartArguments(const QStringList &args)
{
    // The same walk as load(): a value is never mistaken for an option.
    static const QStringList withValue{QStringLiteral("--hgs"), QStringLiteral("--local-poll"),
                                       QStringLiteral("--peer-poll"), QStringLiteral("--terminal"),
                                       QStringLiteral("--icon-color"), QStringLiteral("--open-mode")};
    QStringList kept;
    for (int i = 1; i < args.size(); ++i) {
        if (withValue.contains(args.at(i))) {
            kept << args.mid(i, 2);
            ++i;
        } else if (args.at(i) != QLatin1String("--sessions")) {
            kept << args.at(i);
        }
    }
    return kept;
}

bool AppConfig::load(const QStringList &args, AppConfig *out, QString *error)
{
    bool haveHgs = false, haveLocalPoll = false, havePeerPoll = false, haveTerminal = false;
    bool haveIconColor = false, haveOpenMode = false;

    auto needValue = [&](int &i, const QString &flag) -> QString {
        if (i + 1 >= args.size()) {
            *error = QStringLiteral("%1 requires a value").arg(flag);
            return QString();
        }
        return args.at(++i);
    };

    for (int i = 1; i < args.size(); ++i) {
        const QString a = args.at(i);
        if (a == QLatin1String("--hgs")) {
            out->hgsPath = needValue(i, a);
            haveHgs = true;
        } else if (a == QLatin1String("--local-poll")) {
            const QString v = needValue(i, a);
            if (!error->isEmpty())
                return false;
            bool ok = false;
            const int ms = v.toInt(&ok);
            if (!ok || ms <= 0) {
                *error = QStringLiteral("--local-poll expects a positive integer, got '%1'").arg(v);
                return false;
            }
            out->localPollMs = ms;
            haveLocalPoll = true;
        } else if (a == QLatin1String("--peer-poll")) {
            const QString v = needValue(i, a);
            if (!error->isEmpty())
                return false;
            bool ok = false;
            const int ms = v.toInt(&ok);
            if (!ok || ms <= 0) {
                *error = QStringLiteral("--peer-poll expects a positive integer, got '%1'").arg(v);
                return false;
            }
            out->peerPollMs = ms;
            havePeerPoll = true;
        } else if (a == QLatin1String("--terminal")) {
            out->terminalTemplate = needValue(i, a);
            haveTerminal = true;
        } else if (a == QLatin1String("--icon-color")) {
            out->iconColor = needValue(i, a);
            haveIconColor = true;
        } else if (a == QLatin1String("--open-mode")) {
            const QString v = needValue(i, a);
            if (!error->isEmpty())
                return false;
            // В отличие от файла (там -- предупреждение и откат к дефолту) здесь отказ
            // фатальный: командная строка -- это то, что пользователь напечатал ПРЯМО
            // СЕЙЧАС, и молча сделать не то, что он просил, хуже, чем не запуститься.
            // Стало быть, текст обязан объяснить и что пришло, и что бывает.
            const Platform p = currentPlatform();
            if (!isValidOpenMode(v, p)) {
                *error = QStringLiteral("--open-mode expects one of: %1, got '%2'")
                             .arg(openModes(p).join(QStringLiteral(", ")), v);
                Platform other = p;
                if (foreignPlatform(v, p, &other))
                    *error += QStringLiteral(" ('%1' is a %2 mode, this build is %3)")
                                  .arg(v, platformName(other), platformName(p));
                return false;
            }
            out->openMode = v;
            haveOpenMode = true;
        } else if (a == QLatin1String("--sessions")) {
            out->showSessions = true;
        } else if (a == QLatin1String("--selftest")) {
            out->selfTest = true;
        } else if (a == QLatin1String("-h") || a == QLatin1String("--help")) {
            *error = QStringLiteral("__help__");
            return false;
        } else if (a == QLatin1String("--version")) {
            *error = QStringLiteral("__version__");
            return false;
        } else {
            *error = QStringLiteral("unknown option: %1").arg(a);
            return false;
        }
        if (!error->isEmpty())
            return false;
    }

    QTextStream errOut(stderr);
    applyConfigFile(out, haveHgs, haveLocalPoll, havePeerPoll, haveTerminal, haveIconColor,
                     haveOpenMode, errOut);

    return true;
}

namespace {

// Шапка, с которой файл создаётся, если его ещё не было. Пользователь до этого момента
// tray.conf в глаза не видел -- трей читает его молча и без него прекрасно работает, --
// так что найдя однажды непонятный файл в ~/.config/hgs, он должен из самого файла
// узнать, кто его создал, чем он является и что в него можно дописать. Голого
// "open-mode=tab" для этого мало.
QStringList configHeaderLines()
{
    return {
        QStringLiteral("# hgs-tray -- tray settings, format \"key=value\"."),
        QStringLiteral("# Written by the tray itself when a mode is picked in the tray menu"),
        QStringLiteral("# (\"Open in\"). Editing by hand is fine: the tray reads it at startup,"),
        QStringLiteral("# and when writing it keeps other keys, comments and their order."),
        QStringLiteral("# Keys: hgs, local-poll, peer-poll, terminal, icon-color, open-mode"),
        QStringLiteral("# (what each one means -- hgs-tray --help)."),
        QString(),
    };
}

} // namespace

bool AppConfig::writeConfigValue(const QString &key, const QString &value, QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error != nullptr)
            *error = msg;
        return false;
    };

    // Формат построчный и без экранирования (см. applyConfigFile) -- значит, ключ со
    // знаком '=' или переводом строки записать нечем: он прочитается не так, как
    // записан. Это ошибка вызывающего, а не пользователя, но молча портить файл нельзя.
    if (key.isEmpty() || key.contains(QLatin1Char('=')) || key.contains(QLatin1Char('\n')))
        return fail(QStringLiteral("bad key: '%1'").arg(key));
    if (value.contains(QLatin1Char('\n')))
        return fail(QStringLiteral("a value cannot contain a newline"));

    const QString path = configPath();
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir))
        return fail(QStringLiteral("cannot create directory %1").arg(dir));

    // Читаем БЕЗ QIODevice::Text: файл сохраняется дословно, включая всё, чего этот
    // формат не понимает.
    QStringList lines;
    bool existed = false;
    {
        QFile in(path);
        if (in.exists()) {
            if (!in.open(QIODevice::ReadOnly))
                return fail(QStringLiteral("%1: %2").arg(path, in.errorString()));
            existed = true;
            lines = QString::fromUtf8(in.readAll()).split(QLatin1Char('\n'));
        }
    }

    const QString newLine = key + QLatin1Char('=') + value;

    // Ищем ПОСЛЕДНЕЕ вхождение ключа, а не первое: applyConfigFile() перечитывает
    // значение на каждой встреченной строке, то есть при дубле выигрывает последний.
    // Переписав именно его, мы получаем минимальную правку, которая при этом реально
    // меняет то, что прочитается: ни одной строки не удалено, а раньше затенённый дубль
    // остаётся затенённым ровно как был.
    int target = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        if (line.left(eq).trimmed() == key)
            target = i;
    }

    const bool empty = lines.isEmpty() || (lines.size() == 1 && lines.constFirst().isEmpty());
    if (target >= 0) {
        lines[target] = newLine;
    } else if (!existed || empty) {
        lines = configHeaderLines();
        lines << newLine << QString();
    } else {
        // Ключа в файле не было -- дописываем в конец, не трогая ничего выше. Хвостовой
        // элемент split() -- это пустая строка ПОСЛЕ последнего '\n', а не отдельная
        // строка файла: снимаем её и возвращаем после своей, чтобы файл как кончался
        // переводом строки, так и кончался (а если не кончался -- начал).
        if (lines.constLast().isEmpty())
            lines.removeLast();
        lines << newLine << QString();
    }

    // QSaveFile -- это временный файл рядом + rename(), то есть замена атомарна: трей,
    // убитый (logout, systemctl stop, крах) посреди записи, не оставит обрезанный
    // tray.conf, на который следующий запуск начнёт ругаться разбором. От ОДНОВРЕМЕННОЙ
    // правки в чужом редакторе это не спасает и спасать не пытается: там гонка не в
    // самой записи, а в том, что редактор держит свою копию файла минутами -- решается
    // только блокировкой файла, которую ни один редактор тут всё равно не соблюдает.
    // Цена промаха мала и заметна сразу: единственный ключ, который сюда пишет трей, --
    // open-mode, а окно между чтением и записью здесь микросекунды.
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("%1: %2").arg(path, out.errorString()));
    const QByteArray bytes = lines.join(QLatin1Char('\n')).toUtf8();
    if (out.write(bytes) != bytes.size() || !out.commit())
        return fail(QStringLiteral("%1: %2").arg(path, out.errorString()));
    return true;
}
