//! Project launchers: a script in a session's folder that starts the agent in
//! place of its native CLI, for instance to unlock secrets first. On Unix it is
//! an executable `run_<command>.sh`, or else a `run_<command>.ps1` when
//! PowerShell 7 (`pwsh`) is on PATH. On Windows it is a `run_<command>.ps1` only,
//! which PowerShell 7, else Windows PowerShell, runs with `-File`; a `.sh` there
//! is never started. `--bare` ignores both. The launcher keeps the arguments
//! hgs gives the agent, which follow its path.
//!
//! A PowerShell launcher takes several words of a launch's argv, so everything
//! that reads a saved launch recipe asks `program_words` where the agent's own
//! arguments begin.
use std::path::Path;

/// Replace the program, the first `program` words of `base` and the first word
/// of `resume` (when there is a resume variant), with the launcher of `command`
/// in `directory`, if it has one. An error names a `.ps1` launcher that no
/// PowerShell can run on Windows.
pub fn apply(
    directory: &str,
    command: &str,
    bare: bool,
    program: usize,
    base: &mut Vec<String>,
    resume: &mut Vec<String>,
) -> Result<(), String> {
    if bare || directory.is_empty() {
        return Ok(());
    }
    let words = words(
        Path::new(directory),
        command,
        cfg!(windows),
        |path| crate::platform::executable(path),
        powershell,
    )?;
    if let Some(words) = words {
        replace(base, resume, program, words);
    }
    Ok(())
}

fn replace(base: &mut Vec<String>, resume: &mut Vec<String>, program: usize, words: Vec<String>) {
    if !resume.is_empty() {
        resume.splice(0..1, words.iter().cloned());
    }
    base.splice(0..program.min(base.len()), words);
}

/// The PowerShell that runs a `.ps1` launcher: on Windows PowerShell 7, else
/// Windows PowerShell (`platform::launcher_powershell`), on Unix `pwsh` from PATH.
fn powershell() -> Result<String, String> {
    #[cfg(windows)]
    {
        crate::platform::launcher_powershell()
    }
    #[cfg(unix)]
    {
        crate::platform::which("pwsh")
            .map(|path| path.to_string_lossy().into_owned())
            .ok_or_else(|| "pwsh was not found on PATH".into())
    }
}

/// The words that start the launcher of `command` in `directory`, or `None`
/// when it has none that can run here. `windows` selects the platform's rule;
/// `executable` and `powershell` are `platform::executable` and the PowerShell
/// lookup, given by the caller so that each platform's rule can be tested
/// anywhere.
fn words(
    directory: &Path,
    command: &str,
    windows: bool,
    executable: impl Fn(&Path) -> bool,
    powershell: impl FnOnce() -> Result<String, String>,
) -> Result<Option<Vec<String>>, String> {
    if !windows {
        let script = directory.join(format!("run_{command}.sh"));
        if executable(&script) {
            return Ok(Some(vec![script.to_string_lossy().into_owned()]));
        }
    }
    let script = directory.join(format!("run_{command}.ps1"));
    if !script.is_file() {
        return Ok(None);
    }
    let program = match powershell() {
        Ok(program) => program,
        // Unix keeps starting the native CLI, as it did before `.ps1`
        // launchers: a project may ship one for its Windows users only.
        Err(_) if !windows => return Ok(None),
        Err(error) => {
            return Err(format!(
                "the project launcher {} needs PowerShell: {error}; install PowerShell 7 or start with --bare",
                script.display()
            ))
        }
    };
    let mut words = vec![program, "-NoLogo".into(), "-NoProfile".into()];
    if windows {
        // A downloaded or unsigned project script must not stop at the
        // machine's execution policy; the folder is already trusted to start it.
        words.extend(["-ExecutionPolicy".into(), "Bypass".into()]);
    }
    words.extend(["-File".into(), script.to_string_lossy().into_owned()]);
    Ok(Some(words))
}

