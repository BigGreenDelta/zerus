#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include "AppConfig.h"
#include "SingleInstance.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#ifndef Q_OS_WIN
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

// --open-mode / open-mode= -- см. AppConfig.h. Сама логика "что означает значение" живёт
// в бэкендах (auto/tab/window -- KonsoleBackend, terminal -- MacBackend,
// clipboard -- ClipboardBackend), здесь проверяется только разбор: допустимые значения,
// явный отказ на недопустимых (с понятным текстом) и что командная строка бьёт файл, как
// и остальные пять ключей tray.conf.
//
// Набор значений платформозависим, и тесты написаны так, чтобы идти на обеих платформах
// БЕЗ #ifdef: списки обеих платформ -- чистые функции с параметром (openModes(Platform)),
// поэтому маковый набор проверяется и с Linux и наоборот; а тесты про разбор берут
// значения не литералами, а из списка ТЕКУЩЕЙ платформы (modeA/modeB) либо заведомо
// чужой (foreignMode). Единственное, что этим способом не проверить, -- что
// currentPlatform() не врёт про машину, на которой идёт ctest; для этого пришлось бы
// написать тот же #ifdef второй раз и сверить его с самим собой.
class TestAppConfig : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void openModeListIsPlatformDependent();
    void clipboardIsTheOnlyModeOnBothPlatforms();
    void defaultOpenModeIsFirstOfItsList();
    void openModeDefaultsToPlatformDefault();
    void openModeAcceptsAllValues();
    void openModeRejectsGarbageOnCommandLine();
    void openModeRejectsGarbageInConfigFileAndKeepsDefault();
    void foreignPlatformModeIsHardErrorOnCommandLine();
    void foreignPlatformModeInConfigFileFallsBackToPlatformDefault();
    void commandLineOpenModeBeatsConfigFile();
    void configFileOpenModeAppliesWhenCommandLineSilent();
    void configFileAcceptsClipboardMode();
    void windowsModesKeepSharedValuesAndAddConsole();
    void homeVariableLocatesTrayConfig();
    void bundledHgsIsPreferredOnWindows();
    void enabledWindowsRunsTheStableCli();
    void localPollingFollowsPlatformAndEnablement();
    void singleInstanceNameIsPerUserOnWindows();
    void serviceStateFollowsTheServiceManager();
    void onlyAnotherServiceProcessKeepsTheSocket();
    void concurrentStartsLeaveOneReachableOwner();
    void launcherForwardsOnceTheStartingServiceListens();
    void launcherClaimsWhenTheServiceStopsOrNeverListens();
    void staleSocketAndLockRecoverAfterACrash();
    void aStuckStartDelaysOthersBoundedly();
    void restartKeepsSettingsButNotOneShotOptions();

    void writeCreatesFileWithHeaderWhenAbsent();
    void writePreservesOtherKeysCommentsAndOrder();
    void writeReplacesExistingLineInPlaceWithoutAppending();
    void writeRepeatedKeepsExactlyOneLine();
    void writeWithDuplicateLinesStillReadsBackWritten();
    void writeAppendsNewlineToFileThatLackedOne();
    void writeRejectsBadKeyAndLeavesFileAlone();

    void hgsInstallDirsPreferUserInstallThenHomebrew();
    void fallbackHgsKeepsPathLookupWhenPathProvidesHgs();
    void fallbackHgsFindsFirstExecutableInstall();
    void fallbackHgsIsEmptyWithoutAnyInstall();
    void bundledHgsIsFoundNextToMacBundleExecutable();
    void bundledHgsRequiresAnExecutableFile();
    void bundledHgsRequiresAnApplicationBundle();
    void bundledHgsIsMacOnly();

private:
    static QString writeHgs(const QString &dir, bool executable);
    // Два РАЗНЫХ валидных режима текущей платформы. Тестам про "файл против командной
    // строки" и про запись не важно, какие именно, важно лишь, что значения различаются
    // и оба здесь допустимы -- а литералами этого на двух платформах не написать.
    // modeA() -- это дефолт (первый в списке), modeB() -- заведомо не дефолт, поэтому
    // ожидаемый modeB отличает "значение и правда прочиталось" от "остался дефолт".
    static QString modeA() { return AppConfig::openModes(AppConfig::currentPlatform()).at(0); }
    static QString modeB() { return AppConfig::openModes(AppConfig::currentPlatform()).at(1); }
    // Режим, который валиден на ДРУГОЙ платформе и невалиден здесь: ровно то, что лежит
    // в tray.conf, приехавшем с другой машины.
    static QString foreignMode();

    // Пишущие тесты живут каждый в СВОЁМ каталоге: m_dir один на весь класс, а
    // writeConfigValue дописывает в уже существующий файл -- без этого второй тест
    // читал бы хвост первого.
    QString freshConfPath(const QString &name);
    QStringList readLines(const QString &path);

    QTemporaryDir m_dir;
};

QString TestAppConfig::foreignMode()
{
    const AppConfig::Platform here = AppConfig::currentPlatform();
    const AppConfig::Platform there = here == AppConfig::Platform::Mac
                                          ? AppConfig::Platform::Linux
                                          : AppConfig::Platform::Mac;
    for (const QString &m : AppConfig::openModes(there))
        if (!AppConfig::isValidOpenMode(m, here))
            return m;
    return QString();   // не бывает: списки пересекаются только в clipboard
}

QString TestAppConfig::freshConfPath(const QString &name)
{
    const QString dir = m_dir.filePath(name);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/tray.conf");
}

QStringList TestAppConfig::readLines(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
}

void TestAppConfig::init()
{
    // Перед КАЖДЫМ тестом уводим configPath() на заведомо несуществующий файл внутри
    // m_dir. Без этого тесты, которые override не ставят (проверяющие ДЕФОЛТЫ и разбор
    // командной строки), читали настоящий ~/.config/hgs/tray.conf запустившего ctest --
    // и «проходили» ровно до тех пор, пока у него там лежало то же, что ожидает тест.
    // Поймано вживую: стоило трею записать туда open-mode=clipboard из меню, как
    // openModeDefaultsToAuto() покраснел, ничего не сломав в самом коде. Тесты, которым
    // файл нужен, ставят свой override поверх этого и на него не влияют.
    AppConfig::setConfigPathOverrideForTests(m_dir.filePath(QStringLiteral("no-such.conf")));
}

void TestAppConfig::cleanup()
{
    // Снимаем override после КАЖДОГО теста, а не только в конце файла -- иначе
    // упавший тест оставит configPath() указывающим на уже удалённый QTemporaryDir,
    // и все тесты после него в этом же ctest-прогоне начнут молча писать/читать не туда.
    AppConfig::setConfigPathOverrideForTests(QString());
}

