#include "ClipboardBackend.h"
#include "ChildProcess.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QProcess>
#include <QStringList>

#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#endif

namespace {

// Тот же таймаут и по той же причине, что в KonsoleBackend: вызов происходит по клику
// пользователя, и зависший собеседник не должен вешать трей насовсем.
constexpr int kCallTimeoutMs = 3000;

// Порог, после которого команда в уведомлении сокращается. См. комментарий у
// notificationText() в .h -- почему именно середина и почему порог такой большой.
constexpr int kMaxNotificationChars = 120;

bool copyViaKlipper(const QString &text, QString *error)
{
#ifdef Q_OS_LINUX
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        *error = QStringLiteral("no D-Bus session bus");
        return false;
    }
    // Имя ИНТЕРФЕЙСА -- org.kde.klipper.klipper, а не org.kde.klipper: последнее -- имя
    // сервиса, и они не совпадают. Ручная проверка через `qdbus6 org.kde.klipper /klipper
    // setClipboardContents ...` этой разницы не показывает -- qdbus сам ищет метод по
    // всем интерфейсам объекта, -- а вот прямой вызов с неверным интерфейсом молча
    // проваливается в UnknownMethod (поймано вживую: буфер писался, но уже запасным
    // wl-copy). Проверено интроспекцией: gdbus introspect -d org.kde.klipper -o /klipper.
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.klipper"), QStringLiteral("/klipper"),
        QStringLiteral("org.kde.klipper.klipper"), QStringLiteral("setClipboardContents"));
    msg.setArguments({text});
    const QDBusMessage reply = bus.call(msg, QDBus::Block, kCallTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        // Самый частый случай -- ServiceUnknown: klipper живёт внутри plasmashell, то есть
        // его нет ни на не-Plasma сессии, ни пока plasmashell перезапускается.
        *error = reply.errorName().isEmpty()
                     ? QStringLiteral("no reply")
                     : reply.errorName() + QStringLiteral(": ") + reply.errorMessage();
        return false;
    }
    // Читать обратно для проверки не будем: это второй круг по шине на каждый клик ради
    // случая, которого у setClipboardContents нет -- она либо доехала, либо вернула ошибку.
    return true;
#else
    Q_UNUSED(text);
    *error = QStringLiteral("klipper is Linux-only");
    return false;
#endif
}

bool copyViaWlCopy(const QString &text, QString *error)
{
    QProcess p;
    // Команда уходит ОДНИМ аргументом, а не набором слов: wl-copy склеивает несколько
    // аргументов через пробел, и имя сессии с двумя пробелами подряд после такой склейки
    // вернулось бы изменённым. Явный -t text/plain -- чтобы wl-copy не угадывал тип по
    // содержимому. Ведущий '-' в тексте wl-copy принял бы за опцию, но команда трея
    // всегда начинается с имени бинаря hgs (или его пути), и даже в невозможном случае
    // это не порча содержимого, а ненулевой код возврата и переход к следующему способу.
    const QStringList args{QStringLiteral("-t"), QStringLiteral("text/plain"), text};

    // Каналы в /dev/null, а не в трубы: wl-copy по умолчанию форкается в фон, и
    // фоновый потомок унаследовал бы трубы -- waitForFinished() ждал бы тогда не выхода
    // самого wl-copy, а закрытия труб демоном, то есть до следующей смены буфера.
    p.setStandardOutputFile(QProcess::nullDevice());
    p.setStandardErrorFile(QProcess::nullDevice());
    p.setWorkingDirectory(QDir::homePath());
    // The background owner it forks lives on until the clipboard changes.
    ChildProcess::inheritStdioOnly(p);
    p.start(QStringLiteral("wl-copy"), args);
    if (!p.waitForStarted(kCallTimeoutMs)) {
        *error = QStringLiteral("cannot start wl-copy (not in PATH?)");
        return false;
    }
    // Ждём именно переднего процесса: он форкает демона-донора и выходит сразу, поэтому
    // ждать тут нечего и код возврата у нас настоящий (в отличие от startDetached, где
    // видно только «exec удался»: wl-copy без WAYLAND_DISPLAY отработал бы «успешно», а
    // буфер остался бы старым -- и трей соврал бы «скопировано»).
    if (!p.waitForFinished(kCallTimeoutMs)) {
        p.kill();
        p.waitForFinished(kCallTimeoutMs);
        *error = QStringLiteral("wl-copy did not answer in %1 ms").arg(kCallTimeoutMs);
        return false;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        *error = QStringLiteral("wl-copy exited with code %1").arg(p.exitCode());
        return false;
    }
    // Демон-донор остаётся жить -- и это не утечка, а то, КАК устроен буфер на Wayland:
    // содержимое отдаёт клиент-владелец, и умри он, буфер опустеет. Копий не копится:
    // следующий владелец (наш же следующий wl-copy, klipper, любое окно с Ctrl+C)
    // забирает selection, и прошлый wl-copy по событию отмены выходит сам -- проверено
    // на этой машине: после двух копирований подряд в системе ровно один wl-copy, с
    // новым pid. Прибивать его самим нельзя: это стёрло бы то, что только что скопировали.
    return true;
}

bool copyViaQt(const QString &text, QString *error)
{
    // Без QGuiApplication буфера у Qt нет вовсе (а обращение к нему -- qFatal). В трее
    // приложение есть всегда, но проверка бесплатная и делает функцию безопасной для
    // вызова откуда угодно, включая тест без GUI.
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()) == nullptr) {
        *error = QStringLiteral("no GUI application");
        return false;
    }
    QClipboard *cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        *error = QStringLiteral("the application has no clipboard");
        return false;
    }
    cb->setText(text, QClipboard::Clipboard);
    // Читаем обратно -- здесь это не паранойя: на Wayland отказ композитора выглядит
    // ровно как успех (setText() ничего не возвращает и не может), и без проверки трей
    // показал бы «скопировано» на пустом месте. Единственный способ узнать -- посмотреть,
    // что в буфере на самом деле.
    if (cb->text(QClipboard::Clipboard) != text) {
        *error = QStringLiteral("the clipboard write did not stick");
        return false;
    }
    return true;
}

