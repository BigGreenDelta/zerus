#include "KonsoleBackend.h"
#include "ChildProcess.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QThread>
#include <QXmlStreamReader>

#include <algorithm>

namespace {

const QLatin1String kServicePrefix("org.kde.konsole-");
const QLatin1String kWindowIface("org.kde.konsole.Window");
const QLatin1String kSessionIface("org.kde.konsole.Session");
const QLatin1String kWidgetIface("org.qtproject.Qt.QWidget");
const QLatin1String kPropsIface("org.freedesktop.DBus.Properties");
const QLatin1String kIntrospectIface("org.freedesktop.DBus.Introspectable");
const QLatin1String kAppIface("org.freedesktop.Application");

// Клик пользователя ждать секунды не должен, а зависшая Konsole не должна вешать трей.
constexpr int kCallTimeoutMs = 3000;

QDBusMessage callKonsole(const QString &service, const QString &path, const QLatin1String &iface,
                         const QString &method, const QVariantList &args = {})
{
    QDBusMessage msg = QDBusMessage::createMethodCall(service, path, iface, method);
    if (!args.isEmpty())
        msg.setArguments(args);
    return QDBusConnection::sessionBus().call(msg, QDBus::Block, kCallTimeoutMs);
}

bool ok(const QDBusMessage &reply)
{
    return reply.type() == QDBusMessage::ReplyMessage;
}

QString errorText(const QDBusMessage &reply)
{
    return reply.errorName().isEmpty() ? QStringLiteral("no reply")
                                       : reply.errorName() + QStringLiteral(": ") + reply.errorMessage();
}

// Сервисы konsole, отсортированные ЧИСЛЕННО по pid: pid растут монотонно, поэтому
// это «самая старая Konsole первой» -- порядок стабилен между кликами (одна и та же
// цель, пока окна не менялись), в отличие от лексикографической сортировки строк.
QStringList konsoleServices()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return {};
    QDBusConnectionInterface *iface = bus.interface();
    if (iface == nullptr)
        return {};

    const QDBusReply<QStringList> reply = iface->registeredServiceNames();
    if (!reply.isValid())
        return {};

    QStringList out;
    for (const QString &name : reply.value()) {
        if (name.startsWith(kServicePrefix))
            out << name;
    }
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return a.mid(kServicePrefix.size()).toLongLong() < b.mid(kServicePrefix.size()).toLongLong();
    });
    return out;
}

