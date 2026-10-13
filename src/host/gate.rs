//! Who may run a session host for a state directory, and from which image
//! (spec D2). Local sessions are off until the user enables them, and a host
//! runs from a staged image, never from the GUI's folder or a build tree, so an
//! update can always replace those files.
//!
//! - `__host serve` serves only when the `local-sessions` marker exists and its
//!   own executable is a staged host image (`<images>\<name>\hgs.exe`, whose
//!   SHA-256 starts with `<name>`). Otherwise it exits at once, before it writes
//!   anything, with `EXIT_NOT_ENABLED` or `EXIT_UNSTAGED`, which its launcher
//!   reports. A running host refuses `create_session` once the marker is gone,
//!   so a launch that races `hgs session-host uninstall` starts nothing.
//! - A launcher starts the executable of the image that the pointer `current`
//!   names, and reads the pointer again before each start: a later install may
//!   have retired the image it read.
//!
//! Tests and the isolated hosts of `docs/windows-host-testing.md` run a build
//! that was never staged, on a computer where local sessions may be off. Two
//! variables allow that, and only for an *isolated* state directory: one that
//! `HGS_STATE_DIR` names and that is not a directory this user's sessions use
//! by default (`XDG_STATE_HOME\hgs\sessions`, `~\.local\state\hgs\sessions`).
//! `HGS_STATE_DIR` alone proves nothing: every hgs process exports it to its
//! children, hooks and supervisors included.
//!
//! - `HGS_HOST_ALLOW_UNSTAGED=1` lets `__host serve` run without the marker and
//!   from any executable, and lets that host create sessions without the
//!   marker. For any other state directory the variable is ignored, so one left
//!   in an environment never opens the user's own sessions to an unstaged host.
//! - `HGS_HOST_IMAGE=<absolute path of an hgs.exe>` makes every launcher (`hgs
//!   __host call --start` and the session starts of the terminal backend) start
//!   that executable as the host instead of the staged image; tests name the
//!   build under test, the one that runs. For any other state directory it is
//!   an error. Nothing else reads it.
//!
//! The decisions are plain functions tested on every platform; reading the
//! environment and the files they decide on is Windows-only.
use super::staging::{self, EXECUTABLE};
use std::{
    ffi::OsStr,
    fmt,
    path::{Path, PathBuf},
};

/// `1` allows an unstaged host for an isolated state directory.
pub(crate) const ALLOW_UNSTAGED: &str = "HGS_HOST_ALLOW_UNSTAGED";
/// The executable `__host call --start` starts for an isolated state directory.
pub(crate) const IMAGE: &str = "HGS_HOST_IMAGE";
/// `__host serve`'s exit code when local sessions are not enabled.
pub(crate) const EXIT_NOT_ENABLED: i32 = 4;
/// `__host serve`'s exit code when it does not run from a staged image.
pub(crate) const EXIT_UNSTAGED: i32 = 5;

/// What a test host may do instead (see the module documentation).
const OVERRIDE_HINT: &str =
    "a test host for an isolated HGS_STATE_DIR may set HGS_HOST_ALLOW_UNSTAGED=1";
const NOT_ENABLED: &str =
    "local sessions are not enabled on this computer; run hgs session-host install";

/// Why a host may not serve.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum Refusal {
    /// The `local-sessions` marker is missing.
    NotEnabled,
    /// The host's executable, at this path, is not a staged image.
    Unstaged(PathBuf),
}

impl Refusal {
    /// `__host serve`'s exit code for this refusal.
    pub(crate) fn exit_code(&self) -> i32 {
        match self {
            Self::NotEnabled => EXIT_NOT_ENABLED,
            Self::Unstaged(_) => EXIT_UNSTAGED,
        }
    }
}

impl fmt::Display for Refusal {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NotEnabled => write!(f, "{NOT_ENABLED} ({OVERRIDE_HINT})"),
            Self::Unstaged(image) => write!(
                f,
                "{} is not a staged session host image; a session host runs from the image \
                 hgs session-host install staged ({OVERRIDE_HINT})",
                image.display()
            ),
        }
    }
}

/// Why a host that its launcher started exited with `code` before it served,
/// when that was a refusal.
pub(crate) fn launch_refusal(code: i32) -> Option<String> {
    match code {
        EXIT_NOT_ENABLED => Some(Refusal::NotEnabled.to_string()),
        EXIT_UNSTAGED => Some(format!(
            "the session host does not run from a staged image; run hgs session-host install \
             ({OVERRIDE_HINT})"
        )),
        _ => None,
    }
}

/// Whether `value`, the value of `HGS_HOST_ALLOW_UNSTAGED`, asks for the test
/// override: exactly `1`.
pub(crate) fn override_requested(value: Option<&OsStr>) -> bool {
    value == Some(OsStr::new("1"))
}