void TestAppConfig::openModeListIsPlatformDependent()
{
    // Смысл того, что платформа -- параметр, а не #ifdef: маковый список виден с Linux и
    // наоборот. Порядок фиксирован не для красоты -- ровно в этом порядке трей строит
    // пункты подменю «Open in», и первый элемент является дефолтом.
    QCOMPARE(AppConfig::openModes(AppConfig::Platform::Linux),
             (QStringList{QStringLiteral("auto"), QStringLiteral("tab"),
                          QStringLiteral("window"), QStringLiteral("clipboard")}));
    QCOMPARE(AppConfig::openModes(AppConfig::Platform::Mac),
             (QStringList{QStringLiteral("terminal"), QStringLiteral("clipboard")}));
    // Проверка на "список платформы и валидатор -- одно и то же": режим своей платформы
    // валиден, чужой -- нет. Иначе меню предложило бы то, что следующий старт отвергнет.
    QVERIFY(AppConfig::isValidOpenMode(QStringLiteral("tab"), AppConfig::Platform::Linux));
    QVERIFY(!AppConfig::isValidOpenMode(QStringLiteral("tab"), AppConfig::Platform::Mac));
    QVERIFY(AppConfig::isValidOpenMode(QStringLiteral("terminal"), AppConfig::Platform::Mac));
    QVERIFY(!AppConfig::isValidOpenMode(QStringLiteral("terminal"), AppConfig::Platform::Linux));
    // Режим tabby удалён 2026-08-28 (CLI Tabby на маке не существует -- см. MacBackend.h).
    // Строка из старого tray.conf не должна воскресать: она теперь ровно такой же мусор,
    // как любая другая, и обрабатывается общим путём (предупреждение + дефолт).
    QVERIFY(!AppConfig::isValidOpenMode(QStringLiteral("tabby"), AppConfig::Platform::Mac));
    QVERIFY(!AppConfig::isValidOpenMode(QStringLiteral("tabby"), AppConfig::Platform::Linux));
    QVERIFY(!AppConfig::isValidOpenMode(QStringLiteral("sideways"), AppConfig::Platform::Linux));
    QVERIFY(!AppConfig::isValidOpenMode(QStringLiteral("sideways"), AppConfig::Platform::Mac));
    // И список текущей платформы -- один из трёх, а не что-то четвёртое.
    QVERIFY(AppConfig::openModes(AppConfig::currentPlatform())
            == AppConfig::openModes(AppConfig::Platform::Linux)
            || AppConfig::openModes(AppConfig::currentPlatform())
                   == AppConfig::openModes(AppConfig::Platform::Mac)
            || AppConfig::openModes(AppConfig::currentPlatform())
                   == AppConfig::openModes(AppConfig::Platform::Windows));
}

void TestAppConfig::clipboardIsTheOnlyModeOnBothPlatforms()
{
    // "Не открывать терминал вовсе" -- единственный режим, не зависящий от платформы
    // (буфер обмена есть везде). Всё остальное -- имя конкретного терминала, и общих
    // имён у Linux и мака быть не должно: общее имя означало бы, что одна и та же строка
    // в tray.conf на двух машинах значит разное.
    const QStringList lin = AppConfig::openModes(AppConfig::Platform::Linux);
    const QStringList mac = AppConfig::openModes(AppConfig::Platform::Mac);
    QStringList common;
    for (const QString &m : lin)
        if (mac.contains(m))
            common << m;
    QCOMPARE(common, QStringList{QStringLiteral("clipboard")});
}

void TestAppConfig::defaultOpenModeIsFirstOfItsList()
{
    for (AppConfig::Platform p : {AppConfig::Platform::Linux, AppConfig::Platform::Mac, AppConfig::Platform::Windows}) {
        QCOMPARE(AppConfig::defaultOpenMode(p), AppConfig::openModes(p).constFirst());
        QVERIFY(AppConfig::isValidOpenMode(AppConfig::defaultOpenMode(p), p));
    }
    // Дефолты названы явно: молчаливая смена дефолта -- это смена поведения клика по
    // сессии у всех, кто ничего не настраивал.
    QCOMPARE(AppConfig::defaultOpenMode(AppConfig::Platform::Linux), QStringLiteral("auto"));
    QCOMPARE(AppConfig::defaultOpenMode(AppConfig::Platform::Mac), QStringLiteral("terminal"));
}

void TestAppConfig::openModeDefaultsToPlatformDefault()
{
    AppConfig cfg;
    QString err;
    QVERIFY(AppConfig::load({QStringLiteral("hgs-tray")}, &cfg, &err));
    QCOMPARE(cfg.openMode, AppConfig::defaultOpenMode(AppConfig::currentPlatform()));
}

void TestAppConfig::openModeAcceptsAllValues()
{
    for (const QString &v : AppConfig::openModes(AppConfig::currentPlatform())) {
        AppConfig cfg;
        QString err;
        QVERIFY2(AppConfig::load({QStringLiteral("hgs-tray"), QStringLiteral("--open-mode"), v},
                                  &cfg, &err),
                 qPrintable(v + QStringLiteral(": ") + err));
        QCOMPARE(cfg.openMode, v);
    }
}

void TestAppConfig::openModeRejectsGarbageOnCommandLine()
{
    AppConfig cfg;
    QString err;
    QVERIFY(!AppConfig::load({QStringLiteral("hgs-tray"), QStringLiteral("--open-mode"),
                              QStringLiteral("sideways")},
                             &cfg, &err));
    // Текст должен называть и то, что пришло, и то, что ожидалось -- иначе это не
    // "понятная ошибка", а просто отказ. Ожидается набор ТЕКУЩЕЙ платформы: перечислять
    // пользователю режимы, которых у него нет, -- то же самое, что не перечислять ничего.
    QVERIFY2(err.contains(QStringLiteral("sideways")), qPrintable(err));
    for (const QString &v : AppConfig::openModes(AppConfig::currentPlatform()))
        QVERIFY2(err.contains(v), qPrintable(v + QStringLiteral(" не назван: ") + err));
}

void TestAppConfig::openModeRejectsGarbageInConfigFileAndKeepsDefault()
{
    QVERIFY(m_dir.isValid());
    const QString confPath = m_dir.filePath(QStringLiteral("tray.conf"));
    QFile f(confPath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&f) << "open-mode=sideways\n";
    f.close();
    AppConfig::setConfigPathOverrideForTests(confPath);

    AppConfig cfg;
    QString err;
    // Плохой файл — не фатальная ошибка запуска (см. комментарий в AppConfig.cpp):
    // load() всё равно возвращает true, просто оставляет дефолт.
    QVERIFY(AppConfig::load({QStringLiteral("hgs-tray")}, &cfg, &err));
    QCOMPARE(cfg.openMode, AppConfig::defaultOpenMode(AppConfig::currentPlatform()));
}