// Дочерние узлы объекта -- у Konsole это и есть перечисление окон и сессий:
// специального метода «дай список окон» на сервисе нет, только интроспекция.
QList<int> childIndices(const QString &service, const QString &path)
{
    const QDBusMessage reply = callKonsole(service, path, kIntrospectIface, QStringLiteral("Introspect"));
    if (!ok(reply) || reply.arguments().isEmpty())
        return {};

    QList<int> out;
    QXmlStreamReader xml(reply.arguments().constFirst().toString());
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("node")) {
            bool isNumber = false;
            const int idx = xml.attributes().value(QLatin1String("name")).toInt(&isNumber);
            if (isNumber)
                out << idx;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

QString windowPath(int index)
{
    return QStringLiteral("/Windows/%1").arg(index);
}

QString sessionPath(int index)
{
    return QStringLiteral("/Sessions/%1").arg(index);
}

QString mainWindowPath(int index)
{
    return QStringLiteral("/konsole/MainWindow_%1").arg(index);
}

// Свойство QWidget у окна (isActiveWindow, minimized, ...). Ошибка -> дефолт.
bool widgetFlag(const QString &service, int windowIndex, const QString &property, bool fallback)
{
    const QDBusMessage reply = callKonsole(service, mainWindowPath(windowIndex), kPropsIface,
                                           QStringLiteral("Get"),
                                           {QString(kWidgetIface), property});
    if (!ok(reply) || reply.arguments().isEmpty())
        return fallback;
    return reply.arguments().constFirst().value<QDBusVariant>().variant().toBool();
}

// Konsole 26.08 держит sendText/runCommand за настройкой «Enable the security sensitive
// parts of the DBus API» (konsolerc: [KonsoleWindow] EnableSecuritySensitiveDBusAPI),
// выключенной по умолчанию: без неё они отвечают AccessDenied «Security sensitive DBus
// API is disabled in the settings». Проверять это НАДО ДО newSession(), иначе в окне
// пользователя останется пустая вкладка, в которую нечего послать.
//
// Проба -- sendText("") на уже существующей сессии: Konsole глотает пустую строку, не
// отправляя терминалу ни байта (проверено вживую: пять проб подряд в сессию с `cat >
// файл` не дали в файле ничего), так что в чужую работающую вкладку это не печатает.
bool inputAllowed(const QString &service)
{
    const QList<int> sessions = childIndices(service, QStringLiteral("/Sessions"));
    if (sessions.isEmpty())
        return false;
    return ok(callKonsole(service, sessionPath(sessions.constFirst()), kSessionIface,
                          QStringLiteral("sendText"), {QString()}));
}

// Попытка показать окно. Гарантий нет и быть не может: на Wayland активацию окна
// выдаёт композитор по xdg-activation токену, а у трея нет ни собственного окна, ни
// токена (Plasma присылает его в ProvideXdgActivationToken только вокруг клика по самой
// иконке, не по пункту меню). Поэтому здесь только то, что безопасно и хоть иногда
// помогает: развернуть свёрнутое (это композитор разрешает без токена) и попросить
// внутрипроцессный подъём.
//
// Чего здесь СОЗНАТЕЛЬНО нет -- org.freedesktop.Application.Activate: на Konsole 26.08
// он не поднимает существующее окно, а открывает НОВОЕ (проверено: один вызов -> +1
// окно и +1 сессия). Для приложения в режиме KDBusService::Multiple это штатное
// поведение «активировать = дай ещё один экземпляр», и для нас оно ровно обратно цели.
void bringForward(const QString &service, int windowIndex)
{
    if (widgetFlag(service, windowIndex, QStringLiteral("minimized"), false))
        callKonsole(service, mainWindowPath(windowIndex), kWidgetIface, QStringLiteral("showNormal"));
    callKonsole(service, mainWindowPath(windowIndex), kWidgetIface, QStringLiteral("raise"));
}

// Стало ли окно активным. Это единственный честный признак «пользователь теперь видит
// эту сессию»: currentSession для этого не годится -- в окне с одной вкладкой она и так
// уже текущая, и проверка выродилась бы в «всегда успех», ничего не показав на экране.
// Композитору дают немного времени: активация асинхронна, Qt узнаёт о ней из configure.
// 120 мс блокируют GUI-поток трея, но только на клике по приаттаченной сессии и только
// когда активации не случилось -- иконка за это время не успевает даже моргнуть.
bool windowIsActive(const QString &service, int windowIndex)
{
    if (widgetFlag(service, windowIndex, QStringLiteral("isActiveWindow"), false))
        return true;
    QThread::msleep(120);
    return widgetFlag(service, windowIndex, QStringLiteral("isActiveWindow"), false);
}

struct WindowRef {
    QString service;
    int index = -1;
    bool valid() const { return index >= 0; }
};

// Куда класть новую вкладку. Активное окно Konsole -- ровно то, на которое пользователь
// сейчас смотрит, и вкладка там наименее неожиданна. Меню трея рисует Plasma, так что в
// момент клика активна панель, а не Konsole, и активного окна обычно НЕТ -- тогда цель
// выбирается детерминированно (самый старый процесс, его первое окно): вкладки всегда
// копятся в одном и том же окне, а не расползаются по случайному.
WindowRef chooseWindow(const QStringList &services)
{
    WindowRef first;
    for (const QString &service : services) {
        for (int index : childIndices(service, QStringLiteral("/Windows"))) {
            if (!first.valid())
                first = WindowRef{service, index};
            if (widgetFlag(service, index, QStringLiteral("isActiveWindow"), false))
                return WindowRef{service, index};
        }
    }
    return first;
}

} // namespace

QString KonsoleBackend::ttyFromDevNumber(unsigned int devNumber)
{
    if (devNumber == 0)
        return {};
    // Кодировка ядра (new_encode_dev): major -- биты 8..19, minor разорван на 0..7 и 20..31.
    const unsigned int major = (devNumber >> 8) & 0xfffu;
    const unsigned int minor = (devNumber & 0xffu) | ((devNumber >> 12) & 0xfff00u);
    // UNIX98 pts живут на major 136..143 по 256 минорных на каждый.
    if (major < 136u || major > 143u)
        return {};
    return QStringLiteral("/dev/pts/%1").arg((major - 136u) * 256u + minor);
}

