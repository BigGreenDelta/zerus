#!/usr/bin/env python3
"""Submit a published stable Windows zip to the Windows Package Manager community repository.

The manifest names the immutable GitHub release asset; its hash must match both the
release's sealed SHA256SUMS and GitHub's asset digest. A version already in
microsoft/winget-pkgs, or already proposed there, is never submitted again.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import urllib.error
import urllib.parse
import urllib.request

from release_common import REPOSITORY, require
import windows_portable
import winget_manifest

UPSTREAM = "microsoft/winget-pkgs"
API = "https://api.github.com/"


class GitHub:
    def __init__(self, token):
        self.token = token

    def request(self, method, path, body=None, raw=False):
        """Returns (status, payload); 404 and 422 are answers, other errors raise."""
        url = path if path.startswith("https://") else API + path
        headers = {"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28",
                   "User-Agent": "zerus-publish-winget"}
        if self.token and url.startswith(API):
            headers["Authorization"] = "Bearer " + self.token
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                content = response.read(1024 * 1024 + 1)
                require(len(content) <= 1024 * 1024, f"Oversized GitHub response: {path}")
                return response.status, content if raw else json.loads(content or b"null")
        except urllib.error.HTTPError as error:
            if error.code in (404, 422):
                return error.code, None
            raise ValueError(f"GitHub {method} {path} failed with HTTP {error.code}.") from None


def published_zip(github, version):
    """The release asset hash, confirmed by SHA256SUMS and GitHub's digest, and the release date."""
    status, release = github.request("GET", f"repos/{REPOSITORY}/releases/tags/v{version}")
    require(status == 200, f"Stable release v{version} is not published.")
    require(not release["draft"] and not release["prerelease"], f"v{version} is not a published stable release.")
    assets = {asset["name"]: asset for asset in release["assets"]}
    name = windows_portable.asset_name(version)
    require(name in assets and "SHA256SUMS" in assets, f"v{version} has no Windows zip and SHA256SUMS.")
    status, sums = github.request("GET", assets["SHA256SUMS"]["browser_download_url"], raw=True)
    require(status == 200, "Cannot download SHA256SUMS.")
    lines = [line.split() for line in sums.decode().splitlines() if line.strip()]
    hashes = [parts[0] for parts in lines if len(parts) == 2 and parts[1].lstrip("*") == name]
    require(len(hashes) == 1, f"SHA256SUMS does not name {name} exactly once.")
    require(assets[name].get("digest") == "sha256:" + hashes[0], f"GitHub's digest of {name} differs from SHA256SUMS.")
    return hashes[0], release["published_at"][:10]


def proposal(github, login, version, branch):
    """An open or merged proposal of this version, or None; a closed one needs a person."""
    status, _ = github.request("GET", f"repos/{UPSTREAM}/contents/{winget_manifest.PACKAGE_PATH}/{version}?ref=master")
    if status == 200:
        return "already in " + UPSTREAM
    head = urllib.parse.quote(f"{login}:{branch}")
    _, pulls = github.request("GET", f"repos/{UPSTREAM}/pulls?state=all&head={head}")
    for pull in pulls or []:
        if pull["state"] == "open" or pull.get("merged_at"):
            return pull["html_url"]
        raise ValueError(f"{pull['html_url']} was closed without merging; inspect it before submitting again.")
    query = urllib.parse.quote(f'repo:{UPSTREAM} is:pr is:open in:title "{winget_manifest.IDENTIFIER}" "{version}"')
    _, found = github.request("GET", f"search/issues?q={query}")
    for item in (found or {}).get("items", []):
        if version in item["title"].split():
            return item["html_url"]
    return None


