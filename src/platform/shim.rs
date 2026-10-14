//! npm's Windows command shims (spec D8). `npm install -g` puts `claude.cmd` and
//! `codex.cmd` on PATH: batch files that run Node.js on the package's script.
//! hgs never starts an agent through `cmd.exe /c`, which breaks arguments that
//! span lines and which endpoint protection scores as obfuscation when the line
//! carries a prompt. It reads the shim instead and starts what the shim would,
//! but only when the file is exactly the shape npm's `cmd-shim` (npm 7.6 and
//! later) writes, all or nothing: any other batch file is refused.
//!
//! A shim for a script whose shebang names `node` without arguments or
//! variables:
//!
//! ```text
//! @ECHO off
//! GOTO start
//! :find_dp0
//! SET dp0=%~dp0
//! EXIT /b
//! :start
//! SETLOCAL
//! CALL :find_dp0
//!
//! IF EXIST "%dp0%\node.exe" (
//!   SET "_prog=%dp0%\node.exe"
//! ) ELSE (
//!   SET "_prog=node"
//!   SET PATHEXT=%PATHEXT:;.JS;=;%
//! )
//!
//! endLocal & goto #_undefined_# 2>NUL || title %COMSPEC% & "%_prog%"  "%dp0%\node_modules\pkg\cli.js" %*
//! ```
//!
//! and one for a package's native program, where the eight lines of the head are
//! followed by `"%dp0%\node_modules\pkg\bin\tool.exe"   %*`. Lines end with CR LF
//! as npm writes them, or LF.
// Read by the Windows agent resolution; tested on every platform.
#![cfg_attr(not(windows), allow(dead_code))]

/// What an npm shim starts, relative to the shim's folder (with `\`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum NpmShim {
    /// Node.js on this script: the shim folder's `node.exe` when there is one,
    /// otherwise `node` from PATH.
    Node { script: String },
    /// This program, directly.
    Program { path: String },
}

const HEAD: [&str; 8] = [
    "@ECHO off",
    "GOTO start",
    ":find_dp0",
    "SET dp0=%~dp0",
    "EXIT /b",
    ":start",
    "SETLOCAL",
    "CALL :find_dp0",
];

const NODE: [&str; 8] = [
    "",
    r#"IF EXIST "%dp0%\node.exe" ("#,
    r#"  SET "_prog=%dp0%\node.exe""#,
    ") ELSE (",
    r#"  SET "_prog=node""#,
    "  SET PATHEXT=%PATHEXT:;.JS;=;%",
    ")",
    "",
];

const NODE_RUN: &str =
    r#"endLocal & goto #_undefined_# 2>NUL || title %COMSPEC% & "%_prog%"  "%dp0%\"#;
const NODE_ARGUMENTS: &str = r#"" %*"#;
const PROGRAM_RUN: &str = r#""%dp0%\"#;
const PROGRAM_ARGUMENTS: &str = r#""   %*"#;

/// A path below the shim's folder as the shim spells it: relative, without
/// quotes, `%` (batch expansion) or control characters.
fn relative(path: &str) -> Option<String> {
    let valid = !path.is_empty()
        && !path.starts_with(['\\', '/'])
        && !path.contains([':', '"', '%'])
        && !path.chars().any(char::is_control);
    valid.then(|| path.to_owned())
}