QString KonsoleBackend::ttyOfPid(int pid)
{
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (stat.open(QIODevice::ReadOnly)) {
        const QByteArray line = stat.readAll();
        // Второе поле -- comm в скобках, и оно может содержать и пробелы, и ')',
        // поэтому разбор начинается с ПОСЛЕДНЕЙ ')', а не со split(' ').
        const int commEnd = line.lastIndexOf(')');
        if (commEnd > 0) {
            const QList<QByteArray> fields = line.mid(commEnd + 2).split(' ');
            // После comm: state, ppid, pgrp, session, tty_nr -- tty_nr пятый.
            if (fields.size() > 4) {
                bool isNumber = false;
                const unsigned int dev = fields.at(4).toUInt(&isNumber);
                if (isNumber) {
                    const QString tty = ttyFromDevNumber(dev);
                    if (!tty.isEmpty())
                        return tty;
                }
            }
        }
    }
    // Запасной путь: stdin процесса. Управляющего терминала может не быть (setsid),
    // но fd 0 у оболочки вкладки указывает на её pts.
    const QString link = QFile::symLinkTarget(QStringLiteral("/proc/%1/fd/0").arg(pid));
    if (link.startsWith(QLatin1String("/dev/pts/")))
        return link;
    return {};
}

QString KonsoleBackend::normalizeTty(const QString &tty)
{
    const QString trimmed = tty.trimmed();
    if (trimmed.isEmpty())
        return {};
    return trimmed.startsWith(QLatin1Char('/')) ? trimmed : QStringLiteral("/dev/") + trimmed;
}

QStringList KonsoleBackend::launchArgs(const QString &command)
{
    // -l обязателен: hgs лежит в ~/.local/bin, который в PATH попадает из профиля.
    // В отличие от вкладки (там команда печатается в уже живую интерактивную оболочку
    // с готовым PATH) новое окно стартует оболочку с нуля.
    return {QStringLiteral("-e"), QStringLiteral("bash"), QStringLiteral("-lc"), command};
}

bool KonsoleBackend::canRaise() const
{
    // «Есть смысл пробовать», а не «получится»: получится или нет, решает композитор, и
    // ответ на это даёт только сам raiseTty() -- он проверяет результат и возвращает
    // false, если окно не поднялось (см. windowIsActive). На этой машине -- Plasma 6 на
    // Wayland, Konsole 26.08, август 2026 -- не поднималось НИ РАЗУ: ни QWidget::raise(),
    // ни showNormal(), ни Application.Activate (он вместо подъёма открывает новое окно),
    // ни org.kde.KWin /WindowsRunner Run по uuid окна не сделали окно активным. Причина
    // системная: xdg-activation выдаёт токен только клиенту с недавним вводом, а трей --
    // это иконка без собственного окна. Поэтому сегодня каждый клик доходит до open().
    // Тем не менее false тут не зашит: измерения снимались при заблокированном экране
    // (kscreenlocker держит фокус, активным не может стать вообще никакое окно), и
    // намертво выключать ветку по таким данным было бы неправильно -- пусть решает
    // проверка в рантайме.
    return !konsoleServices().isEmpty();
}

KonsoleBackend::KonsoleBackend(OpenMode mode) : m_mode(mode)
{
}

QString KonsoleBackend::takeAdvisory()
{
    const QString result = m_advisory;
    m_advisory.clear();
    return result;
}

