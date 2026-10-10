#pragma once

#include <QString>

// Терминал — сменная деталь. open() обязателен, raise() — нет: у Konsole есть D-Bus и
// вкладку можно адресовать, у Terminal.app на маке нет ни того ни другого (AppleScript
// не умеет адресовать чужую вкладку), и клик по уже открытой сессии там честно откроет
// второе окно на тот же экран.
class TerminalBackend {
public:
    virtual ~TerminalBackend() = default;
    // Запустить команду в новой вкладке или окне. false — не получилось.
    virtual bool open(const QString &command, QString *error) = 0;
    virtual bool canRaise() const { return false; }
    // Поднять и сфокусировать окно, в котором открыт этот tty. false — не нашли.
    virtual bool raiseTty(const QString &tty, QString *error) { Q_UNUSED(tty); Q_UNUSED(error); return false; }

    // Вызывать сразу после open()==true. Бэкенд мог выполнить желание пользователя не
    // буквально (например, обещали вкладку, а дали окно, потому что вкладка была
    // недоступна) -- пустая строка значит "открылось ровно так, как просили", непустая
    // объясняет расхождение и одноразовая: второй вызов подряд без нового open() снова
    // вернёт пусто. Это отдельный канал, а не перегрузка error: error значит "не
    // открылось вообще", а это сообщение — "открылось, но не совсем то". CommandBackend
    // такого не бывает, дефолт здесь его и покрывает без переопределения.
    virtual QString takeAdvisory() { return QString(); }

    // Заголовок уведомления для takeAdvisory(). Дефолт -- единственный случай, который у
    // этого канала был изначально: команда выполнена, но не тем способом, который просили
    // (вкладка вместо окна и т.п.). ClipboardBackend его переопределяет, потому что там
    // не открывается ВООБЩЕ ничего, и «сессия открыта» было бы прямой ложью в первой же
    // строке балуна. Виртуальный метод, а не dynamic_cast в TrayAgent: трей сознательно
    // не знает, какой бэкенд у него внутри (см. m_terminal в TrayAgent.h).
    virtual QString advisoryTitle() const
    {
        return QStringLiteral("Zerus: session opened, but not the way you asked");
    }
};