/// The target of `text` when it is exactly an npm shim.
pub fn npm_shim(text: &str) -> Option<NpmShim> {
    let text = text.strip_suffix('\n')?;
    let lines: Vec<&str> = text
        .split('\n')
        .map(|line| line.strip_suffix('\r').unwrap_or(line))
        .collect();
    let rest = lines.strip_prefix(&HEAD[..])?;
    match rest {
        [run] => {
            let path = run
                .strip_prefix(PROGRAM_RUN)?
                .strip_suffix(PROGRAM_ARGUMENTS)?;
            relative(path).map(|path| NpmShim::Program { path })
        }
        [middle @ .., run] if middle == NODE => {
            let script = run.strip_prefix(NODE_RUN)?.strip_suffix(NODE_ARGUMENTS)?;
            relative(script).map(|script| NpmShim::Node { script })
        }
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A shim as npm's cmd-shim writes it, with its CR LF line ends.
    fn node_shim(target: &str) -> String {
        format!(
            "@ECHO off\r\nGOTO start\r\n:find_dp0\r\nSET dp0=%~dp0\r\nEXIT /b\r\n:start\r\n\
             SETLOCAL\r\nCALL :find_dp0\r\n\r\nIF EXIST \"%dp0%\\node.exe\" (\r\n  \
             SET \"_prog=%dp0%\\node.exe\"\r\n) ELSE (\r\n  SET \"_prog=node\"\r\n  \
             SET PATHEXT=%PATHEXT:;.JS;=;%\r\n)\r\n\r\nendLocal & goto #_undefined_# 2>NUL || \
             title %COMSPEC% & \"%_prog%\"  \"%dp0%\\{target}\" %*\r\n"
        )
    }

    fn program_shim(target: &str) -> String {
        format!(
            "@ECHO off\r\nGOTO start\r\n:find_dp0\r\nSET dp0=%~dp0\r\nEXIT /b\r\n:start\r\n\
             SETLOCAL\r\nCALL :find_dp0\r\n\"%dp0%\\{target}\"   %*\r\n"
        )
    }

    #[test]
    fn npm_shims_name_their_script_or_program() {
        let script = r"node_modules\@anthropic-ai\claude-code\cli.js";
        assert_eq!(
            npm_shim(&node_shim(script)),
            Some(NpmShim::Node {
                script: script.into()
            })
        );
        // A local package's shim below node_modules\.bin reaches up.
        let script = r"..\@openai\codex\bin\codex.js";
        assert_eq!(
            npm_shim(&node_shim(script)),
            Some(NpmShim::Node {
                script: script.into()
            })
        );
        assert_eq!(
            npm_shim(&node_shim(script).replace("\r\n", "\n")),
            Some(NpmShim::Node {
                script: script.into()
            })
        );
        let program = r"node_modules\@scope\tool\bin\tool.exe";
        assert_eq!(
            npm_shim(&program_shim(program)),
            Some(NpmShim::Program {
                path: program.into()
            })
        );
    }

    #[test]
    fn any_other_batch_file_is_refused() {
        let script = r"node_modules\pkg\cli.js";
        let shim = node_shim(script);
        let refused = [
            String::new(),
            // A shebang with arguments, variables or another interpreter.
            shim.replace("\"%_prog%\"  ", "\"%_prog%\" --max-old-space-size=4096 "),
            shim.replace(
                "CALL :find_dp0\r\n\r\n",
                "CALL :find_dp0\r\n@SET NODE_ENV=x\r\n\r\n",
            ),
            shim.replace("SET \"_prog=node\"", "SET \"_prog=python\""),
            // Anything after the shim's last line, or a last line without its end.
            format!("{shim}echo done\r\n"),
            shim.trim_end().to_owned(),
            // Arguments that are not exactly the caller's.
            shim.replace(" %*\r\n", " %1\r\n"),
            // npm 6's shim, and a hand-written wrapper.
            "@IF EXIST \"%~dp0\\node.exe\" (\r\n  \"%~dp0\\node.exe\"  \"%~dp0\\cli.js\" %*\r\n) \
             ELSE (\r\n  @SETLOCAL\r\n  @SET PATHEXT=%PATHEXT:;.JS;=;%\r\n  node  \
             \"%~dp0\\cli.js\" %*\r\n)\r\n"
                .into(),
            "@echo off\r\nnode \"%~dp0\\cli.js\" %*\r\n".into(),
            // Targets that leave the shim's folder or expand variables.
            node_shim(r"C:\tools\cli.js"),
            node_shim(r"\tools\cli.js"),
            node_shim(r"node_modules\%PKG%\cli.js"),
            node_shim(r#"node_modules\a" & calc & "\cli.js"#),
            node_shim(""),
            program_shim(r"bin\tool.exe").replace("\"   %*", "\" %*"),
        ];
        for text in refused {
            assert_eq!(npm_shim(&text), None, "{text:?}");
        }
    }
}
