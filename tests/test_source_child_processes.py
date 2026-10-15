"""Exercise the desktop child process rule of scripts/ci/check-source.py."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_source", ROOT / "scripts/ci/check-source.py")
check = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check)

NAME = "tray/src/Launcher.cpp"
UNCONFIGURED = ("QProcess {} is not passed to ChildProcess::inheritStdioOnly() in the function that creates it "
                "(tray/src/ChildProcess.h).")


def findings(text, name=NAME, others=None):
    return check.child_process_findings({name: text, **(others or {})})


def function(*lines):
    return "#include \"ChildProcess.h\"\n\nvoid Launcher::run()\n{\n" + "".join(f"    {line}\n" for line in lines) + "}\n"


class CurrentTreeTest(unittest.TestCase):
    def test_every_desktop_child_starts_through_child_process(self):
        files = {path.relative_to(ROOT).as_posix(): path.read_text(encoding="utf-8")
                 for path in (ROOT / "tray/src").rglob("*") if path.suffix in {".cpp", ".h", ".mm"}}
        self.assertEqual(check.child_process_findings(files), [])
        self.assertLessEqual(set(check.CHILD_PROCESS_EXEMPT), set(files), "remove exemptions of deleted files")


class StaticStartTest(unittest.TestCase):
    def test_static_starts_name_the_file_line_and_child_process(self):
        for name in (NAME, "tray/src/Launcher.h", "tray/src/MacLauncher.mm", "tray/src/speech/Tool.cpp"):
            for method in ("startDetached", "execute"):
                with self.subTest(name=name, method=method):
                    text = function(f"QProcess::{method}(QStringLiteral(\"xdg-open\"), {{m_url}});")
                    self.assertEqual(findings(text, name), [
                        f"{name}:5: QProcess::{method}() bypasses ChildProcess::inheritStdioOnly(); use "
                        "ChildProcess::startDetached() or a QProcess passed to inheritStdioOnly() (tray/src/ChildProcess.h)."])

    def test_exempt_files_and_other_sources_may_start_children(self):
        text = function("QProcess::startDetached(m_terminal, m_arguments);", "QProcess process;", "process.startDetached();")
        for name in (*check.CHILD_PROCESS_EXEMPT, "tray/tests/test_launcher.cpp", "tray/src/notes.md", "src/launcher.cpp"):
            with self.subTest(name=name):
                self.assertEqual(findings(text, name), [])

    def test_comments_and_literals_are_not_code(self):
        text = function(
            "// QProcess::startDetached(program, arguments) would hand the child our pipes.",
            "/* auto *process = new QProcess(this); process->start(); */",
            "const char *usage = \"QProcess::execute(\\\"tool\\\")\";",
            "const QString script = QStringLiteral(R\"js(QProcess::startDetached(\"a\"); // \")js\");",
            "const int limit = 1'000'000; const QChar quote = QLatin1Char('\"');",
            "ChildProcess::startDetached(QStringLiteral(\"open\"), {QStringLiteral(\"-R\"), m_path});")
        self.assertEqual(findings(text), [])


class ProcessObjectTest(unittest.TestCase):
    def test_a_process_passed_to_inherit_stdio_only_may_start(self):
        for lines in (
            ("auto *process = new QProcess(this);", "process->setProgram(m_tool);",
             "ChildProcess::inheritStdioOnly(*process);", "process->start();"),
            ("QProcess process;", "ChildProcess::inheritStdioOnly(process);", "process.startDetached();"),
            ("auto *process = m_process = new QProcess(this);", "ChildProcess::inheritStdioOnly(*m_process);",
             "connect(process, &QProcess::finished, this, [process] { process->deleteLater(); });", "process->start();"),
            ("auto process = std::make_unique<QProcess>();", "ChildProcess::inheritStdioOnly(*process);",
             "process->start(m_tool, {});"),
            ("m_process = new QProcess(this);", "if (m_ready) {", "    ChildProcess::inheritStdioOnly(*this->m_process);",
             "    QTimer::singleShot(0, this, [this] { m_process->start(); });", "}"),
        ):
            with self.subTest(lines=lines):
                self.assertEqual(findings(function(*lines)), [])

    def test_a_process_without_inherit_stdio_only_fails_where_it_is_created(self):
        for lines, variable in (
            (("auto *process = new QProcess(this);", "process->start(m_tool, {});"), "process"),
            (("QProcess probe;", "probe.start(m_tool, {});", "probe.waitForFinished();"), "probe"),
            (("m_process.reset(new QProcess);", "m_process->startDetached();"), "m_process"),
            (("auto *process = new QProcess(this);", "// ChildProcess::inheritStdioOnly(*process);",
              "process->open();"), "process"),
        ):
            with self.subTest(lines=lines):
                self.assertEqual(findings(function(*lines)), [f"{NAME}:5: " + UNCONFIGURED.format(variable)])

    def test_inherit_stdio_only_must_come_before_the_start(self):
        text = function("auto *process = new QProcess(this);", "process->start();", "ChildProcess::inheritStdioOnly(*process);")
        self.assertEqual(findings(text), [f"{NAME}:6: process->start() comes before "
                                          "ChildProcess::inheritStdioOnly(process) (tray/src/ChildProcess.h)."])

    def test_inherit_stdio_only_belongs_to_the_function_that_creates_the_process(self):
        configured = "void Launcher::first()\n{\n    QProcess process;\n    ChildProcess::inheritStdioOnly(process);\n    process.startDetached();\n}\n"
        unconfigured = "void Launcher::second()\n{\n    QProcess process;\n    process.startDetached();\n}\n"
        self.assertEqual(findings(configured + unconfigured), [f"{NAME}:9: " + UNCONFIGURED.format("process")])
        later = "void Launcher::configure()\n{\n    ChildProcess::inheritStdioOnly(*m_process);\n}\n"
        self.assertEqual(findings(function("m_process = new QProcess(this);") + later),
                         [f"{NAME}:5: " + UNCONFIGURED.format("m_process")])

    def test_a_function_that_passes_its_parameter_to_inherit_stdio_only_configures_it(self):
        helper = ("void Tools::prepare(const QString &program, QProcess *child)\n{\n"
                  "    child->setProgram(program);\n    ChildProcess::inheritStdioOnly(*child);\n}\n")
        text = function("auto *process = new QProcess(this);", "Tools::prepare(m_tool, process);", "process->start();")
        self.assertEqual(findings(text, others={"tray/src/Tools.cpp": helper}), [])
        self.assertEqual(findings(text, others={"tray/src/Tools.cpp": helper.replace("ChildProcess::inheritStdioOnly(*child);", "")}),
                         [f"{NAME}:5: " + UNCONFIGURED.format("process")])
        self.assertEqual(findings(text.replace("prepare(m_tool, process)", "prepare(process, m_tool)"),
                                  others={"tray/src/Tools.cpp": helper}), [f"{NAME}:5: " + UNCONFIGURED.format("process")])

    def test_a_member_may_be_configured_by_its_class_sources(self):
        header = "#pragma once\n#include <QProcess>\n\nclass Tool : public QObject\n{\n    QProcess m_process;\n};\n"
        source = ("Tool::Tool(QObject *parent) : QObject(parent)\n{\n    ChildProcess::inheritStdioOnly(m_process);\n}\n"
                  "void Tool::run() { m_process.start(); }\n")
        self.assertEqual(findings(header, "tray/src/Tool.h", {"tray/src/Tool.cpp": source}), [])
        self.assertEqual(findings(header, "tray/src/Tool.h", {"tray/src/Tool.cpp": source.replace("Child", "// Child"),
                                                              "tray/src/OtherTool.cpp": source}),
                         ["tray/src/Tool.h:6: " + UNCONFIGURED.format("m_process")])
        initialized = "Tool::Tool() : m_process(new QProcess(this))\n{\n    ChildProcess::inheritStdioOnly(*m_process);\n}\n"
        self.assertEqual(findings(initialized), [])
        self.assertEqual(findings(initialized.replace("ChildProcess::", "// ")), [f"{NAME}:1: " + UNCONFIGURED.format("m_process")])

    def test_processes_the_check_cannot_follow_fail(self):
        unnamed = ("assign the new QProcess to a variable and pass it to ChildProcess::inheritStdioOnly() "
                   "(tray/src/ChildProcess.h).")
        for text, expected in (
            (function("m_children.append(new QProcess(this));"), f"{NAME}:5: {unnamed}"),
            ("QProcess *Launcher::make()\n{\n    return new QProcess(this);\n}\n", f"{NAME}:3: {unnamed}"),
            ("class Tool : public QProcess\n{\n};\n", f"{NAME}:1: a QProcess subclass starts children this check "
                                                    "cannot follow (tray/src/ChildProcess.h)."),
            ("using Child = QProcess;\n" + function("auto *child = new Child(this);", "child->start();"),
             f"{NAME}:6: " + UNCONFIGURED.format("child")),
        ):
            with self.subTest(text=text):
                self.assertEqual(findings(text), [expected])


if __name__ == "__main__":
    unittest.main()
