"""A local bare Git repository standing in for the ufna/homebrew-zerus tap."""
from pathlib import Path
import subprocess


def make_tap_remote(root, files=None):
    remote = Path(root) / "tap.git"
    seed = Path(root) / "tap-seed"
    subprocess.run(["git", "init", "--quiet", "--bare", "-b", "main", str(remote)], check=True)
    subprocess.run(["git", "init", "--quiet", "-b", "main", str(seed)], check=True)
    for name, content in {"README.md": "tap\n", "Casks/.gitkeep": "", **(files or {})}.items():
        (seed / name).parent.mkdir(parents=True, exist_ok=True)
        (seed / name).write_text(content)
    identity = ["-c", "user.name=Test", "-c", "user.email=test@example.invalid", "-c", "commit.gpgsign=false"]
    subprocess.run(["git", "-C", str(seed), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(seed), *identity, "commit", "--quiet", "-m", "seed"], check=True)
    subprocess.run(["git", "-C", str(seed), "push", "--quiet", str(remote), "main"], check=True)
    return remote


def remote_file(remote, path):
    result = subprocess.run(["git", "--git-dir", str(remote), "show", f"main:{path}"], capture_output=True, text=True)
    return result.stdout if result.returncode == 0 else None
