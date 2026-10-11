# Public release catalogs and mirrors

GitHub Releases are the provenance origin. The static website mirrors verified
release bytes and serves anonymous catalogs at:

- `https://zerus.dev/updates/v1/android/dev.json`
- `https://zerus.dev/updates/v1/desktop/stable.json`
- `https://zerus.dev/updates/v1/desktop/nightly.json`

The schema is specified in [mobile architecture](mobile-architecture.md#public-distribution-and-update-channels).
These catalogs do not use relay credentials. The mirror does not build, publish
to GitHub/AUR, dispatch hosted CI, or install anything on a client.

## Seal an Android development release

Build and verify the APK using the persistent development signing identity. Keep
keystore paths and passwords in private local configuration, outside the source
archive. Development APKs must be universal, non-debuggable, at most 128 MiB,
and retain package ID `app.zerus.mobile` and the existing signing certificate.
Use a larger `versionCode` for every newly published APK, including rebuilds of
the same display version.

Freeze the exact source tree used for the build into a reviewed `.tar.gz` source
snapshot. Include new source files and the uncommitted changes, honor export-ignore for `.beads`, `.agents`, `.github`,
`design`, agent instruction files and other excluded metadata, exclude ignored
runtime/build/credential files, and audit the frozen archive for secrets before
publication. `git archive HEAD` alone does **not** describe an uncommitted build.
An uncommitted development release records the base commit plus
`source_dirty: true` and the exact checked source archive. Its release notes must
say **Development source snapshot; includes uncommitted changes**. The commit
alone is not the build provenance. Do not create a commit to conceal this fact.

Example with synthetic paths (use the SDK tools from the verified build):

```sh
python3 scripts/prepare-mobile-release.py \
  --apk artifacts/release/zerus-0.1.8.apk \
  --source-snapshot artifacts/release/zerus-0.1.8-source.tar.gz \
  --source-commit 0123456789abcdef0123456789abcdef01234567 \
  --source-dirty --channel dev \
  --aapt2 /opt/android-sdk/build-tools/36.0.0/aapt2 \
  --apksigner /opt/android-sdk/build-tools/36.0.0/apksigner \
  --output artifacts/release/android-dev-0.1.8
```

The helper extracts the real APK package/version/SDK metadata, verifies its
signature, checks frozen copies again, and creates `release-info.json` and
`SHA256SUMS`. It refuses an existing candidate directory. The source archive must
already have been audited; the helper validates bytes, not the correspondence
between an arbitrary archive and a prior build.

Publish the reviewed frozen candidate within the existing release authorization.
Use an authenticated paginated release lookup before creating or resuming a draft:

```sh
gh api --paginate --slurp 'repos/ufna/zerus/releases?per_page=100' \
  | jq '[.[][] | select(.tag_name=="android-dev-0.1.8") | {id, tag_name, draft, prerelease, target_commitish}]'
```

A draft may exist even when `/releases/tags/<tag>` returns 404. During the reviewed
0.1.8 publication, the tag lookup returned 404 while the authenticated list found
draft release `407980374` with its uploaded assets. **Do not blindly recreate a
release after a tag lookup returns 404.** If the authenticated paginated lookup
fails, stop and resolve access first. If it returns an existing matching draft,
resume that exact ID; if it returns an already published release, verify it rather
than publishing again. Multiple matches require investigation.

Create a draft only when the successful authenticated lookup confirms no matching
release. Keep the target equal to the candidate's disclosed source base:

```sh
gh release create android-dev-0.1.8 artifacts/release/android-dev-0.1.8/* \
  --repo ufna/zerus \
  --target 0123456789abcdef0123456789abcdef01234567 \
  --draft --prerelease --latest=false --title 'Android 0.1.8' \
  --notes-file artifacts/release/android-dev-0.1.8-notes.txt
```

Repeat the authenticated paginated lookup to obtain the created draft's exact
positive integer ID. The following uses the reviewed 0.1.8 ID as an example;
subsequent publications must use their own verified ID:

```sh
ZERUS_RELEASE_ID=407980374
gh api "repos/ufna/zerus/releases/$ZERUS_RELEASE_ID" \
  --jq '{id, tag_name, draft, prerelease, target_commitish, assets: [.assets[] | {name, size, digest, state}]}'
```

Verify that the response ID and tag match the selected candidate, `draft` and
`prerelease` are true, and `target_commitish` matches its disclosed base. Compare
the exact asset set, every uploaded size and `sha256:` digest with the frozen
candidate, including `release-info.json` and `SHA256SUMS`; all assets must be
`uploaded`. Do not rebuild or change a frozen candidate to repair an API lookup.
Only after these checks, publish that exact release ID without changing GitHub's
latest stable desktop release:

```sh
gh api --method PATCH "repos/ufna/zerus/releases/$ZERUS_RELEASE_ID" \
  -F draft=false -F prerelease=true -f make_latest=false \
  --jq '{id, tag_name, draft, prerelease, published_at}'
gh api repos/ufna/zerus/releases/latest --jq '{tag_name, id}'
```

Verify the published ID/tag again and that the latest stable desktop tag remains
unchanged. The reviewed 0.1.8 publication retained `v0.37.0` as latest.

The tag points to the disclosed base commit. The mirror requires Android dev
releases to be prereleases with an `android-` or `mobile-` tag and checked source
snapshot metadata. A published asset name is immutable: use a new release/tag and
larger Android version code for changed bytes. This command does not dispatch CI.
Stable Android releases require clean committed provenance and a stable channel.

## Desktop release compatibility

The pull publisher consumes current main's sealed `release-info.json`,
`SHA256SUMS`, `.SRCINFO` recipes and actual GitHub asset digests. Stable tags are
`v<version>`; nightly tags are `nightly-<workflow_run_id>` with exact source and
workflow version provenance. It resolves each tag to its real source commit and
never substitutes GitHub's generic `/latest` endpoint for independent channels.

AUR versions are read from the published recipe archive, including epoch and
package release. In particular `zerus-git` is a VCS recipe, not an invented copy
of the stable version. The feed describes the release recipes; live AUR
propagation can lag GitHub publication. Existing main's release/AUR publication
workflow remains responsible for those releases and credentials.

Desktop update checks are asynchronous and throttled to one automatic attempt per
channel per 24 hours. A periodic timer checks the throttle in a continuously
running GUI. Settings → Updates exposes a persisted automatic-check preference;
disabling it cancels automatic requests and pending automatic notifications,
while the manual check remains available. Selecting a channel checks it immediately
only when automatic checking is enabled and its 24-hour throttle permits it. Bounded verified public metadata is cached per channel;
failed checks preserve the known release and explicitly mark it stale. Manual
checks remain available in Settings → Updates. A confirmed newer release adds
an Update available entry in the tray menu and workspace status bar. A fresh
verified comparison posts one system notification per release; cached data alone
does not post a notification. Each update click opens Settings → Updates and
never enters the session-attention route.

Pacman ownership is resolved for the canonical running GUI executable, followed
by the actual installed package version. Arch package comparison uses `vercmp`.
Supported AUR families are `zerus`, `zerus-git`, `zerus-ade-bin` and
`zerus-ade-nightly-bin`. The UI only copies instructions, never invokes privilege
escalation or replaces owned files. Example full-system commands:

```sh
paru -Syu zerus-ade-bin
paru -Syu --devel
```

Replace `paru` with your configured AUR helper if needed. Review the full system
upgrade. VCS/source installs require deliberate source updates and rebuilds;
unknown owners receive download/build instructions. Strict plain stable product
versions can identify a newer release for local builds, but equality, patched or
nightly versions never imply that an unowned executable is up to date. Arch is
detected independently by bounded parsing of os-release, never by sourcing it.
Available yay/paru helpers supply safe copied commands. An unowned local build
on Arch gets an explicitly optional AUR installation command rather than a false
package-ownership claim; no command is executed by Zerus. There is currently no
published macOS installer. Updates never stop agents, tmux or resident adapters.

## Desktop mobile-connection settings

Settings → Mobile connection edits the existing private connector configuration.
Credentials stay masked and saves are atomic. Saving is independent of applying:
configuration changes take effect only when the connector is explicitly restarted
using the displayed commands. Zerus does not restart the connector or native
agents. The panel preserves the managed relay's receipt identity; a different
relay or credential requires a new unused private receipt directory, so old
receipts cannot authorize replay against another workspace.

## Pull and mirror

Install the two adjacent files `scripts/publish-update-feed.py` and
`scripts/update_feed.py`. They require only Python 3.11+ standard-library modules.
Run each channel independently so an unavailable channel cannot starve another:

```sh
python3 /opt/zerus-updates/publish-update-feed.py \
  --root /var/lib/zerus-updates --channel desktop/stable
python3 /opt/zerus-updates/publish-update-feed.py \
  --root /var/lib/zerus-updates --channel desktop/stable --execute
```

Omitting `--execute` performs a read-only validation/download dry-run without
creating the mirror root or publishing files. Repeat independently for
`desktop/nightly` and `android/dev`. A dedicated unprivileged writer should own
the root, with nginx having read-only access. Deny dotfiles in the web server.
The job uses a nonblocking root lock, bounded assets/metadata/redirects and a
20 GiB retained disk budget (`--max-mirror-bytes` can override it). A service
runtime deadline should bound the entire job; each network socket has a 30-second
timeout. A fifteen-minute timer leaves additional anonymous API headroom. Failed jobs retain the old catalog;
there is no automatic deletion of immutable historical assets.

Public releases can be read anonymously. For larger catalogs, optionally supply
a read-only GitHub credential using `GITHUB_TOKEN_FILE` (a systemd credential
path), `GH_TOKEN` or `GITHUB_TOKEN`. Only `api.github.com` requests receive it;
redirected asset/CDN requests never receive authorization. Credentials and signed
CDN URLs are not logged or placed in published files. No GitHub deploy secret is
needed on the website.

Downloaded assets are checked against GitHub sizes/digests and sealed checksums.
Existing assets are streamed and rehashed for reuse, avoiding repeated binary
downloads. A differing immutable asset, tag mismatch, same-code Android
replacement, malformed/unsealed release, or rollback fails closed. Assets,
including `SHA256SUMS`, are durably linked before the small catalog pointer is
atomically replaced. Clients receive only `/downloads/<tag>/<name>` URLs.

## Local validation and current-main integration

```sh
python3 -m unittest discover -s tests -p test_update_feed.py -v
python3 -m unittest discover -s tests -p test_mobile_release.py -v
python3 scripts/export-desktop-updates.py \
  --main /path/to/current-main --output artifacts/desktop-updates-main
cmake -S artifacts/desktop-updates-main/tray \
  -B artifacts/desktop-updates-main/build -G Ninja -DBUILD_TESTING=ON
cmake --build artifacts/desktop-updates-main/build \
  --target hgs-tray test_updates test_sessionswindow
ctest --test-dir artifacts/desktop-updates-main/build -R updates --output-on-failure
```

The export uses current main read-only and adds only the update controller,
widget, test and exact integration anchors. It preserves main's product version
and unrelated desktop features. The main checkout, installed GUI and native
processes are untouched. Deploy only the reviewed current-main GUI within the existing update authorization.
An older mobile worktree GUI must not replace a newer desktop. Source commits,
pushes and hosted CI dispatch remain separately controlled.
