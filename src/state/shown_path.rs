//! Folders as agents print them in startup panels, banners and status rows.
//!
//! Unix paths compare exactly, as they always have. Windows agents print drive
//! paths (`C:\Users\…`; Kimi never shortens them), Claude and Codex print a
//! folder below the home as `~\…`, and Codex lower-cases the folder of its Git
//! root note (spike S3, 2026-10-13). Windows names ignore case, so a drive path
//! and its home form compare without case. Other Windows forms (UNC shares,
//! `\\?\` paths, MSYS `/c/…`) are not recognized, so a panel showing them for
//! a Windows folder fails closed.

/// A drive path such as `C:\Users`: an ASCII letter, a colon and a backslash.
pub(super) fn drive(path: &str) -> bool {
    matches!(path.as_bytes(), [letter, b':', b'\\', ..] if letter.is_ascii_alphabetic())
}

/// An absolute folder row: a Unix path or a Windows drive path.
pub(super) fn absolute(path: &str) -> bool {
    path.starts_with('/') || drive(path)
}

/// Where the folder starts in a row that ends with one: at its first `/`, `~`
/// or drive path after a space.
pub(super) fn start(row: &str) -> Option<usize> {
    let drive_at = row
        .char_indices()
        .find(|&(i, _)| drive(&row[i..]) && (i == 0 || row[..i].ends_with(' ')))
        .map(|(i, _)| i);
    match (row.find(['/', '~']), drive_at) {
        (Some(a), Some(b)) => Some(a.min(b)),
        (a, b) => a.or(b),
    }
}

/// `path` without `prefix`; letters may differ in case only when `windows`.
pub(super) fn strip_prefix<'a>(path: &'a str, prefix: &str, windows: bool) -> Option<&'a str> {
    if !windows {
        return path.strip_prefix(prefix);
    }
    let mut rest = path.chars();
    for expected in prefix.chars() {
        let ch = rest.next()?;
        if ch != expected && !ch.to_lowercase().eq(expected.to_lowercase()) {
            return None;
        }
    }
    Some(rest.as_str())
}

/// Whether two folder texts are equal; letters may differ in case only when
/// `windows`.
pub(super) fn equal(a: &str, b: &str, windows: bool) -> bool {
    strip_prefix(a, b, windows).is_some_and(str::is_empty)
}

/// Whether an agent's `shown` folder is `path`: exactly, or in any letter
/// case for a Windows drive path.
pub(super) fn same(shown: &str, path: &str) -> bool {
    equal(shown, path, drive(path))
}

/// The form in which Claude and Codex print `path` below `home`, the platform
/// home: `~`, or `~/…` or `~\…` after the style of `home` itself. None outside
/// the home or for a relative home.
pub(super) fn home_alias(path: &str, home: &str) -> Option<String> {
    let windows = drive(home);
    let separator = if windows {
        '\\'
    } else if home.starts_with('/') {
        '/'
    } else {
        return None;
    };
    if equal(path, home, windows) {
        return Some("~".to_owned());
    }
    let relative =
        strip_prefix(path, home.trim_end_matches(separator), windows)?.strip_prefix(separator)?;
    Some(format!("~{separator}{relative}"))
}

/// Whether an agent's `shown` folder is `path`, itself or in its home form
/// below `home`.
pub(super) fn names(shown: &str, path: &str, home: Option<&str>) -> bool {
    let windows = drive(path);
    equal(shown, path, windows)
        || home
            .and_then(|home| home_alias(path, home))
            .is_some_and(|alias| equal(shown, &alias, windows))
}

/// Whether a status row's folder has a shape an agent prints on a computer
/// with this `home`: absolute or below the home form. Windows shapes need a
/// Windows home, so Unix rows read exactly as before.
pub(super) fn status_folder(path: &str, home: Option<&str>) -> bool {
    path.starts_with('/')
        || path.starts_with("~/")
        || (home.is_some_and(drive) && (drive(path) || path.starts_with("~\\")))
}

