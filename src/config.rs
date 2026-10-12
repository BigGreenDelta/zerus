//! Compatibility with hgs' assignment-based configuration and project maps.
//! Configuration is data: unlike the former shell implementation, reading it does not execute it.
use crate::{
    cli::{Error, Result},
    platform,
};
use std::{
    collections::{HashMap, HashSet},
    env, fs,
    io::Write,
    path::{Path, PathBuf},
};

pub struct Config {
    pub home: String,
    pub dir: PathBuf,
    pub state: PathBuf,
    pub selves: Vec<String>,
    pub peers: Vec<String>,
    pub base_peers: Vec<String>,
    pub peers_from_environment: bool,
    pub machines: std::collections::BTreeMap<String, crate::machines::Connection>,
    pub tab: bool,
    pub tab_colors: String,
}

impl Config {
    pub fn load() -> Result<Self> {
        let home = platform::home().ok_or_else(|| {
            Error::new(
                1,
                if cfg!(windows) {
                    "HOME or USERPROFILE is not set"
                } else {
                    "HOME is not set"
                },
            )
        })?;
        let dir = PathBuf::from(
            env::var("HGS_CONFIG_DIR").unwrap_or_else(|_| platform::below(&home, ".config/hgs")),
        );
        let raw_state = env::var("HGS_STATE_DIR").unwrap_or_else(|_| {
            platform::below(
                &env::var("XDG_STATE_HOME")
                    .unwrap_or_else(|_| platform::below(&home, ".local/state")),
                "hgs/sessions",
            )
        });
        let state = if Path::new(&raw_state).is_absolute() {
            PathBuf::from(raw_state)
        } else {
            env::current_dir()?.join(raw_state)
        };
        env::set_var("HGS_STATE_DIR", &state);
        let mut values = HashMap::new();
        match fs::read_to_string(dir.join("config")) {
            Ok(text) => {
                for (index, line) in text.lines().enumerate() {
                    let line = line.trim();
                    if line.is_empty() || line.starts_with('#') {
                        continue;
                    }
                    let line = line.strip_prefix("export ").unwrap_or(line).trim_start();
                    let Some((key, raw)) = line.split_once('=') else {
                        return Err(Error::new(
                            1,
                            format!(
                                "{}:{}: expected NAME=value",
                                dir.join("config").display(),
                                index + 1
                            ),
                        ));
                    };
                    let key = key.trim();
                    if !matches!(key, "HGS_SELF" | "HGS_PEERS" | "HGS_TAB" | "HGS_TAB_COLORS") {
                        continue;
                    }
                    let value = assignment_value(raw, &home, &values).map_err(|message| {
                        Error::new(
                            1,
                            format!("{}:{}: {message}", dir.join("config").display(), index + 1),
                        )
                    })?;
                    values.insert(key.to_owned(), value);
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => return Err(e.into()),
        }
        let get = |key: &str, default: &str| {
            env::var(key)
                .ok()
                .or_else(|| values.get(key).cloned())
                .unwrap_or_else(|| default.to_owned())
        };
        let mut self_names = env::var("HGS_SELF")
            .ok()
            .filter(|v| !v.is_empty())
            .or_else(|| values.get("HGS_SELF").cloned())
            .unwrap_or_default();
        if self_names.is_empty() {
            self_names = platform::hostname();
            if self_names.is_empty() {
                self_names = "localhost".into();
            }
        }
        let base_peers: Vec<String> = get("HGS_PEERS", "")
            .split_whitespace()
            .map(String::from)
            .collect();
        let machines = crate::machines::load(&dir)?;
        let peers_from_environment = env::var("HGS_PEERS").is_ok();
        let peers =
            crate::machines::effective_peers(&base_peers, &machines, peers_from_environment);
        Ok(Self {
            home,
            dir,
            state,
            selves: self_names.split_whitespace().map(String::from).collect(),
            peers,
            base_peers,
            peers_from_environment,
            machines,
            tab: get("HGS_TAB", "1") == "1",
            tab_colors: get("HGS_TAB_COLORS", ""),
        })
    }
    pub fn host(&self) -> &str {
        self.selves
            .first()
            .map(String::as_str)
            .unwrap_or("localhost")
    }
    pub fn is_self(&self, host: &str) -> bool {
        self.selves.iter().any(|v| v == host)
    }
    pub fn expand_home(&self, path: &str) -> String {
        if cfg!(windows) {
            if let Some(tail) = path.strip_prefix("~/").or_else(|| path.strip_prefix("~\\")) {
                return platform::below(&self.home, tail);
            }
        }
        path.strip_prefix('~')
            .map(|tail| format!("{}{tail}", self.home))
            .unwrap_or_else(|| path.to_owned())
    }
    pub fn projects(&self) -> Result<Vec<Project>> {
        let mut projects = Vec::new();
        let mut seen = HashSet::new();
        for (file, source) in [("projects.local", "local")] {
            let text = match fs::read_to_string(self.dir.join(file)) {
                Ok(text) => text,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
                Err(e) => return Err(e.into()),
            };
            for line in text.lines() {
                let Some((name, path)) = line.split_once('=') else {
                    continue;
                };
                let (name, path) = (name.trim(), path.trim());
                if name.is_empty()
                    || name.starts_with('#')
                    || path.is_empty()
                    || !seen.insert(name.to_owned())
                {
                    continue;
                }
                projects.push(Project {
                    name: name.into(),
                    dir: path.into(),
                    src: source.into(),
                });
            }
        }
        Ok(projects)
    }
    /// Serialize project read/modify/write across CLI and GUI processes, then atomically replace.
    pub fn write_project(&self, name: &str, directory: Option<&str>, add_only: bool) -> Result<()> {
        fs::create_dir_all(&self.dir)?;
        let lock = fs::OpenOptions::new()
            .create(true)
            .truncate(false)
            .write(true)
            .open(self.dir.join("projects.lock"))?;
        let _lock = crate::platform::FileLock::exclusive(lock)?;
        let existing = self.projects()?.into_iter().find(|p| p.name == name);
        if add_only {
            if let Some(project) = existing.as_ref() {
                return Err(Error::new(1, format!("project '{name}' already exists ({}); use 'hgs project set' to override it", project.src)));
            }
        }
        if directory.is_none() {
            match existing.as_ref().map(|p| p.src.as_str()) {
                Some("local") => {}
                _ => return Err(Error::new(1, format!("unknown project '{name}'"))),
            }
        }
        let file = self.dir.join("projects.local");
        let old = match fs::read_to_string(&file) {
            Ok(text) => text,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => String::new(),
            Err(e) => return Err(e.into()),
        };
        let mut temporary = tempfile::NamedTempFile::new_in(&self.dir)?;
        if let Ok(metadata) = fs::metadata(&file) {
            temporary
                .as_file()
                .set_permissions(metadata.permissions())?;
        }
        for line in old.lines() {
            if line
                .split_once('=')
                .map(|(key, _)| key.trim())
                .unwrap_or(line.trim())
                != name
            {
                writeln!(temporary, "{line}")?;
            }
        }
        if let Some(directory) = directory {
            writeln!(temporary, "{name}={directory}")?;
        }
        temporary.as_file().sync_all()?;
        platform::persist(temporary, &file)
            .map_err(|e| Error::new(1, format!("cannot replace {}: {e}", file.display())))?;
        platform::sync_dir(&self.dir)?;
        Ok(())
    }
}

pub struct Project {
    pub name: String,
    pub dir: String,
    pub src: String,
}

pub fn check_name(name: &str) -> Result<()> {
    if name.is_empty()
        || name.contains(['/', '='])
        || name.starts_with('#')
        || name.chars().any(char::is_whitespace)
    {
        return Err(Error::new(1, format!("bad project name '{name}': must be non-empty, no '/', no '=', no leading '#' and no whitespace")));
    }
    if name.contains(['.', ':']) {
        eprintln!("hgs: '{name}' contains . or : — the session name will use '_' instead");
    }
    Ok(())
}
pub fn check_directory(path: &str) -> Result<()> {
    if path.contains(['\n', '\r']) {
        return Err(Error::new(1, "bad directory: a path with a newline cannot be stored in projects.local (one 'name=dir' per line)"));
    }
    if path.trim() != path {
        return Err(Error::new(1, format!("bad directory '{path}': leading/trailing whitespace is stripped when the file is read back")));
    }
    Ok(())
}

/// Decode one shell-style assignment, including quotes, escapes and simple variable expansion.
/// Command substitutions and operators are rejected rather than executed or silently changed.
fn assignment_value(
    raw: &str,
    home: &str,
    values: &HashMap<String, String>,
) -> std::result::Result<String, String> {
    let mut out = String::new();
    let mut quote = None;
    let mut chars = raw.trim_start().chars().peekable();
    while let Some(c) = chars.next() {
        if quote == Some('\'') {
            if c == '\'' {
                quote = None;
            } else {
                out.push(c);
            }
            continue;
        }
        match c {
            '\'' if quote.is_none() => quote = Some('\''),
            '"' => if quote == Some('"') { quote = None; } else { quote = Some('"'); },
            '\\' => {
                let next = chars.next().ok_or("unterminated escape in config value")?;
                if quote == Some('"') && !matches!(next, '$' | '`' | '"' | '\\') { out.push('\\'); }
                out.push(next);
            },
            '$' => {
                let braced = chars.peek() == Some(&'{');
                if braced { chars.next(); }
                if chars.peek() == Some(&'(') { return Err("shell command substitution is not supported in config; use literal NAME=value assignments".into()); }
                let mut key = String::new();
                while chars.peek().is_some_and(|c| c.is_ascii_alphanumeric() || *c == '_') { key.push(chars.next().unwrap()); }
                if braced && chars.next() != Some('}') { return Err("only simple ${NAME} variable references are supported in config".into()); }
                if key.is_empty() { out.push('$'); continue; }
                if key == "HOME" { out.push_str(home); }
                else if let Some(value) = values.get(&key) { out.push_str(value); }
                else if let Ok(value) = env::var(&key) { out.push_str(&value); }
            },
            '`' => return Err("shell command substitution is not supported in config; use literal NAME=value assignments".into()),
            c if quote.is_none() && c.is_whitespace() => {
                while chars.peek().is_some_and(|c| c.is_whitespace()) { chars.next(); }
                if chars.peek().is_none() || chars.peek() == Some(&'#') { break; }
                return Err("quote values containing spaces; shell commands are not supported in config".into());
            },
            '#' if quote.is_none() && out.is_empty() => break,
            ';' | '|' | '&' | '<' | '>' | '(' | ')' if quote.is_none() => return Err("shell operators are not supported in config; use literal NAME=value assignments".into()),
            _ => out.push(c),
        }
    }
    if quote.is_some() {
        return Err("unterminated quote in config value".into());
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn assignment_quotes_comments_and_expansions_match_supported_shell_format() {
        let values = HashMap::from([("BOX".to_owned(), "arch".to_owned())]);
        let parse = |raw| assignment_value(raw, "/home/test user", &values).unwrap();
        assert_eq!(parse(r#""arch mac" # two aliases"#), "arch mac");
        assert_eq!(parse("'mac=#80ff80'"), "mac=#80ff80");
        assert_eq!(parse("mac=#80ff80 # colour"), "mac=#80ff80");
        assert_eq!(parse(r#""${HOME}/$BOX""#), "/home/test user/arch");
        assert_eq!(parse("'$HOME/${BOX}'"), "$HOME/${BOX}");
        assert_eq!(parse(r#""\$HOME \\ \"quoted\"""#), "$HOME \\ \"quoted\"");
        assert_eq!(parse("arch\\ mac"), "arch mac");
        assert_eq!(parse("# empty value"), "");
    }

    #[test]
    fn reading_config_never_executes_shell_commands() {
        let directory = tempfile::tempdir().unwrap();
        let marker = directory.path().join("must-not-exist");
        for raw in [
            format!("$(touch {})", marker.display()),
            format!("\"$(touch {})\"", marker.display()),
            format!("`touch {}`", marker.display()),
            format!("arch; touch {}", marker.display()),
            "arch | sh".into(),
            "arch && echo oops".into(),
            "${HOME:-fallback}".into(),
            "\"unterminated".into(),
            "arch mac".into(),
        ] {
            assert!(
                assignment_value(&raw, "/home/test", &HashMap::new()).is_err(),
                "accepted {raw}"
            );
            assert!(!marker.exists(), "executed {raw}");
        }
        // Single quotes deliberately preserve command-looking text as inert data.
        assert_eq!(
            assignment_value("'$(touch never)'", "/home/test", &HashMap::new()).unwrap(),
            "$(touch never)"
        );
    }
}
