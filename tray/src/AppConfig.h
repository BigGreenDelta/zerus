#pragma once

#include <QString>
#include <QStringList>

// Разбирается ДО создания QApplication: --selftest и --version обязаны работать по ssh,
// где QApplication упал бы, не найдя дисплея.
struct AppConfig {
    QString hgsPath;             // путь к hgs; пусто = искать в PATH
    int localPollMs = 2000;      // тик опроса своего бокса
    int peerPollMs = 60000;      // фоновый тик опроса пиров
    QString terminalTemplate;    // шаблон команды терминала; {cmd} подставляется
    QString iconColor;           // ручной override цвета иконки; пусто = авто по палитре
    // Провалидированная строка, не enum: смысл значения знают бэкенды
    // (KonsoleBackend::OpenMode на Linux, MacBackend на маке -- там режим один и
    // разбирать нечего, ClipboardBackend на обеих), AppConfig лишь разбирает и проверяет
    // ввод пользователя, как и остальные
    // поля здесь. НАБОР допустимых значений зависит от платформы -- см. openModes():
    // auto|tab|window на Linux, terminal на маке. Все они отвечают на вопрос
    // «каким терминалом открывать», поэтому при заданном terminal= (CommandBackend) не
    // читаются вовсе: шаблон побеждает. "clipboard" стоит особняком и есть на обеих
    // платформах -- это «не открывать терминал вообще», поэтому он сильнее и шаблона, и
    // платформы, см. makeTerminal() в TrayAgent.cpp.
    QString openMode = defaultOpenMode(currentPlatform());
    bool selfTest = false;
    bool showSessions = false;

    // Возвращает false и заполняет *error, если разбор не удался. Особые значения
    // error: "__help__" и "__version__" — не ошибки, а запрос вывести текст и выйти.
    static bool load(const QStringList &args, AppConfig *out, QString *error);
    static QString usage();

    // Платформа -- ПАРАМЕТР, а не #ifdef внутри самих проверок: тот же приём и та же
    // причина, что у ClipboardBackend::Platform (см. её .h) -- так набор режимов ОБЕИХ
    // платформ виден тесту с любой из них, а единственный #ifdef во всём вопросе живёт
    // в currentPlatform(). Свой enum, а не общий с ClipboardBackend: AppConfig ничего не
    // знает о бэкендах и собирается (и тестируется) без QtGui, а ClipboardBackend тянет
    // за собой QGuiApplication.
    // Windows reuses the Linux names auto|tab|window (Windows Terminal there) so a
    // tray.conf shared through dotfiles keeps working, and adds console.
    enum class Platform { Linux, Mac, Windows };
    static Platform currentPlatform();
    // Допустимые значения open-mode для платформы -- В ТОМ ЖЕ ПОРЯДКЕ, в каком их
    // показывает подменю «Open in»: TrayAgent строит меню отсюда, второго списка
    // режимов в проекте нет. Первый элемент -- значение по умолчанию (см.
    // defaultOpenMode). "clipboard" есть в списке каждой платформы -- единственный
    // режим, который не зависит от того, где трей запущен.
    static QStringList openModes(Platform p);
    static QString defaultOpenMode(Platform p);
    static bool isValidOpenMode(const QString &v, Platform p);

    // ~/.config/hgs/tray.conf, формат "ключ=значение". Отсутствие файла — не ошибка.
    // Home is HOME when it is set, as for hgs itself (on Windows HOME, else the
    // profile directory).
    static QString configPath();

    // Where installations put hgs, in search order: the per-user install, then
    // Homebrew on Apple silicon and Intel.
    static QStringList hgsInstallDirs(const QString &home);
    // hgs for an app started without --hgs or hgs=. Finder, Dock and Spotlight
    // start it with PATH=/usr/bin:/bin:/usr/sbin:/sbin, which has no hgs. Empty
    // keeps the PATH lookup when PATH provides hgs or nothing is installed.
    static QString fallbackHgsPath(const QString &pathEnv, const QStringList &installDirs);

    // Записывает "ключ=значение" в configPath(), СОХРАНЯЯ файл дословно: чужие ключи,
    // комментарии, пустые строки и их порядок остаются как были — переписывается ровно
    // одна строка этого ключа (или дописывается в конец, если её не было). Обратная
    // операция к applyConfigFile(), а не «сериализовать структуру»: сериализация тихо
    // потеряла бы всё, чего нет в AppConfig, — комментарии пользователя в первую очередь.
    // Файла нет — создаётся с короткой шапкой (см. .cpp): его никто никогда не видел, и
    // найдя его потом, пользователь должен понять, откуда он взялся.
    // false + *error — не удалось прочитать/записать; вызывающий решает, что сказать.
    static bool writeConfigValue(const QString &key, const QString &value, QString *error);
    // The hgs executable on Windows: a configured name or path as given (a bare
    // name looked up in PATH), otherwise hgs.exe next to hgs-tray.exe (the
    // portable zip), otherwise hgs.exe in PATH. Absolute where it was found, so the
    // tray, its terminals and copied commands all name the same file.
    static QString resolveHgsPath(const QString &configured, const QString &applicationDir);
    // The options of args (as for load(), argv[0] first) that a restarted tray
    // keeps: every setting, but not argv[0] and the one-shot --sessions. Windows
    // restarts the tray with them after a crash or a watchdog abort
    // (RegisterApplicationRestart in main.cpp).
    static QStringList restartArguments(const QStringList &args);

    // Только для тестов: подменяет путь, который вернёт configPath(), не трогая
    // реальный ~/.config/hgs/tray.conf пользователя. Пустая строка -- снять подмену.
    static void setConfigPathOverrideForTests(const QString &path);
};