/// Whether the canonical `state` is an isolated state directory: `explicit`
/// (`HGS_STATE_DIR` names it) and none of `defaults`, this user's default
/// state directories in canonical form. A default that does not exist cannot
/// be the existing `state` and may be left out.
pub(crate) fn isolated(explicit: bool, state: &Path, defaults: &[PathBuf]) -> bool {
    explicit && !defaults.iter().any(|default| default == state)
}

/// Whether the canonical `exe` is the executable of an image in the canonical
/// `images` folder: `<images>\<name>\hgs.exe`, where `<name>` is the image name
/// of `hash`, the executable's SHA-256 (staging's content addressing).
pub(crate) fn staged(exe: &Path, images: &Path, hash: &str) -> bool {
    let Some(folder) = exe.parent() else {
        return false;
    };
    let named = |path: &Path, expected: &str, any_case: bool| {
        path.file_name()
            .and_then(OsStr::to_str)
            .is_some_and(|name| {
                if any_case {
                    name.eq_ignore_ascii_case(expected)
                } else {
                    name == expected
                }
            })
    };
    // A whole SHA-256: 64 hex digits.
    hash.len() == 64
        && folder.parent() == Some(images)
        && named(folder, staging::image_name(hash), false)
        && named(exe, EXECUTABLE, true)
}

/// Whether a host may serve: when the marker exists (`enabled`) and it runs
/// from a staged image, or when the test override applies (`unstaged_allowed`:
/// requested, for an isolated state directory). `image` is the host's
/// executable, for the message.
pub(crate) fn admit(
    enabled: bool,
    staged: bool,
    unstaged_allowed: bool,
    image: &Path,
) -> Result<(), Refusal> {
    if unstaged_allowed {
        Ok(())
    } else if !enabled {
        Err(Refusal::NotEnabled)
    } else if !staged {
        Err(Refusal::Unstaged(image.to_owned()))
    } else {
        Ok(())
    }
}

/// The executable `__host call --start` starts as the host: `explicit`
/// (`HGS_HOST_IMAGE`) for an isolated state directory, else the executable of
/// the image the pointer names (`current`) once local sessions are enabled.
pub(crate) fn call_image(
    explicit: Option<PathBuf>,
    isolated: bool,
    enabled: bool,
    current: Option<PathBuf>,
) -> Result<PathBuf, String> {
    match explicit {
        Some(_) if !isolated => Err(format!(
            "{IMAGE} applies only to an isolated state directory: set HGS_STATE_DIR to a \
             directory that is not this user's own"
        )),
        Some(image) if !image.is_absolute() => Err(format!(
            "{IMAGE} must be an absolute path, not {}",
            image.display()
        )),
        Some(image) => Ok(image),
        None if !enabled => Err(NOT_ENABLED.into()),
        None => current
            .ok_or_else(|| "no session host image is staged; run hgs session-host install".into()),
    }
}

#[cfg(windows)]
pub(crate) use self::windows::{explicit_state_dir, host_image, unstaged_allowed};

#[cfg(windows)]
mod windows {
    use super::*;
    use crate::platform;
    use std::env;

    /// The executable a launcher starts as the host of the canonical `state`
    /// (D2): the staged image the pointer names once local sessions are
    /// enabled, or `HGS_HOST_IMAGE` for an isolated state directory. Asked
    /// again before each start (`client::connect_or_start`).
    pub(crate) fn host_image(state: &Path) -> Result<PathBuf, String> {
        let explicit = env::var_os(IMAGE)
            .filter(|value| !value.is_empty())
            .map(PathBuf::from);
        let isolated = explicit.is_some()
            && super::isolated(explicit_state_dir(), state, &default_state_dirs());
        let current = staging::current_image().map(|folder| folder.join(EXECUTABLE));
        call_image(explicit, isolated, staging::enabled(), current)
    }

    /// Whether `HGS_STATE_DIR` names the state directory.
    pub(crate) fn explicit_state_dir() -> bool {
        env::var_os("HGS_STATE_DIR").is_some_and(|value| !value.is_empty())
    }

    /// This user's default state directories (what `state::storage::root`
    /// uses without `HGS_STATE_DIR`, below `XDG_STATE_HOME`, `HOME` or
    /// `USERPROFILE`) that exist, in canonical form.
    pub(crate) fn default_state_dirs() -> Vec<PathBuf> {
        let below = |variable: &str, parts: &[&str]| {
            env::var_os(variable)
                .filter(|value| !value.is_empty())
                .map(|value| {
                    parts
                        .iter()
                        .fold(PathBuf::from(value), |path, part| path.join(part))
                })
        };
        [
            below("XDG_STATE_HOME", &["hgs", "sessions"]),
            below("HOME", &[".local", "state", "hgs", "sessions"]),
            below("USERPROFILE", &[".local", "state", "hgs", "sessions"]),
        ]
        .into_iter()
        .flatten()
        .filter_map(|path| platform::canonical(path).ok())
        .collect()
    }

