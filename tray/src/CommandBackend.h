#pragma once

#include "TerminalBackend.h"

#include <QString>
#include <QStringList>

#include <functional>

class QProcess;

// Запасной бэкенд: шаблон команды из ~/.config/hgs/tray.conf (`terminal=`) или из
// --terminal. Ничего не знает ни про Konsole, ни про D-Bus, поэтому годится и как
// «терминал, который выбрал пользователь», и как заглушка на платформе без бэкенда.
// Поднимать окно не умеет: узнать, где открыт tty, через запуск процесса нельзя.
class CommandBackend : public TerminalBackend {
public:
    explicit CommandBackend(QString terminalTemplate);

    bool open(const QString &command, QString *error) override;

    // Чистая функция — проверяется тестом без десктопа.
    //
    // Шаблон СНАЧАЛА режется на аргументы и только потом в каждом из них {cmd}
    // заменяется на команду. Порядок именно такой, чтобы команда всегда оставалась
    // ровно одним аргументом: имя сессии приходит из имени каталога проекта и может
    // содержать пробелы и кавычки, а склейка «подставить в строку, потом разрезать»
    // на таком имени разъедется (и это была бы дыра ровно того класса, что и
    // shell-инъекция). Плата за это -- шаблон обязан заканчиваться тем, что принимает
    // командную строку одним аргументом: `foot -e bash -lc {cmd}`, а не `xterm -e {cmd}`.
    static QStringList buildArgv(const QString &terminalTemplate, const QString &command,
                                 QString *error);

    // The detached start open() performs, without starting it: argv's first
    // element is the program, the folder is home. On Windows a console program
    // (powershell.exe -NoExit -Command {cmd}) gets a console window of its own
    // instead of the hidden console QProcess gives children of a GUI process.
    static void prepare(QProcess &process, QStringList argv);

    // Template for the desktop's default terminal, used on Linux when Konsole is not
    // installed and no terminal= is configured. Pure: isInstalled answers whether a
    // program is in PATH. Empty when no standard launcher is available.
    static QString systemTerminalTemplate(const std::function<bool(const QString &)> &isInstalled);

private:
    QString m_template;
};