bool KonsoleBackend::open(const QString &command, QString *error)
{
    // Каждый open() отвечает только за СВОЙ исход -- пояснение от прошлого клика (если
    // вызывающий его не забрал) сюда не протекает.
    m_advisory.clear();

    auto launchWindow = [&]() -> bool {
        if (!ChildProcess::startDetached(QStringLiteral("konsole"), launchArgs(command), QDir::homePath())) {
            *error = QStringLiteral("cannot start konsole (not in PATH?)");
            return false;
        }
        return true;
    };

    if (m_mode == OpenMode::Window) {
        // Пользователь явно попросил всегда окно -- ниже по функции сплошь D-Bus
        // (konsoleServices()/inputAllowed()), и в этом режиме к нему не обращаются
        // вовсе: ни одного вызова sessionBus() для этого клика.
        return launchWindow();
    }

    // Вкладка возможна только там, где Konsole разрешает runCommand; иначе окно.
    QStringList usable;
    for (const QString &service : konsoleServices()) {
        if (inputAllowed(service))
            usable << service;
    }
    if (usable.isEmpty()) {
        if (m_mode == OpenMode::Tab) {
            const bool opened = launchWindow();
            // Пояснение -- только если окно и правда открылось (иначе `*error` уже
            // говорит "не открылось", и второе, противоречащее сообщение не нужно) и
            // только один раз за прогон трея, см. m_tabAdvisoryShown в .h.
            if (opened && !m_tabAdvisoryShown) {
                m_tabAdvisoryShown = true;
                m_advisory = QStringLiteral(
                    "Konsole will not hand out a tab over D-Bus -- the API is off in "
                    "its settings. To make a click open a tab instead of a window: add "
                    "EnableSecuritySensitiveDBusAPI=true under [KonsoleWindow] in %1 (or "
                    "Settings -> Configure Konsole -> \"Enable the security sensitive "
                    "parts of the DBus API\") and restart Konsole. The session was opened "
                    "in a separate window.")
                    .arg(QDir::homePath() + QStringLiteral("/.config/konsolerc"));
            }
            return opened;
        }
        // auto: тихий откат на окно, как было -- лог остаётся тем же уведомлением, что
        // и до этой задачи, только в stderr, не балуном (auto не должен ничего просить
        // у пользователя).
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning("hgs-tray: Konsole will not take runCommand over D-Bus -- opening a separate "
                     "window. A tab instead of a window is enabled by Settings -> Configure "
                     "Konsole -> \"Enable the security sensitive parts of the DBus API\".");
        }
        return launchWindow();
    }

    const WindowRef target = chooseWindow(usable);
    if (!target.valid())
        return launchWindow();

    const QDBusMessage created = callKonsole(target.service, windowPath(target.index), kWindowIface,
                                             QStringLiteral("newSession"));
    if (!ok(created) || created.arguments().isEmpty()) {
        // Окно могло закрыться между перечислением и вызовом -- это не ошибка
        // пользователя, просто открываем новое.
        return launchWindow();
    }
    const int sessionId = created.arguments().constFirst().toInt();

    const QDBusMessage ran = callKonsole(target.service, sessionPath(sessionId), kSessionIface,
                                         QStringLiteral("runCommand"), {command});
    if (!ok(ran)) {
        // Вкладка уже создана и в ней живая оболочка -- второе окно поверх неё было бы
        // лишним; отдаём ошибку, чтобы трей показал команду, которую можно вставить
        // прямо в эту вкладку.
        *error = QStringLiteral("Konsole rejected the command (%1)").arg(errorText(ran));
        return false;
    }

    // Вкладка создана и команда в неё уехала -- дальше только попытка показать окно;
    // её провал ничего не отменяет, вкладка уже есть и второй раз открывать нечего.
    callKonsole(target.service, windowPath(target.index), kWindowIface,
                QStringLiteral("setCurrentSession"), {sessionId});
    bringForward(target.service, target.index);
    return true;
}

bool KonsoleBackend::raiseTty(const QString &tty, QString *error)
{
    const QString wanted = normalizeTty(tty);
    if (wanted.isEmpty()) {
        *error = QStringLiteral("empty tty");
        return false;
    }

    for (const QString &service : konsoleServices()) {
        for (int sessionId : childIndices(service, QStringLiteral("/Sessions"))) {
            const QDBusMessage pidReply = callKonsole(service, sessionPath(sessionId), kSessionIface,
                                                      QStringLiteral("processId"));
            if (!ok(pidReply) || pidReply.arguments().isEmpty())
                continue;
            // processId() -- лидер сессии вкладки, то есть её оболочка. Клиент tmux
            // запускается ИЗ этой оболочки и наследует её pts, поэтому tty, который
            // отдаёт `tmux list-clients` (и, значит, hgs в clients[]), совпадает с tty
            // оболочки -- проверено на живой сессии, спускаться по детям не нужно.
            if (ttyOfPid(pidReply.arguments().constFirst().toInt()) != wanted)
                continue;

            for (int windowIndex : childIndices(service, QStringLiteral("/Windows"))) {
                const QDBusMessage list = callKonsole(service, windowPath(windowIndex), kWindowIface,
                                                      QStringLiteral("sessionList"));
                if (!ok(list) || list.arguments().isEmpty())
                    continue;
                if (!list.arguments().constFirst().toStringList().contains(QString::number(sessionId)))
                    continue;

                // setCurrentSession на Konsole 26.08 -- пустышка: ни он, ни
                // setCurrentView, ни действия next-tab/switch-to-tab-N не меняют
                // текущую вкладку (проверено на двух окнах двумя независимыми
                // признаками -- currentSession и windowTitle; при этом moveSessionLeft/
                // Right на том же окне работают, то есть адаптор жив). Вызов оставлен:
                // он корректен по API и заработает сам, если Konsole починят.
                callKonsole(service, windowPath(windowIndex), kWindowIface,
                            QStringLiteral("setCurrentSession"), {sessionId});
                bringForward(service, windowIndex);
                // Успех = окно РЕАЛЬНО поднялось. Если композитор отказал, честнее
                // вернуть false и дать вызывающему открыть вкладку, чем отчитаться об
                // успехе, после которого на экране ничего не изменилось.
                if (windowIsActive(service, windowIndex))
                    return true;
                *error = QStringLiteral("found the window with %1, but the compositor "
                                         "would not raise it").arg(wanted);
                return false;
            }
        }
    }

    *error = QStringLiteral("no Konsole tab with %1").arg(wanted);
    return false;
}