    /// Whether this process may run an unstaged host for the canonical `state`:
    /// `HGS_HOST_ALLOW_UNSTAGED=1` and an isolated state directory.
    pub(crate) fn unstaged_allowed(state: &Path) -> bool {
        override_requested(env::var_os(ALLOW_UNSTAGED).as_deref())
            && isolated(explicit_state_dir(), state, &default_state_dirs())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const HASH: &str = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    fn path(text: &str) -> PathBuf {
        // Absolute on every platform.
        if cfg!(windows) {
            PathBuf::from(format!(r"C:\{}", text.replace('/', r"\")))
        } else {
            PathBuf::from(format!("/{text}"))
        }
    }

    #[test]
    fn only_a_named_non_default_state_directory_is_isolated() {
        let state = path("tmp/case/state");
        let defaults = [path("home/.local/state/hgs/sessions")];
        assert!(isolated(true, &state, &defaults));
        assert!(isolated(true, &state, &[]));
        // Every hgs process exports HGS_STATE_DIR, the default one included.
        assert!(!isolated(true, &defaults[0], &defaults));
        assert!(!isolated(false, &state, &defaults));

        assert!(override_requested(Some(OsStr::new("1"))));
        for value in [None, Some(""), Some("0"), Some("true"), Some("1 ")] {
            assert!(!override_requested(value.map(OsStr::new)), "{value:?}");
        }
    }

    #[test]
    fn a_staged_image_is_named_by_its_hash_inside_the_images_folder() {
        let images = path("home/.local/share/hgs/host");
        let exe = images.join(&HASH[..16]).join(EXECUTABLE);
        assert!(staged(&exe, &images, HASH));
        assert!(staged(
            &images.join(&HASH[..16]).join("HGS.EXE"),
            &images,
            HASH
        ));
        // Another build's folder, a nested folder, another file, another root.
        let other = "f".repeat(64);
        assert!(!staged(&exe, &images, &other));
        assert!(!staged(
            &images.join("x").join(&HASH[..16]).join(EXECUTABLE),
            &images,
            HASH
        ));
        assert!(!staged(
            &images.join(&HASH[..16]).join("hgs-old.exe"),
            &images,
            HASH
        ));
        assert!(!staged(&exe, &path("other/host"), HASH));
        assert!(!staged(&path("build/hgs.exe"), &images, HASH));
        assert!(!staged(&exe, &images, &HASH[..16]));
        assert!(!staged(Path::new(EXECUTABLE), &images, HASH));
    }

    #[test]
    fn a_host_serves_when_enabled_and_staged_or_when_allowed() {
        let image = path("build/hgs.exe");
        assert_eq!(admit(true, true, false, &image), Ok(()));
        assert_eq!(admit(false, true, false, &image), Err(Refusal::NotEnabled));
        assert_eq!(
            admit(true, false, false, &image),
            Err(Refusal::Unstaged(image.clone()))
        );
        for (enabled, staged) in [(false, false), (true, false), (false, true)] {
            assert_eq!(admit(enabled, staged, true, &image), Ok(()));
        }
        assert_eq!(Refusal::NotEnabled.exit_code(), EXIT_NOT_ENABLED);
        assert_eq!(Refusal::Unstaged(image.clone()).exit_code(), EXIT_UNSTAGED);
        let message = Refusal::NotEnabled.to_string();
        assert!(message.contains("hgs session-host install"), "{message}");
        assert!(message.contains(ALLOW_UNSTAGED), "{message}");
        let message = Refusal::Unstaged(image.clone()).to_string();
        assert!(message.contains(&image.display().to_string()), "{message}");
        // A launcher tells the two refusals from other failures.
        assert!(launch_refusal(EXIT_NOT_ENABLED)
            .unwrap()
            .contains("not enabled"));
        assert!(launch_refusal(EXIT_UNSTAGED).unwrap().contains("staged"));
        for code in [0, 1, 2, 3, 101] {
            assert_eq!(launch_refusal(code), None, "{code}");
        }
    }

    #[test]
    fn a_call_starts_the_staged_image_or_an_isolated_override() {
        let current = Some(path("home/.local/share/hgs/host/0123456789abcdef/hgs.exe"));
        let build = path("build/hgs.exe");
        assert_eq!(
            call_image(None, false, true, current.clone()),
            Ok(current.clone().unwrap())
        );
        assert_eq!(
            call_image(None, true, true, current.clone()),
            Ok(current.clone().unwrap())
        );
        let error = call_image(None, false, false, current.clone()).unwrap_err();
        assert!(error.contains("not enabled"), "{error}");
        let error = call_image(None, false, true, None).unwrap_err();
        assert!(error.contains("no session host image is staged"), "{error}");

        assert_eq!(
            call_image(Some(build.clone()), true, false, None),
            Ok(build.clone())
        );
        let error = call_image(Some(build.clone()), false, true, current.clone()).unwrap_err();
        assert!(error.contains("isolated"), "{error}");
        let error = call_image(Some(PathBuf::from("hgs.exe")), true, true, None).unwrap_err();
        assert!(error.contains("absolute"), "{error}");
    }
}