#[cfg(test)]
mod tests {
    use super::*;
    const HOME: &str = r"C:\Users\user01";
    #[test]
    fn drive_paths_only_are_windows_folders() {
        for path in [r"C:\", r"c:\Users\user01", r"D:\src\repo"] {
            assert!(drive(path) && absolute(path), "{path}");
        }
        for path in [
            "C:",
            "C:/Users",
            r"\\server\share",
            r"\\?\C:\Users",
            "/c/Users",
            r"~\work",
            "1:\\x",
            "CC:\\x",
        ] {
            assert!(!drive(path), "{path}");
        }
        assert!(absolute("/work/example") && !absolute("~/work") && !absolute("work"));
    }
    #[test]
    fn folder_starts_at_the_first_path_form() {
        assert_eq!(start(" ▝▝   ▝▝   /work/example"), Some(" ▝▝   ▝▝   ".len()));
        assert_eq!(
            start(r" ▝▝   ▝▝   ~\zerus-dev\work"),
            Some(" ▝▝   ▝▝   ".len())
        );
        assert_eq!(start(r" ▝▝   ▝▝   D:\src\a~b"), Some(" ▝▝   ▝▝   ".len()));
        assert_eq!(start(r"C:\src"), Some(0));
        assert_eq!(start(r"xC:\src"), None);
        assert_eq!(start(" ▝▝   ▝▝   no folder"), None);
    }
    #[test]
    fn only_windows_folders_ignore_case() {
        assert!(same(r"c:\users\USER01\Work", r"C:\Users\user01\work"));
        assert!(same(r"C:\Users\Ölaf", r"c:\users\ölaf"));
        assert!(!same(r"C:\Users\user01\work2", r"C:\Users\user01\work"));
        assert!(!same(r"C:\Users\user01", r"C:\Users\user01\work"));
        assert!(!same("/WORK/example", "/work/example"));
        assert!(same("/work/example", "/work/example"));
        assert_eq!(strip_prefix(r"C:\USERS\x", r"c:\users", true), Some(r"\x"));
        assert_eq!(strip_prefix("/USERS/x", "/users", false), None);
    }
    #[test]
    fn home_form_follows_the_home_style() {
        let path = r"C:\Users\user01\zerus-dev\work";
        assert_eq!(home_alias(path, HOME).as_deref(), Some(r"~\zerus-dev\work"));
        assert_eq!(
            home_alias(path, r"c:\users\USER01\").as_deref(),
            Some(r"~\zerus-dev\work")
        );
        assert_eq!(home_alias(r"c:\users\user01", HOME).as_deref(), Some("~"));
        assert_eq!(home_alias(r"C:\Users\user012\work", HOME), None);
        assert_eq!(home_alias(r"D:\work", HOME), None);
        assert_eq!(
            home_alias("/home/example/work", "/home/example/").as_deref(),
            Some("~/work")
        );
        assert_eq!(
            home_alias("/home/example", "/home/example").as_deref(),
            Some("~")
        );
        assert_eq!(home_alias("/HOME/example/work", "/home/example"), None);
        assert_eq!(home_alias("/home/example/work", "relative"), None);
        assert_eq!(home_alias(path, "/home/example"), None);
        assert!(names(r"~\ZERUS-dev\work", path, Some(HOME)));
        assert!(names(r"C:\Users\user01\zerus-dev\work", path, None));
        assert!(!names(r"~/zerus-dev/work", path, Some(HOME)));
        assert!(!names(
            "~/Work",
            "/home/example/work",
            Some("/home/example")
        ));
    }
    #[test]
    fn windows_status_folders_need_a_windows_home() {
        for path in ["/work", "~/work"] {
            assert!(
                status_folder(path, None) && status_folder(path, Some(HOME)),
                "{path}"
            );
        }
        for path in [r"~\work", r"C:\work"] {
            assert!(status_folder(path, Some(HOME)), "{path}");
            assert!(
                !status_folder(path, Some("/home/example")) && !status_folder(path, None),
                "{path}"
            );
        }
        for path in ["~", "work", r"\\server\share", "arbitrary text"] {
            assert!(!status_folder(path, Some(HOME)), "{path}");
        }
    }
}