void TestAppConfig::foreignPlatformModeIsHardErrorOnCommandLine()
{
    // Командная строка -- это то, что напечатали ПРЯМО СЕЙЧАС: сделать не то, что
    // просили, хуже, чем не запуститься. Поэтому здесь, в отличие от файла, отказ.
    AppConfig cfg;
    QString err;
    QVERIFY(!AppConfig::load({QStringLiteral("hgs-tray"), QStringLiteral("--open-mode"),
                              foreignMode()},
                             &cfg, &err));
    QVERIFY2(err.contains(foreignMode()), qPrintable(err));
    // И текст обязан объяснить, что это не опечатка, а режим другой платформы -- иначе
    // человек будет искать ошибку в написании.
    QVERIFY2(err.contains(QStringLiteral("Linux")) || err.contains(QStringLiteral("macOS")),
             qPrintable(err));
}

void TestAppConfig::foreignPlatformModeInConfigFileFallsBackToPlatformDefault()
{
    // tray.conf, приехавший с другой машины (общие dotfiles, rsync домашнего каталога),
    // НЕ должен ломать трей: предупреждение в stderr и дефолт платформы -- тем же
    // правилом, каким обрабатывается любое незнакомое значение из файла.
    QVERIFY(m_dir.isValid());
    const QString confPath = freshConfPath(QStringLiteral("foreign"));
    QFile f(confPath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&f) << "open-mode=" << foreignMode() << "\n";
    f.close();
    AppConfig::setConfigPathOverrideForTests(confPath);

    AppConfig cfg;
    QString err;
    QVERIFY2(AppConfig::load({QStringLiteral("hgs-tray")}, &cfg, &err), qPrintable(err));
    QCOMPARE(cfg.openMode, AppConfig::defaultOpenMode(AppConfig::currentPlatform()));
}

void TestAppConfig::commandLineOpenModeBeatsConfigFile()
{
    QVERIFY(m_dir.isValid());
    const QString confPath = m_dir.filePath(QStringLiteral("tray.conf"));
    QFile f(confPath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&f) << "open-mode=" << modeA() << "\n";
    f.close();
    AppConfig::setConfigPathOverrideForTests(confPath);

    AppConfig cfg;
    QString err;
    QVERIFY(AppConfig::load({QStringLiteral("hgs-tray"), QStringLiteral("--open-mode"), modeB()},
                            &cfg, &err));
    // modeB не только не равен файлу, но и не равен дефолту -- то есть прийти он мог
    // только из командной строки.
    QCOMPARE(cfg.openMode, modeB());
}

void TestAppConfig::configFileOpenModeAppliesWhenCommandLineSilent()
{
    QVERIFY(m_dir.isValid());
    const QString confPath = m_dir.filePath(QStringLiteral("tray.conf"));
    QFile f(confPath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&f) << "open-mode=" << modeB() << "\n";
    f.close();
    AppConfig::setConfigPathOverrideForTests(confPath);

    AppConfig cfg;
    QString err;
    QVERIFY(AppConfig::load({QStringLiteral("hgs-tray")}, &cfg, &err));
    QCOMPARE(cfg.openMode, modeB());   // не дефолт -- значит, прочиталось из файла
}

// --- запись в tray.conf (AppConfig::writeConfigValue) ------------------------------
// Пункт меню «Open in» сохраняет выбор именно этой функцией, а файл при этом --
// пользовательский: в нём могут лежать чужие ключи и его же комментарии, и потерять их
// при записи нельзя. Это чистая логика над строками, десктоп ей не нужен.

void TestAppConfig::configFileAcceptsClipboardMode()
{
    // Ровно тот путь, которым режим переживает перезапуск трея: меню записало
    // open-mode=clipboard, следующий старт читает его из файла.
    QVERIFY(m_dir.isValid());
    const QString confPath = m_dir.filePath(QStringLiteral("tray.conf"));
    QFile f(confPath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&f) << "open-mode=clipboard\n";
    f.close();
    AppConfig::setConfigPathOverrideForTests(confPath);

    AppConfig cfg;
    QString err;
    QVERIFY2(AppConfig::load({QStringLiteral("hgs-tray")}, &cfg, &err), qPrintable(err));
    QCOMPARE(cfg.openMode, QStringLiteral("clipboard"));
}

void TestAppConfig::writeCreatesFileWithHeaderWhenAbsent()
{
    QVERIFY(m_dir.isValid());
    const QString confPath = freshConfPath(QStringLiteral("absent"));
    QVERIFY(!QFile::exists(confPath));
    AppConfig::setConfigPathOverrideForTests(confPath);

    QString err;
    QVERIFY2(AppConfig::writeConfigValue(QStringLiteral("open-mode"), modeB(), &err),
             qPrintable(err));
    QVERIFY(QFile::exists(confPath));

    const QStringList lines = readLines(confPath);
    // Шапка: файл, который пользователь никогда не видел, должен объяснить сам себя.
    QVERIFY2(lines.constFirst().startsWith(QLatin1Char('#')), qPrintable(lines.join(QLatin1Char('|'))));
    QVERIFY(lines.contains(QStringLiteral("open-mode=") + modeB()));
    // И, главное, обратное чтение даёт ровно то, что записали.
    AppConfig cfg;
    QVERIFY(AppConfig::load({QStringLiteral("hgs-tray")}, &cfg, &err));
    QCOMPARE(cfg.openMode, modeB());
}

