#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include "AppConfig.h"

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
    // И список текущей платформы -- один из этих двух, а не что-то третье.
    QVERIFY(AppConfig::openModes(AppConfig::currentPlatform())
            == AppConfig::openModes(AppConfig::Platform::Linux)
            || AppConfig::openModes(AppConfig::currentPlatform())
                   == AppConfig::openModes(AppConfig::Platform::Mac));
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
    for (AppConfig::Platform p : {AppConfig::Platform::Linux, AppConfig::Platform::Mac}) {
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
    // Same order in which hgs extends PATH for its own children (src/platform.rs).
    QCOMPARE(AppConfig::hgsInstallDirs(QStringLiteral("/home/example")),
             (QStringList{QStringLiteral("/home/example/.local/bin"), QStringLiteral("/opt/homebrew/bin"),
                          QStringLiteral("/usr/local/bin")}));
}

void TestAppConfig::fallbackHgsKeepsPathLookupWhenPathProvidesHgs()
{
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
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QCOMPARE(AppConfig::fallbackHgsPath(dir.path(), {dir.filePath(QStringLiteral("missing"))}),
             QString());
}

QTEST_APPLESS_MAIN(TestAppConfig)
#include "test_appconfig.moc"
