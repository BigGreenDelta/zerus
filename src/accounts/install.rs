//! Explicit terminal action; catalog/identity reads never invoke an installer.
use super::*;

pub(super) fn run(provider: &str, dry: bool) -> Result<i32> {
    let (package, url) = match provider {
        "codex" => ("@openai/codex", ""),
        "dsh" => ("@deepseek-ai/dsh@0.2.0-rc.2", ""),
        "claude" => ("", "https://claude.ai/install.sh"),
        "kimi" => ("", "https://code.kimi.com/kimi-code/install.sh"),
        _ => return Err(Error::new(1, "Unknown agent installer.")),
    };
    if dry {
        println!(
            "{}",
            json!({"provider":provider,"package":package,"installer_url":url,"dry_run":true})
        );
        return Ok(0);
    }
    println!("Installing {provider} on this machine. After installation, return to Zerus and refresh Accounts.");
    if !package.is_empty() {
        if !platform::available("npm") {
            return Err(Error::new(
                1,
                "Install Node.js and npm on this machine, then retry Install agent.",
            ));
        }
        return platform::wait_interactive(Command::new("npm").args(["install", "-g", package]));
    }
    // Download completely before running, so a failed transfer cannot execute
    // a partial script. Only the provider's fixed HTTPS installer is accepted.
    let script = tempfile::NamedTempFile::new()?;
    let status = platform::status(
        Command::new("curl")
            .args([
                "--fail",
                "--show-error",
                "--silent",
                "--location",
                "--proto",
                "=https",
                "--proto-redir",
                "=https",
                "--connect-timeout",
                "10",
                "--max-time",
                "120",
                "--max-filesize",
                "2097152",
                "--output",
            ])
            .arg(script.path())
            .arg(url),
    )?;
    if !status.success() {
        return Err(Error::new(
            1,
            "Could not download the agent installer. Nothing was executed.",
        ));
    }
    platform::wait_interactive(Command::new("bash").arg(script.path()))
}