void TestAppConfig::writePreservesOtherKeysCommentsAndOrder()
{
    QVERIFY(m_dir.isValid());
    const QString confPath = freshConfPath(QStringLiteral("preserve"));
    const QString original = QStringLiteral(
        "# мой комментарий, писан руками\n"
        "hgs=/opt/hgs/bin/hgs\n"
        "\n"
        "# а тут пусто было специально\n"
        "local-poll=5000\n");
    QFile f(confPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(original.toUtf8());
    f.close();
    AppConfig::setConfigPathOverrideForTests(confPath);

    QString err;
    QVERIFY2(AppConfig::writeConfigValue(QStringLiteral("open-mode"), modeB(), &err),
             qPrintable(err));

    // Всё, что было, осталось на своих местах и в том же порядке -- дословно.
    const QStringList lines = readLines(confPath);
    const QStringList before = original.split(QLatin1Char('\n'));
    for (int i = 0; i < before.size() - 1; ++i)  // -1: хвостовая пустая после последнего \n
        QCOMPARE(lines.value(i), before.at(i));
    QVERIFY(lines.contains(QStringLiteral("open-mode=") + modeB()));

    // Чужие ключи не только уцелели в файле, но и читаются как раньше.
    AppConfig cfg;
    QVERIFY(AppConfig::load({QStringLiteral("hgs-tray")}, &cfg, &err));
    QCOMPARE(cfg.hgsPath, QStringLiteral("/opt/hgs/bin/hgs"));
    QCOMPARE(cfg.localPollMs, 5000);
    QCOMPARE(cfg.openMode, modeB());
}

void TestAppConfig::writeReplacesExistingLineInPlaceWithoutAppending()
{
    QVERIFY(m_dir.isValid());
    const QString confPath = freshConfPath(QStringLiteral("inplace"));
    QFile f(confPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write((QStringLiteral("open-mode=") + modeA()
             + QStringLiteral("\nicon-color=#ff0000\n")).toUtf8());
    f.close();
    AppConfig::setConfigPathOverrideForTests(confPath);

    QString err;
    QVERIFY2(AppConfig::writeConfigValue(QStringLiteral("open-mode"), modeB(), &err),
             qPrintable(err));

    const QStringList lines = readLines(confPath);
    // Строка заменена НА СВОЁМ МЕСТЕ (первой), а не дописана в конец.
    QCOMPARE(lines.value(0), QStringLiteral("open-mode=") + modeB());
    QCOMPARE(lines.value(1), QStringLiteral("icon-color=#ff0000"));
    QCOMPARE(lines.count(QStringLiteral("open-mode=") + modeB()), 1);
    QCOMPARE(lines.filter(QStringLiteral("open-mode")).size(), 1);
}

void TestAppConfig::writeRepeatedKeepsExactlyOneLine()
{
    QVERIFY(m_dir.isValid());
    const QString confPath = freshConfPath(QStringLiteral("repeat"));
    AppConfig::setConfigPathOverrideForTests(confPath);

    QString err;
    // Пользователь щёлкает переключателем несколько раз подряд -- файл не должен расти.
    // Последовательность -- все режимы платформы и возврат к первому: ровно то, что
    // делает рука, перебирающая пункты подменю.
    QStringList seq = AppConfig::openModes(AppConfig::currentPlatform());
    seq << seq.constFirst();
    for (const QString &v : seq)
        QVERIFY2(AppConfig::writeConfigValue(QStringLiteral("open-mode"), v, &err), qPrintable(err));
    const QStringList lines = readLines(confPath);
    QCOMPARE(lines.filter(QStringLiteral("open-mode=")).size(), 1);
    QVERIFY(lines.contains(QStringLiteral("open-mode=") + seq.constLast()));
}

void TestAppConfig::writeWithDuplicateLinesStillReadsBackWritten()
{
    QVERIFY(m_dir.isValid());
    const QString confPath = freshConfPath(QStringLiteral("dupes"));
    QFile f(confPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    // Дубль ключа руками: applyConfigFile() применяет значение на каждой встреченной
    // строке, то есть выигрывает ПОСЛЕДНЯЯ -- переписать надо именно её, иначе запись
    // "не подействует" при обратном чтении.
    // Обе строки -- modeA: если запись переписала не последнюю, обратное чтение вернёт
    // именно modeA, и тест это поймает.
    f.write((QStringLiteral("open-mode=") + modeA() + QStringLiteral("\nterminal=xterm -e {cmd}\n")
             + QStringLiteral("open-mode=") + modeA() + QStringLiteral("\n")).toUtf8());
    f.close();
    AppConfig::setConfigPathOverrideForTests(confPath);

    QString err;
    QVERIFY2(AppConfig::writeConfigValue(QStringLiteral("open-mode"), modeB(), &err),
             qPrintable(err));

    AppConfig cfg;
    QVERIFY(AppConfig::load({QStringLiteral("hgs-tray")}, &cfg, &err));
    QCOMPARE(cfg.openMode, modeB());
    // Ни одна строка при этом не удалена -- правка минимальная.
    QCOMPARE(readLines(confPath).size(), 4);   // 3 строки + хвостовая пустая
}

void TestAppConfig::writeAppendsNewlineToFileThatLackedOne()
{
    QVERIFY(m_dir.isValid());
    const QString confPath = freshConfPath(QStringLiteral("nonewline"));
    QFile f(confPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray("icon-color=#00ff00"));   // БЕЗ перевода строки в конце
    f.close();
    AppConfig::setConfigPathOverrideForTests(confPath);

    QString err;
    QVERIFY2(AppConfig::writeConfigValue(QStringLiteral("open-mode"), modeB(), &err),
             qPrintable(err));

    QFile check(confPath);
    QVERIFY(check.open(QIODevice::ReadOnly));
    const QByteArray got = check.readAll();
    // Дописано новой строкой, а не приклеено к последней.
    QCOMPARE(got, (QStringLiteral("icon-color=#00ff00\nopen-mode=") + modeB()
                   + QStringLiteral("\n")).toUtf8());
}

void TestAppConfig::writeRejectsBadKeyAndLeavesFileAlone()
{
    QVERIFY(m_dir.isValid());
    const QString confPath = freshConfPath(QStringLiteral("badkey"));
    QVERIFY(!QFile::exists(confPath));
    AppConfig::setConfigPathOverrideForTests(confPath);

    QString err;
    QVERIFY(!AppConfig::writeConfigValue(QStringLiteral("open=mode"), QStringLiteral("tab"), &err));
    QVERIFY2(!err.isEmpty(), "отказ обязан объяснить причину");
    // Файл не создан: неудачная запись не оставляет за собой полуфабрикат.
    QVERIFY(!QFile::exists(confPath));
}

void TestAppConfig::windowsModesKeepSharedValuesAndAddConsole()
{
    // Windows reads the Linux names (Windows Terminal there), so a tray.conf shared
    // through dotfiles keeps working, and adds a console window.
    QCOMPARE(AppConfig::openModes(AppConfig::Platform::Windows),
             (QStringList{"auto", "tab", "window", "console", "clipboard"}));
    QCOMPARE(AppConfig::defaultOpenMode(AppConfig::Platform::Windows), QStringLiteral("auto"));
    for (const QString &mode : AppConfig::openModes(AppConfig::Platform::Linux))
        QVERIFY(AppConfig::isValidOpenMode(mode, AppConfig::Platform::Windows));
    QVERIFY(!AppConfig::isValidOpenMode("console", AppConfig::Platform::Linux));
    QVERIFY(!AppConfig::isValidOpenMode("terminal", AppConfig::Platform::Windows));
    if (AppConfig::currentPlatform() == AppConfig::Platform::Windows)
        return;
    // Elsewhere console is a Windows value: rejected with that explanation.
    AppConfig cfg; QString err;
    QVERIFY(!AppConfig::load({"hgs-tray", "--open-mode", "console"}, &cfg, &err));
    QVERIFY2(err.contains("is a Windows mode"), qPrintable(err));
}

void TestAppConfig::homeVariableLocatesTrayConfig()
{
    AppConfig::setConfigPathOverrideForTests(QString());
    const QByteArray saved = qgetenv("HOME");
    qputenv("HOME", m_dir.path().toUtf8());
    QCOMPARE(AppConfig::configPath(), m_dir.path() + "/.config/hgs/tray.conf");
    qunsetenv("HOME");
    QCOMPARE(AppConfig::configPath(), QDir::homePath() + "/.config/hgs/tray.conf");
    qputenv("HOME", saved);
}

void TestAppConfig::bundledHgsIsPreferredOnWindows()
{
    QTemporaryDir app; QVERIFY(app.isValid());
    QFile bundled(app.filePath("hgs.exe")); QVERIFY(bundled.open(QIODevice::WriteOnly)); bundled.close();
    QCOMPARE(AppConfig::resolveHgsPath({}, app.path()), app.filePath("hgs.exe"));
    // A configured path wins and becomes absolute; a bare name is looked up in PATH.
    QCOMPARE(AppConfig::resolveHgsPath(m_dir.filePath("custom/hgs"), app.path()), m_dir.filePath("custom/hgs"));
    QCOMPARE(AppConfig::resolveHgsPath("no-such-hgs-for-zerus", app.path()), QString("no-such-hgs-for-zerus"));
    QTemporaryDir empty; QVERIFY(empty.isValid());
    const QString fallback = AppConfig::resolveHgsPath({}, empty.path());
    QVERIFY(fallback == "hgs" || QFileInfo(fallback).isAbsolute());
}

void TestAppConfig::enabledWindowsRunsTheStableCli()
{
    // Spec D2: `session-host status --json` of the bundled hgs.exe decides.
    const QString bundled = "C:/Program Files/Zerus/hgs.exe", stable = "C:/Users/user/.local/bin/hgs.exe";
    const QString built = QString(64, 'a'), other = QString(64, 'b');
    const auto status = [](bool enabled, const QString &version, const QJsonValue &sha256, const QJsonValue &staged) {
        return QJsonObject{{"enabled", enabled}, {"version", version},
            {"marker", "C:\\Users\\user\\.config\\hgs\\local-sessions"},
            {"cli", QJsonObject{{"path", "C:\\Users\\user\\.local\\bin\\hgs.exe"}, {"installed", !sha256.isNull()},
                                {"sha256", sha256}, {"version", staged}}},
            {"image", QJsonValue::Null}, {"host", QJsonObject{{"state", "stopped"}}}};
    };
    const auto choose = [&](const QJsonObject &answer, const QString &sha256 = QString(64, 'a'), const QString &configured = {}) {
        const auto choice = AppConfig::chooseHgs(answer, bundled, sha256, configured);
        return QStringList{choice.path, choice.refresh ? "refresh" : "keep", choice.fallback};
    };
    const QStringList asBundled{bundled, "keep", bundled}, asStable{stable, "keep", stable},
        refreshed{stable, "refresh", stable};
    // Not enabled, or no answer (an older hgs.exe): the bundled build. An
    // uninstalled computer keeps its staged files and still runs the bundled one.
    QCOMPARE(choose(status(false, "1.52.0", QJsonValue::Null, QJsonValue::Null)), asBundled);
    QCOMPARE(choose(status(false, "1.52.0", other, "1.51.0")), asBundled);
    QCOMPARE(choose({}), asBundled);
    // Enabled: the stable CLI, in the form resolveHgsPath gives.
    QCOMPARE(choose(status(true, "1.52.0", built.toUpper(), "1.52.0")), asStable);
    // A different build at least as new is staged first; a refresh that fails
    // keeps the staged CLI.
    QCOMPARE(choose(status(true, "1.52.0", other, "1.51.9")), refreshed);
    QCOMPARE(choose(status(true, "1.52.0", other, "1.52.0")), refreshed);
    QCOMPARE(choose(status(true, "1.52.0-dev.12", other, "1.52.0-dev.9")), refreshed);
    QCOMPARE(choose(status(true, "1.52.0", other, "1.52.0-dev.30")), refreshed);
    QCOMPARE(choose(status(true, "2.0.0", other, "1.99.99")), refreshed);
    // A newer staged build stays.
    QCOMPARE(choose(status(true, "1.52.0", other, "1.53.0")), asStable);
    QCOMPARE(choose(status(true, "1.52.0-dev.9", other, "1.52.0")), asStable);
    QCOMPARE(choose(status(true, "1.52.0-dev.9", other, "1.52.0-dev.12")), asStable);
    QCOMPARE(choose(status(true, "1.52.0-alpha", other, "1.52.0-beta")), asStable);
    // A staged build of unknown version counts as older.
    QCOMPARE(choose(status(true, "1.52.0", other, QJsonValue::Null)), refreshed);
    // A bundled build that names no version, or cannot be read, refreshes nothing.
    QCOMPARE(choose(status(true, "", other, "1.51.0")), asStable);
    QCOMPARE(choose(status(true, "1.52.0", other, "1.51.0"), QString()), asStable);
    // Enabled without a staged CLI: staged first, and the bundled build runs
    // when that fails.
    QCOMPARE(choose(status(true, "1.52.0", QJsonValue::Null, QJsonValue::Null)), (QStringList{stable, "refresh", bundled}));
    // An explicit --hgs or hgs= is used as given.
    QCOMPARE(choose(status(true, "1.52.0", other, "1.51.0"), built, "D:/tools/hgs.exe"),
             (QStringList{"D:/tools/hgs.exe", "keep", "D:/tools/hgs.exe"}));
}

void TestAppConfig::localPollingFollowsPlatformAndEnablement()
{
    using MachinePlatform::Style;
    // Unix keeps the configured interval, as before.
    for (const int configured : {500, 2000, 4999, 5000, 60000})
        QCOMPARE(AppConfig::localPollInterval(configured, true, Style::Posix), configured);
    // A control console waits 15 s; an enabled Windows computer at least 5 s.
    for (const auto style : {Style::Posix, Style::Windows}) {
        QCOMPARE(AppConfig::localPollInterval(2000, false, style), 15000);
        QCOMPARE(AppConfig::localPollInterval(20000, false, style), 20000);
    }
    QCOMPARE(AppConfig::localPollInterval(2000, true, Style::Windows), 5000);
    QCOMPARE(AppConfig::localPollInterval(5000, true, Style::Windows), 5000);
    QCOMPARE(AppConfig::localPollInterval(8000, true, Style::Windows), 8000);
}

void TestAppConfig::singleInstanceNameIsPerUserOnWindows()
{
    QCOMPARE(SingleInstance::key(), QString("hgs-tray"));
    const QString first = SingleInstance::key("C:/Users/alice"), second = SingleInstance::key("C:/Users/bob");
    QVERIFY(first.startsWith("hgs-tray-")); QCOMPARE(first.size(), 9 + 16);
    QVERIFY(first != second);
    // Windows paths compare without case.
    QCOMPARE(SingleInstance::key("c:/users/ALICE"), first);
}

namespace {
using Service = SingleInstance::Service;
using ServiceState = SingleInstance::Service::State;
using Outcome = SingleInstance::Outcome;

// A start on a socket in `dir`, where the service manager answers `service`.
// self is compared with the service's pid only, so fake ids stand for processes.
SingleInstance::Startup startupIn(const QString &dir, qint64 self, std::function<Service()> service)
{
    SingleInstance::Startup startup;
    startup.key = dir + QStringLiteral("/hgs-tray");
    startup.lockPath = dir + QStringLiteral("/hgs-tray-start.lock");
    startup.activation = "show\n";
    startup.self = self;
    startup.service = std::move(service);
    startup.pollMs = 20;
    return startup;
}

Service noService() { return {}; }

bool answers(const QString &key)
{
    QLocalSocket probe;
    probe.connectToServer(key);
    return probe.waitForConnected(1000);
}

// The next activation line a GUI receives, empty when none arrives.
QByteArray nextActivation(QLocalServer &server)
{
    if (!server.hasPendingConnections() && !server.waitForNewConnection(5000))
        return {};
    std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
    while (!socket->canReadLine() && socket->waitForReadyRead(5000)) {
    }
    return socket->readLine();
}
} // namespace

void TestAppConfig::serviceStateFollowsTheServiceManager()
{
    const auto systemd = [](const QByteArray &text) {
        const Service service = SingleInstance::systemdService(text);
        return std::make_pair(service.state, service.pid);
    };
    QCOMPARE(systemd("ActiveState=active\nMainPID=4242\n"), std::make_pair(ServiceState::Running, qint64(4242)));
    // Restart=on-failure waits RestartSec before the next process.
    QCOMPARE(systemd("MainPID=0\nActiveState=activating\n"), std::make_pair(ServiceState::Starting, qint64(0)));
    // `systemctl restart` stops the previous process first.
    QCOMPARE(systemd("ActiveState=deactivating\nMainPID=4242\n"), std::make_pair(ServiceState::Starting, qint64(4242)));
    for (const QByteArray &absent : {QByteArray("ActiveState=inactive\nMainPID=0\n"),
                                     QByteArray("ActiveState=failed\nMainPID=0\n"), QByteArray(), QByteArray("garbage")})
        QCOMPARE(systemd(absent), std::make_pair(ServiceState::Absent, qint64(0)));
    QCOMPARE(systemd("ActiveState=active\nMainPID=unknown\n"), std::make_pair(ServiceState::Running, qint64(0)));

    const auto launchd = [](const QByteArray &state, const QByteArray &pid) {
        // Only the job's own properties count, not those of nested blocks.
        const QByteArray text = "gui/501/com.hgdev.hgs-tray = {\n"
                                "\tactive count = 1\n"
                                "\tpath = /Users/example/Library/LaunchAgents/com.hgdev.hgs-tray.plist\n"
                                "\ttype = LaunchAgent\n"
                                + (state.isEmpty() ? QByteArray() : "\tstate = " + state + "\n")
                                + "\n\tprogram = /Applications/Zerus.app/Contents/MacOS/hgs-tray\n"
                                  "\targuments = {\n"
                                  "\t\t/Applications/Zerus.app/Contents/MacOS/hgs-tray\n"
                                  "\t}\n"
                                  "\tevent triggers = {\n"
                                  "\t\tcom.example.trigger => {\n"
                                  "\t\t\tstate = running\n"
                                  "\t\t\tpid = 7\n"
                                  "\t\t}\n"
                                  "\t}\n"
                                + (pid.isEmpty() ? QByteArray() : "\tpid = " + pid + "\n")
                                + "\tlast exit code = (never exited)\n"
                                  "}\n";
        const Service service = SingleInstance::launchdService(text);
        return std::make_pair(service.state, service.pid);
    };
    QCOMPARE(launchd("running", "4242"), std::make_pair(ServiceState::Running, qint64(4242)));
    QCOMPARE(launchd("spawn scheduled", ""), std::make_pair(ServiceState::Starting, qint64(0)));
    QCOMPARE(launchd("not running", ""), std::make_pair(ServiceState::Absent, qint64(0)));
    QCOMPARE(launchd("", ""), std::make_pair(ServiceState::Absent, qint64(0)));
    QCOMPARE(SingleInstance::launchdService({}).state, ServiceState::Absent);
}

void TestAppConfig::onlyAnotherServiceProcessKeepsTheSocket()
{
    QVERIFY(!SingleInstance::waitsForService(Service(), 100));
    // The service's own process claims the socket.
    QVERIFY(!SingleInstance::waitsForService(Service{ServiceState::Running, 100}, 100));
    QVERIFY(SingleInstance::waitsForService(Service{ServiceState::Running, 200}, 100));
    // A scheduled restart, and the previous process while it stops.
    QVERIFY(SingleInstance::waitsForService(Service{ServiceState::Starting, 0}, 100));
    QVERIFY(SingleInstance::waitsForService(Service{ServiceState::Starting, 200}, 100));
}

void TestAppConfig::concurrentStartsLeaveOneReachableOwner()
{
#ifdef Q_OS_WIN
    QSKIP("Windows starts keep their own lifetime lock (main.cpp)");
#endif
    // Without the lock, two of these removed each other's socket or failed to
    // listen: a GUI that nothing reached, or a start that exited with code 3.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    constexpr int count = 8;
    std::atomic<int> ready{0}, finished{0};
    std::atomic<bool> go{false};
    std::vector<Outcome> outcomes(count, Outcome::Failed);
    std::vector<char> listening(count, false);
    QMutex mutex;
    QList<QByteArray> received;
    std::vector<std::unique_ptr<QThread>> threads;
    for (int i = 0; i < count; ++i) {
        threads.emplace_back(QThread::create([&, i] {
            QLocalServer server;
            SingleInstance::Startup startup = startupIn(dir.path(), 100 + i, noService);
            startup.activation = QByteArray::number(i) + '\n';
            ++ready;
            while (!go)
                QThread::yieldCurrentThread();
            outcomes[i] = SingleInstance::start(startup, server);
            if (outcomes[i] == Outcome::Claimed)
                for (int n = 1; n < count; ++n) {
                    const QByteArray activation = nextActivation(server);
                    const QMutexLocker locker(&mutex);
                    received << activation;
                }
            // Every server stays until all starts are done, as running GUIs would.
            ++finished;
            while (finished < count)
                QThread::msleep(5);
            listening[i] = server.isListening();
        }));
        threads.back()->start();
    }
    while (ready < count)
        QThread::yieldCurrentThread();
    go = true;
    for (const auto &thread : threads)
        QVERIFY(thread->wait(60000));

    QCOMPARE(std::count(outcomes.begin(), outcomes.end(), Outcome::Claimed), 1);
    QCOMPARE(std::count(outcomes.begin(), outcomes.end(), Outcome::Forwarded), count - 1);
    const int owner = int(std::find(outcomes.begin(), outcomes.end(), Outcome::Claimed) - outcomes.begin());
    QVERIFY(listening[owner]);
    // Each other start reached that one GUI.
    QList<QByteArray> expected;
    for (int i = 0; i < count; ++i)
        if (i != owner)
            expected << QByteArray::number(i) + '\n';
    std::sort(received.begin(), received.end());
    QCOMPARE(received, expected);
    QVERIFY(!QFileInfo::exists(dir.filePath("hgs-tray-start.lock")));
}

void TestAppConfig::launcherForwardsOnceTheStartingServiceListens()
{
#ifdef Q_OS_WIN
    QSKIP("Windows has no service manager here (main.cpp)");
#endif
    // `systemctl --user restart hgs-tray` and then `hgs-tray --sessions`: the
    // launcher used to claim the socket first, and the service then forwarded to it
    // and exited, leaving no managed GUI.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    constexpr qint64 launcherPid = 100, servicePid = 200;
    const auto service = [] { return Service{ServiceState::Running, servicePid}; };
    std::atomic<Outcome> launched{Outcome::Failed};
    std::atomic<bool> launcherListened{true};
    QStringList reports;
    std::unique_ptr<QThread> launcher(QThread::create([&] {
        QLocalServer server;
        SingleInstance::Startup startup = startupIn(dir.path(), launcherPid, service);
        startup.activation = "sessions\n";
        startup.report = [&reports](const QString &text) { reports << text; };
        launched = SingleInstance::start(startup, server);
        launcherListened = server.isListening();
    }));
    launcher->start();
    QTest::qWait(500);
    QVERIFY(launcher->isRunning());
    QVERIFY(!answers(dir.filePath("hgs-tray")));

    // The service's own process claims the socket and opens the launcher's workspace.
    QLocalServer owner;
    QCOMPARE(SingleInstance::start(startupIn(dir.path(), servicePid, service), owner), Outcome::Claimed);
    QCOMPARE(nextActivation(owner), QByteArray("sessions\n"));
    QVERIFY(launcher->wait(10000));
    QCOMPARE(launched.load(), Outcome::Forwarded);
    QVERIFY(!launcherListened);
    QCOMPARE(reports, QStringList{"waiting for the Zerus service (pid 200) to open its socket"});
}

void TestAppConfig::launcherClaimsWhenTheServiceStopsOrNeverListens()
{
#ifdef Q_OS_WIN
    QSKIP("Windows has no service manager here (main.cpp)");
#endif
    {
        // The service stops before it listens, for example when it crashes on start.
        // (Without a system tray it listens first and then exits: main.cpp checks
        // the tray only after it has claimed the socket.)
        QTemporaryDir dir;
        QElapsedTimer elapsed;
        elapsed.start();
        const auto service = [&elapsed] {
            return elapsed.elapsed() < 400 ? Service{ServiceState::Running, 200} : Service();
        };
        QLocalServer server;
        QCOMPARE(SingleInstance::start(startupIn(dir.path(), 100, service), server), Outcome::Claimed);
        QVERIFY(elapsed.elapsed() >= 400);
        QVERIFY(elapsed.elapsed() < 10000);
        QVERIFY(server.isListening());
        QVERIFY(answers(dir.filePath("hgs-tray")));
    }
    {
        // A service that never listens delays the launcher by serviceWaitMs at most.
        QTemporaryDir dir;
        QStringList reports;
        SingleInstance::Startup startup =
            startupIn(dir.path(), 100, [] { return Service{ServiceState::Starting, 0}; });
        startup.serviceWaitMs = 300;
        startup.report = [&reports](const QString &text) { reports << text; };
        QElapsedTimer elapsed;
        elapsed.start();
        QLocalServer server;
        QCOMPARE(SingleInstance::start(startup, server), Outcome::Claimed);
        QVERIFY(elapsed.elapsed() >= 300);
        QVERIFY(server.isListening());
        QCOMPARE(reports, (QStringList{"waiting for the Zerus service to start",
                                       "the Zerus service did not open its socket within 0.3 s; starting here"}));
    }
}

void TestAppConfig::staleSocketAndLockRecoverAfterACrash()
{
#ifdef Q_OS_WIN
    QSKIP("Windows pipes leave no socket file behind");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString key = dir.filePath("hgs-tray");
    // A crashed GUI's socket: bound, never unlinked, nobody listening.
    {
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        const QByteArray path = QFile::encodeName(key);
        QVERIFY(size_t(path.size()) < sizeof(address.sun_path));
        std::memcpy(address.sun_path, path.constData(), size_t(path.size()));
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(fd >= 0);
        QCOMPARE(::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
        ::close(fd);
    }
    QVERIFY(QFileInfo::exists(key));
    QVERIFY(!answers(key));
    // A start that crashed while it held the lock.
    QProcess finished;
    finished.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exit 0")});
    QVERIFY(finished.waitForStarted());
    const qint64 deadPid = finished.processId();
    QVERIFY(finished.waitForFinished());
    {
        QFile lock(dir.filePath("hgs-tray-start.lock"));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.write(QByteArray::number(deadPid) + "\nhgs-tray\n" + QSysInfo::machineHostName().toUtf8() + '\n');
    }

    QStringList reports;
    SingleInstance::Startup startup = startupIn(dir.path(), 100, noService);
    startup.report = [&reports](const QString &text) { reports << text; };
    QElapsedTimer elapsed;
    elapsed.start();
    QLocalServer server;
    QCOMPARE(SingleInstance::start(startup, server), Outcome::Claimed);
    QVERIFY(elapsed.elapsed() < 5000);
    QVERIFY(reports.isEmpty());
    QVERIFY(answers(key));
#endif
}

void TestAppConfig::aStuckStartDelaysOthersBoundedly()
{
#ifdef Q_OS_WIN
    QSKIP("Windows starts keep their own lifetime lock (main.cpp)");
#endif
    // A live start that keeps the lock, for example one stopped in a debugger.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QLockFile stuck(dir.filePath("hgs-tray-start.lock"));
    QVERIFY(stuck.tryLock(0));
    QStringList reports;
    SingleInstance::Startup startup = startupIn(dir.path(), 100, noService);
    startup.lockWaitMs = 300;
    startup.report = [&reports](const QString &text) { reports << text; };
    QElapsedTimer elapsed;
    elapsed.start();
    QLocalServer server;
    QCOMPARE(SingleInstance::start(startup, server), Outcome::Claimed);
    QVERIFY(elapsed.elapsed() >= 300);
    QCOMPARE(reports, QStringList{QStringLiteral("another start keeps %1 locked; starting without it")
                                      .arg(dir.filePath("hgs-tray-start.lock"))});
    QVERIFY(stuck.isLocked());
}

void TestAppConfig::restartKeepsSettingsButNotOneShotOptions()
{
    // Windows restarts a crashed tray with these (RegisterApplicationRestart).
    const QStringList args{"C:/Zerus/hgs-tray.exe", "--sessions", "--hgs", "D:/tools/hgs.exe", "--open-mode",
                           "console", "--terminal", "--sessions", "--local-poll", "5000", "--icon-color", "#fff",
                           "--peer-poll", "60000"};
    QCOMPARE(AppConfig::restartArguments(args),
             (QStringList{"--hgs", "D:/tools/hgs.exe", "--open-mode", "console", "--terminal", "--sessions",
                          "--local-poll", "5000", "--icon-color", "#fff", "--peer-poll", "60000"}));
    QVERIFY(AppConfig::restartArguments({"hgs-tray"}).isEmpty());
    QVERIFY(AppConfig::restartArguments({"hgs-tray", "--sessions"}).isEmpty());
}

QString TestAppConfig::writeHgs(const QString &dir, bool executable)
{
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/hgs");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write("#!/bin/sh\n");
    f.close();
    f.setPermissions(executable ? QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                : QFile::ReadOwner | QFile::WriteOwner);
    return path;
}

void TestAppConfig::hgsInstallDirsPreferUserInstallThenHomebrew()
{
    // Same order in which hgs extends PATH for its own children (src/platform/unix.rs).
    QCOMPARE(AppConfig::hgsInstallDirs(QStringLiteral("/home/example")),
             (QStringList{QStringLiteral("/home/example/.local/bin"), QStringLiteral("/opt/homebrew/bin"),
                          QStringLiteral("/usr/local/bin")}));
}

void TestAppConfig::fallbackHgsKeepsPathLookupWhenPathProvidesHgs()
{
#ifdef Q_OS_WIN
    QSKIP("Windows finds hgs.exe next to hgs-tray.exe, then in PATH");
#endif
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(!writeHgs(dir.filePath(QStringLiteral("path")), true).isEmpty());
    QVERIFY(!writeHgs(dir.filePath(QStringLiteral("local")), true).isEmpty());
    // A terminal launch keeps resolving bare "hgs" through the user's PATH.
    QCOMPARE(AppConfig::fallbackHgsPath(dir.filePath(QStringLiteral("path")),
                                        {dir.filePath(QStringLiteral("local"))}),
             QString());
}

void TestAppConfig::fallbackHgsFindsFirstExecutableInstall()
{
#ifdef Q_OS_WIN
    QSKIP("Windows finds hgs.exe next to hgs-tray.exe, then in PATH");
#endif
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir().mkpath(dir.filePath(QStringLiteral("empty")));
    QVERIFY(!writeHgs(dir.filePath(QStringLiteral("plain")), false).isEmpty());
    const QString first = writeHgs(dir.filePath(QStringLiteral("first")), true);
    QVERIFY(!writeHgs(dir.filePath(QStringLiteral("second")), true).isEmpty());
    // Finder, Dock and Spotlight start the app with PATH=/usr/bin:/bin:/usr/sbin:/sbin.
    QCOMPARE(AppConfig::fallbackHgsPath(dir.filePath(QStringLiteral("empty")),
                                        {dir.filePath(QStringLiteral("missing")),
                                         dir.filePath(QStringLiteral("plain")),
                                         dir.filePath(QStringLiteral("first")),
                                         dir.filePath(QStringLiteral("second"))}),
             first);
}

void TestAppConfig::fallbackHgsIsEmptyWithoutAnyInstall()
{
#ifdef Q_OS_WIN
    QSKIP("Windows finds hgs.exe next to hgs-tray.exe, then in PATH");
#endif
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QCOMPARE(AppConfig::fallbackHgsPath(dir.path(), {dir.filePath(QStringLiteral("missing"))}),
             QString());
}

namespace {
// A synthetic Foo.app/Contents/MacOS directory with hgs-tray and, optionally, hgs.
QString makeBundle(const QString &root, const QString &bundle, bool withHgs, bool executable)
{
    const QString macos = root + QLatin1Char('/') + bundle + QStringLiteral("/Contents/MacOS");
    QDir().mkpath(macos);
    for (const QString &name : {QStringLiteral("hgs-tray"), QStringLiteral("hgs")}) {
        if (name == QLatin1String("hgs") && !withHgs)
            continue;
        QFile file(macos + QLatin1Char('/') + name);
        file.open(QIODevice::WriteOnly);
        file.write("#!/bin/sh\n");
        file.close();
        auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
        if (executable)
            permissions |= QFileDevice::ExeOwner;
        file.setPermissions(permissions);
    }
    return macos;
}
} // namespace

void TestAppConfig::bundledHgsIsFoundNextToMacBundleExecutable()
{
#ifdef Q_OS_WIN
    QSKIP("Windows has no executable bit for the bundle's extensionless hgs");
#endif
    QTemporaryDir dir;
    const QString macos = makeBundle(dir.path(), QStringLiteral("Zerus.app"), true, true);
    QCOMPARE(AppConfig::bundledHgsPath(macos + QStringLiteral("/hgs-tray"), AppConfig::Platform::Mac),
             QFileInfo(macos + QStringLiteral("/hgs")).absoluteFilePath());
}

void TestAppConfig::bundledHgsRequiresAnExecutableFile()
{
    QTemporaryDir dir;
    const QString missing = makeBundle(dir.path(), QStringLiteral("Missing.app"), false, true);
    QVERIFY(AppConfig::bundledHgsPath(missing + QStringLiteral("/hgs-tray"), AppConfig::Platform::Mac).isEmpty());
    const QString plain = makeBundle(dir.path(), QStringLiteral("Plain.app"), true, false);
    QVERIFY(AppConfig::bundledHgsPath(plain + QStringLiteral("/hgs-tray"), AppConfig::Platform::Mac).isEmpty());
}

void TestAppConfig::bundledHgsRequiresAnApplicationBundle()
{
    QTemporaryDir dir;
    const QString build = dir.path() + QStringLiteral("/build");
    QDir().mkpath(build);
    for (const QString &name : {QStringLiteral("hgs-tray"), QStringLiteral("hgs")}) {
        QFile file(build + QLatin1Char('/') + name);
        file.open(QIODevice::WriteOnly);
        file.close();
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    QVERIFY(AppConfig::bundledHgsPath(build + QStringLiteral("/hgs-tray"), AppConfig::Platform::Mac).isEmpty());
}

void TestAppConfig::bundledHgsIsMacOnly()
{
    QTemporaryDir dir;
    const QString macos = makeBundle(dir.path(), QStringLiteral("Zerus.app"), true, true);
    QVERIFY(AppConfig::bundledHgsPath(macos + QStringLiteral("/hgs-tray"), AppConfig::Platform::Linux).isEmpty());
}

QTEST_GUILESS_MAIN(TestAppConfig)
#include "test_appconfig.moc"