bool copyWith(ClipboardBackend::Method m, const QString &text, QString *error)
{
    switch (m) {
    case ClipboardBackend::Method::Klipper:
        return copyViaKlipper(text, error);
    case ClipboardBackend::Method::WlCopy:
        return copyViaWlCopy(text, error);
    case ClipboardBackend::Method::QtClipboard:
        return copyViaQt(text, error);
    }
    *error = QStringLiteral("unknown copy method");
    return false;
}

} // namespace

QVector<ClipboardBackend::Method> ClipboardBackend::methodChain(Platform platform)
{
    switch (platform) {
    case Platform::Linux:
        // klipper первым: он единственный не упирается в требование Wayland «serial от
        // своего недавнего ввода» (см. Method::Klipper). wl-copy -- для Wayland без
        // Plasma. QClipboard последним: на X11 он сработает первым же делом, но проверять
        // «а не X11 ли у нас» отдельно незачем -- на X11 klipper либо есть и тоже
        // работает, либо его нет и мы просто дойдём до Qt.
        return {Method::Klipper, Method::WlCopy, Method::QtClipboard};
    case Platform::Mac:
        // На маке ограничений Wayland нет вовсе: NSPasteboard пишется кем угодно и когда
        // угодно, QClipboard поверх неё работает и из фонового LSUIElement-приложения.
        // Ни klipper'а, ни wl-copy там не существует -- пробовать их значило бы гонять
        // заведомо мёртвые ветки на каждый клик.
        return {Method::QtClipboard};
    }
    return {Method::QtClipboard};
}

ClipboardBackend::Platform ClipboardBackend::currentPlatform()
{
#ifdef Q_OS_LINUX
    return Platform::Linux;
#else
    return Platform::Mac;
#endif
}

QString ClipboardBackend::methodName(Method m)
{
    switch (m) {
    case Method::Klipper:
        return QStringLiteral("klipper");
    case Method::WlCopy:
        return QStringLiteral("wl-copy");
    case Method::QtClipboard:
        return QStringLiteral("QClipboard");
    }
    return QStringLiteral("?");
}

bool ClipboardBackend::isCopyable(const QString &command, QString *error)
{
    if (command.trimmed().isEmpty()) {
        *error = QStringLiteral("nothing to copy: the command is empty");
        return false;
    }
    // Отказываем, а не вычищаем молча. Причин две. Первая: вставка команды с переводом
    // строки внутри означает, что оболочка её ВЫПОЛНИТ -- в той вкладке, куда вставили,
    // возможно, поверх работающего агента; весь смысл этого режима в том, что решение
    // «выполнять» остаётся за пользователем. Вторая: обрезав команду тихо, трей положил
    // бы в буфер не то, что показал в уведомлении, -- расхождение хуже отказа. Сегодня
    // сюда такого не приходит (трей строит "hgs a <имя>" и "hgs <cmd> <проект>", а имя
    // сессии и имя проекта живут в построчных форматах, где перевод строки не выживает),
    // так что это растяжка на будущее: сломается инвариант -- будет видно сразу и громко.
    if (command.contains(QLatin1Char('\n')) || command.contains(QLatin1Char('\r'))) {
        *error = QStringLiteral("the command has a newline in it -- copying that is "
                                "dangerous: the shell would run it the moment you paste");
        return false;
    }
    return true;
}

QString ClipboardBackend::notificationText(const QString &command)
{
    if (command.size() <= kMaxNotificationChars)
        return command;
    // Многоточие в середине: голова говорит, на каком боксе сессия, хвост -- какая именно.
    const QString ellipsis = QStringLiteral(" … ");
    const int keep = kMaxNotificationChars - ellipsis.size();
    const int head = keep / 2;
    const int tail = keep - head;
    return command.left(head) + ellipsis + command.right(tail);
}

QString ClipboardBackend::advisoryTitle() const
{
    // Дефолтный заголовок канала ("session opened, but not the way you asked") здесь был бы
    // прямой ложью: ничего не открылось и не должно было.
    return QStringLiteral("Zerus: copied to the clipboard");
}

QString ClipboardBackend::takeAdvisory()
{
    const QString result = m_advisory;
    m_advisory.clear();
    return result;
}

bool ClipboardBackend::open(const QString &command, QString *error)
{
    m_advisory.clear();

    if (!isCopyable(command, error))
        return false;

    QStringList failures;
    const QVector<Method> chain = methodChain(currentPlatform());
    for (Method m : chain) {
        QString why;
        if (copyWith(m, command, &why)) {
            // Подтверждение на КАЖДОЕ копирование, а не один раз за прогон: клик в этом
            // режиме не даёт никакой другой обратной связи -- окно не появляется,
            // вкладка не мигает, -- и без уведомления пользователь не отличит
            // «скопировалось» от «трей меня не услышал».
            m_advisory = notificationText(command);
            return true;
        }
        failures << methodName(m) + QStringLiteral(": ") + why;
    }

    // Перечисляем ВСЕ провалившиеся способы: «не удалось скопировать» без деталей на
    // чужом десктопе (не Plasma, без wl-clipboard) не даёт ни одной зацепки, что чинить.
    *error = QStringLiteral("cannot put the command on the clipboard (%1)")
                 .arg(failures.join(QStringLiteral("; ")));
    return false;
}