/// How many leading words of the saved launch `argv` start the program: the
/// whole PowerShell command line of a `.ps1` launcher, as `words` builds it,
/// else the program alone. The agent's own arguments follow.
pub fn program_words(argv: &[String]) -> usize {
    let Some(first) = argv.first() else {
        return 0;
    };
    let name = first
        .rsplit(['/', '\\'])
        .next()
        .unwrap_or(first)
        .to_ascii_lowercase();
    if !matches!(
        name.strip_suffix(".exe").unwrap_or(&name),
        "pwsh" | "powershell"
    ) {
        return 1;
    }
    let mut index = 1;
    while index < argv.len() {
        match argv[index].as_str() {
            "-NoLogo" | "-NoProfile" => index += 1,
            "-ExecutionPolicy" => index += 2,
            "-File" if index + 1 < argv.len() => return index + 2,
            _ => break,
        }
    }
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    fn strings(values: &[&str]) -> Vec<String> {
        values.iter().map(|value| (*value).to_owned()).collect()
    }

    /// A folder with the files `names`; `executable` reads every `.sh` as
    /// executable, as `chmod +x` would make it on Unix.
    fn folder(names: &[&str]) -> tempfile::TempDir {
        let folder = tempfile::tempdir().unwrap();
        for name in names {
            std::fs::write(folder.path().join(name), "").unwrap();
        }
        folder
    }

    fn sh(path: &Path) -> bool {
        path.extension().is_some_and(|extension| extension == "sh") && path.is_file()
    }

    fn launch(
        folder: &Path,
        command: &str,
        windows: bool,
        powershell: Result<&str, &str>,
    ) -> Result<Option<Vec<String>>, String> {
        words(folder, command, windows, sh, || {
            powershell.map(str::to_owned).map_err(str::to_owned)
        })
    }

    fn path(folder: &Path, name: &str) -> String {
        folder.join(name).to_string_lossy().into_owned()
    }

    #[test]
    fn windows_runs_a_ps1_launcher_with_powershell_and_never_a_sh_one() {
        let pwsh = r"C:\Program Files\PowerShell\7\pwsh.exe";
        let both = folder(&["run_claude.sh", "run_claude.ps1"]);
        let script = path(both.path(), "run_claude.ps1");
        assert_eq!(
            launch(both.path(), "claude", true, Ok(pwsh)).unwrap().unwrap(),
            strings(&[pwsh, "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", &script])
        );
        let only_sh = folder(&["run_claude.sh"]);
        assert_eq!(launch(only_sh.path(), "claude", true, Ok(pwsh)).unwrap(), None);
        // Another command's launcher is not this one's.
        assert_eq!(launch(both.path(), "codex", true, Ok(pwsh)).unwrap(), None);
    }

    #[test]
    fn windows_names_a_ps1_launcher_that_no_powershell_can_run() {
        let ps1 = folder(&["run_codex.ps1"]);
        let error = launch(ps1.path(), "codex", true, Err("neither pwsh.exe nor powershell.exe was found"))
            .unwrap_err();
        assert!(error.contains("run_codex.ps1"), "{error}");
        assert!(error.contains("neither pwsh.exe nor powershell.exe was found"), "{error}");
        assert!(error.contains("--bare"), "{error}");
        // Without a launcher PowerShell is never asked for.
        let none = folder(&[]);
        assert_eq!(launch(none.path(), "codex", true, Err("unused")).unwrap(), None);
    }

    #[test]
    fn unix_prefers_an_executable_sh_launcher_then_a_ps1_one_that_pwsh_runs() {
        let both = folder(&["run_claude.sh", "run_claude.ps1"]);
        assert_eq!(
            launch(both.path(), "claude", false, Ok("/usr/bin/pwsh")).unwrap().unwrap(),
            vec![path(both.path(), "run_claude.sh")]
        );
        let ps1 = folder(&["run_claude.ps1"]);
        let script = path(ps1.path(), "run_claude.ps1");
        assert_eq!(
            launch(ps1.path(), "claude", false, Ok("/usr/bin/pwsh")).unwrap().unwrap(),
            strings(&["/usr/bin/pwsh", "-NoLogo", "-NoProfile", "-File", &script])
        );
        // No pwsh: the native CLI starts, as before `.ps1` launchers.
        assert_eq!(launch(ps1.path(), "claude", false, Err("pwsh was not found on PATH")).unwrap(), None);
    }

    #[test]
    fn launcher_words_replace_the_program_of_both_variants_before_the_agent_arguments() {
        let pwsh = strings(&["pwsh", "-NoLogo", "-NoProfile", "-File", "/p/run_codex.ps1"]);
        let mut base = strings(&["codex"]);
        let mut resume = strings(&["codex", "resume", "--last"]);
        replace(&mut base, &mut resume, 1, pwsh.clone());
        // The caller adds a requested ID, extra arguments and options afterwards.
        base.extend(strings(&["-c", "model=\"test model\""]));
        assert_eq!(base, [pwsh.clone(), strings(&["-c", "model=\"test model\""])].concat());
        assert_eq!(resume, [pwsh.clone(), strings(&["resume", "--last"])].concat());
        assert_eq!(program_words(&resume), 5);
        // Commands without a resume variant keep none.
        let mut base = strings(&["htop"]);
        let mut resume = Vec::new();
        replace(&mut base, &mut resume, 1, pwsh.clone());
        assert_eq!((base, resume), (pwsh.clone(), Vec::new()));
        // A Windows `sh` session's program is every word of its PowerShell.
        let mut base = strings(&[r"C:\Windows\powershell.exe", "-NoLogo"]);
        replace(&mut base, &mut Vec::new(), 2, pwsh.clone());
        assert_eq!(base, pwsh);
    }

    #[test]
    fn bare_and_folderless_launches_keep_the_native_program() {
        let ps1 = folder(&["run_claude.ps1"]);
        let directory = ps1.path().to_string_lossy().into_owned();
        let mut base = strings(&["claude"]);
        let mut resume = strings(&["claude", "-c"]);
        apply(&directory, "claude", true, 1, &mut base, &mut resume).unwrap();
        assert_eq!((base.as_slice(), resume.as_slice()), (&strings(&["claude"])[..], &strings(&["claude", "-c"])[..]));
        apply("", "claude", false, 1, &mut base, &mut resume).unwrap();
        assert_eq!(base, ["claude"]);
    }

    #[test]
    fn program_words_cover_exactly_a_powershell_launcher_command_line() {
        let windows = strings(&[
            r"C:\Program Files\PowerShell\7\pwsh.exe", "-NoLogo", "-NoProfile",
            "-ExecutionPolicy", "Bypass", "-File", r"C:\p\run_claude.ps1", "--resume", "id",
        ]);
        assert_eq!(program_words(&windows), 7);
        let legacy = strings(&[
            r"C:\WINDOWS\System32\WindowsPowerShell\v1.0\PowerShell.EXE", "-NoLogo", "-NoProfile",
            "-ExecutionPolicy", "Bypass", "-File", r"C:\p\run_claude.ps1",
        ]);
        assert_eq!(program_words(&legacy), 7);
        let unix = strings(&["/usr/bin/pwsh", "-NoLogo", "-NoProfile", "-File", "/p/run_kimi.ps1", "-c"]);
        assert_eq!(program_words(&unix), 5);
        for native in [
            strings(&["claude", "--resume", "id"]),
            strings(&["/p/run_claude.sh", "-c"]),
            // PowerShell that runs no launcher file is a program like another.
            strings(&["pwsh", "-NoLogo"]),
            strings(&["pwsh", "-NoLogo", "-File"]),
            strings(&["pwsh", "-Command", "claude"]),
        ] {
            assert_eq!(program_words(&native), 1, "{native:?}");
        }
        assert_eq!(program_words(&[]), 0);
    }
}
