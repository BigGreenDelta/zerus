#include "MacBackend.h"
#include "ChildProcess.h"

#include <QDir>

namespace {

// Строка внутри AppleScript-литерала: экранируются три символа. Порядок ОБЯЗАТЕЛЕН --
// сначала обратный слэш, потом кавычка и перевод строки: иначе слэш, добавленный перед
// ними, будет удвоен на следующем проходе и экранирование съест само себя.
//
// Перевод строки нужен из-за текста уведомлений (notificationScript): в тексте балуна
// переносы есть, а СЫРОЙ перевод строки внутри литерала AppleScript -- синтаксическая
// ошибка, скрипт целиком не выполнится. Команде для `do script` переносы не свойственны,
// но и там замена безвредна.
QString appleScriptEscape(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    out.replace(QLatin1Char('"'), QLatin1String("\\\""));
    out.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    return out;
}

} // namespace

QString MacBackend::terminalAppScript(const QString &command)
{
    // activate отдельной строкой: без него окно от `do script` открывается ПОЗАДИ всех
    // остальных (проверено в TrustTunnelAgent, src/TrayAgent.cpp, onViewLogs).
    return QStringLiteral("tell application \"Terminal\"\n"
                          "    do script \"%1\"\n"
                          "    activate\n"
                          "end tell")
        .arg(appleScriptEscape(command));
}

QString MacBackend::notificationScript(const QString &title, const QString &body)
{
    // Порядок аргументов -- как в словаре Standard Additions: сначала текст, потом
    // заголовок. Обе подстановки экранируются (см. appleScriptEscape), поэтому ни
    // кавычка в имени сессии, ни перенос строки в тексте ошибки скрипт не ломают.
    return QStringLiteral("display notification \"%1\" with title \"%2\"")
        .arg(appleScriptEscape(body), appleScriptEscape(title));
}

bool MacBackend::open(const QString &command, QString *error)
{
    // Ветки здесь нет и быть не может: режим на маке один (см. врезку в .h про удалённый
    // Tabby), а "clipboard" до бэкенда не доходит вовсе -- его перехватывает
    // TrayAgent::makeTerminal(). takeAdvisory() не переопределяется: расхождению между
    // обещанным и сделанным здесь взяться неоткуда, и дефолт из TerminalBackend (пустая
    // строка = «открылось ровно так, как просили») говорит правду.
    //
    // startDetached() отвечает только за то, что osascript ЗАПУЩЕН; выполнился ли сам
    // AppleScript, отсюда не видно и видно быть не может. Это осознанно: при сломанном
    // скрипте окно просто не появится, и это заметнее любого балуна, а ждать здесь
    // завершения osascript значило бы блокировать GUI-поток трея на клике. Скрипт при
    // этом собирается из ровно одной подстановки, которая экранируется (см.
    // appleScriptEscape), -- синтаксически сломать его входными данными нельзя.
    if (!ChildProcess::startDetached(QStringLiteral("osascript"),
                                     {QStringLiteral("-e"), terminalAppScript(command)},
                                     QDir::homePath())) {
        *error = QStringLiteral("cannot start osascript");
        return false;
    }
    return true;
}
