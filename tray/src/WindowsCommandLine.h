#pragma once

#include <QString>
#include <QStringList>

// A CreateProcess command line built exactly as QProcess builds it on Windows
// (qt_create_commandline in qprocess_win.cpp). A child started through a
// pseudoconsole then parses the same argv as one started by QProcess: the MSVC
// runtime rules, which Rust's std::env::args uses as well. Pure, so it is tested
// on every platform.
namespace WindowsCommandLine {

inline QString argument(QString value)
{
    // Quotes are escaped and their preceding backslashes are doubled.
    qsizetype index = value.indexOf(QLatin1Char('"'));
    while (index >= 0) {
        value.insert(index++, QLatin1Char('\\'));
        for (qsizetype i = index - 2; i >= 0 && value.at(i) == QLatin1Char('\\'); --i) {
            value.insert(i, QLatin1Char('\\'));
            ++index;
        }
        index = value.indexOf(QLatin1Char('"'), index + 1);
    }
    if (value.isEmpty() || value.contains(QLatin1Char(' ')) || value.contains(QLatin1Char('\t'))) {
        // A trailing backslash would escape the closing quote: "foo"\ rather than "foo\".
        qsizetype i = value.size();
        while (i > 0 && value.at(i - 1) == QLatin1Char('\\'))
            --i;
        value.insert(i, QLatin1Char('"'));
        value.prepend(QLatin1Char('"'));
    }
    return value;
}

inline QString build(const QString &program, const QStringList &arguments)
{
    QString line;
    if (!program.isEmpty()) {
        QString name = program;
        if (!name.startsWith(QLatin1Char('"')) && !name.endsWith(QLatin1Char('"'))
            && name.contains(QLatin1Char(' ')))
            name = QLatin1Char('"') + name + QLatin1Char('"');
        name.replace(QLatin1Char('/'), QLatin1Char('\\'));
        line = name + QLatin1Char(' ');
    }
    for (const QString &value : arguments)
        line += QLatin1Char(' ') + argument(value);
    return line;
}

} // namespace WindowsCommandLine