def submit(github, version, files, run_url=""):
    status, user = github.request("GET", "user")
    require(status == 200, "The winget token cannot read its GitHub account.")
    login = user["login"]
    fork = f"{login}/winget-pkgs"
    status, repository = github.request("GET", f"repos/{fork}")
    require(status == 200 and repository.get("fork") and repository.get("parent", {}).get("full_name") == UPSTREAM,
            f"Fork {UPSTREAM} as {fork} first.")
    branch = f"{winget_manifest.IDENTIFIER}-{version}"
    existing = proposal(github, login, version, branch)
    if existing:
        print(f"{winget_manifest.IDENTIFIER} {version} is already proposed: {existing}")
        return existing
    # Base the commit on the fork's master once it matches upstream: upstream moves on
    # every few minutes, and the fork must hold every object the new tree refers to.
    status, _ = github.request("POST", f"repos/{fork}/merge-upstream", {"branch": "master"})
    require(status == 200, f"Cannot synchronize {fork} master with upstream; it must not carry its own commits.")
    status, head = github.request("GET", f"repos/{fork}/git/ref/heads/master")
    require(status == 200, f"Cannot read {fork} master.")
    base = head["object"]["sha"]
    _, commit = github.request("GET", f"repos/{fork}/git/commits/{base}")
    tree = [{"path": path, "mode": "100644", "type": "blob", "content": text} for path, text in sorted(files.items())]
    status, created = github.request("POST", f"repos/{fork}/git/trees", {"base_tree": commit["tree"]["sha"], "tree": tree})
    require(status == 201, "Cannot create the manifest tree in the fork.")
    status, created = github.request("POST", f"repos/{fork}/git/commits", {
        "message": f"{winget_manifest.IDENTIFIER} {version}", "tree": created["sha"], "parents": [base]})
    require(status == 201, "Cannot create the manifest commit in the fork.")
    status, _ = github.request("POST", f"repos/{fork}/git/refs", {"ref": f"refs/heads/{branch}", "sha": created["sha"]})
    if status == 422:
        # A branch left by an interrupted run without a proposal: point it at the new commit.
        status, _ = github.request("PATCH", f"repos/{fork}/git/refs/heads/{branch}", {"sha": created["sha"], "force": True})
    require(status in (200, 201), f"Cannot create branch {branch} in the fork.")
    status, root = github.request("GET", f"repos/{UPSTREAM}/contents/{winget_manifest.PACKAGE_PATH}?ref=master")
    title = (f"Update: {winget_manifest.IDENTIFIER} to {version}" if status == 200
             else f"New package: {winget_manifest.IDENTIFIER} version {version}")
    body = (f"## Description\n\n{winget_manifest.IDENTIFIER} {version}: the portable Windows x86_64 zip of the "
            f"[Zerus {version}](https://github.com/{REPOSITORY}/releases/tag/v{version}) stable release.\n"
            f"Generated by `scripts/publish-winget.py` from the release's SHA256SUMS and GitHub asset digest"
            + (f" in [this workflow run]({run_url})" if run_url else "") + ".\n\n"
            "## Manifest Checklist\n\n"
            "- [x] Checked that there aren't other open pull requests for the same manifest update/change\n"
            "- [x] This PR only modifies one (1) manifest\n"
            f"- [x] Manifest conforms to the {winget_manifest.MANIFEST_VERSION} schema\n")
    status, pull = github.request("POST", f"repos/{UPSTREAM}/pulls", {
        "title": title, "head": f"{login}:{branch}", "base": "master", "body": body, "maintainer_can_modify": True})
    require(status == 201, "Cannot open the winget-pkgs pull request.")
    print(f"Opened {pull['html_url']}")
    return pull["html_url"]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True, help="published stable version, without v")
    parser.add_argument("--output", type=Path, help="write the manifests here instead of submitting them")
    arguments = parser.parse_args()
    winget_manifest.require_version(arguments.version)
    token = os.environ.get("WINGET_TOKEN", "")
    github = GitHub(token)
    sha256, date = published_zip(github, arguments.version)
    files = winget_manifest.manifests(arguments.version, sha256, date)
    if arguments.output:
        for path, text in files.items():
            target = arguments.output / Path(path).name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8", newline="\n")
        print(f"Wrote {len(files)} manifests for {winget_manifest.IDENTIFIER} {arguments.version} to {arguments.output}")
        return
    require(token, "Set WINGET_TOKEN to a token of the account that owns the winget-pkgs fork.")
    run_url = ""
    if os.environ.get("GITHUB_RUN_ID"):
        run_url = f"https://github.com/{REPOSITORY}/actions/runs/{os.environ['GITHUB_RUN_ID']}"
    submit(github, arguments.version, files, run_url)


if __name__ == "__main__":
    try:
        main()
    except ValueError as error:
        sys.exit(f"error: {error}")
