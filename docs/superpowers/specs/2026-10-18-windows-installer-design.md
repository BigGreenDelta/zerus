# Windows code signing, per-user installer and self-update

## Intent

Windows ships as an unsigned portable zip (stable releases after 0.42.2 and
nightlies), and winget installs the same zip as `ufna.Zerus`. Owner direction
2026-10-17 deferred the rest of the Windows distribution plan (epic
`zerus-qxdx`). On 2026-10-18 the owner directed work on every unfinished item
(`zerus-pvfa`), so this document turns the deferred plan into:

1. the signing decision, with costs and legal and identity terms the owner must
   choose between (`zerus-qxdx.1`);
2. a per-user installer built from the unchanged portable payload
   (`zerus-qxdx.3`);
3. a provider-independent check of the owner rule on bundled binaries
   (`zerus-qxdx.2`);
4. self-update for installer builds, behind a trust root that the owner creates
   (`zerus-qxdx.4`).

Owner rule (2026-10-17): «dll класть можно, нельзя на них подпись менять!»
Bundling third-party DLLs and EXEs is fine, but their bytes, and therefore their
signatures, never change. Only files built from this repository (`hgs.exe` and
`hgs-tray.exe`) may ever be signed by us.

## Signing decision (owner choice)

Facts were checked on 2026-10-18 against the sources named in each row. Prices
exclude VAT.

| Option | Cost | Who can use it | Publisher Windows shows | Per-release work | Fits CI-only signing |
|---|---|---|---|---|---|
| SignPath Foundation | Free | OSI-licensed OSS (Zerus is MIT) | "SignPath Foundation" | A team member approves every release in SignPath | Yes: signs CI artifacts from GitHub-hosted builds |
| Azure Artifact Signing, Basic | about $9.99/month (5,000 signatures) on a paid Azure subscription | Organizations in the US, Canada, EU, UK, Australia, New Zealand, Japan, South Korea, Singapore, Switzerland, Norway or Israel; individuals in the US or Canada only | The validated legal name; for an individual also city, state and country | None after onboarding | Yes: GitHub Action with a service principal |
| OV certificate from a CA (Sectigo, DigiCert, SSL.com) | $150–300/year, plus a cloud HSM fee for CI | Worldwide, subject to the CA's own restrictions | The validated legal name of the person or company | None | Only with a cloud HSM: a USB token would need a self-hosted runner, which our CI rules forbid |
| Certum Open Source Code Signing | from €69 for the set with a smart card and reader | Individual OSS developers | "Open Source Developer" plus the person's name | The owner signs locally with the card | No: the card is physical (a cloud variant of this tier was not confirmed) |
| Microsoft Store (MSIX) | Free; the Store re-signs | Worldwide | The Store's Microsoft signature | Store certification per submission | Not applicable: a separate channel |
| Unsigned (today) | Free | — | "Unknown publisher" | None | — |

Common facts:

- No option gives instant SmartScreen trust. Microsoft: "Azure Artifact Signing
  does not provide instant SmartScreen trust", OV is "functionally equivalent",
  and EV lost the bypass in 2024 ([code signing options], updated 2026-08-29).
  Reputation builds when consecutive releases keep the same publisher.
- An OV certificate's key must sit on an HSM or token (since June 2023). Its
  validity is at most 460 days for certificates issued from 2026-03-01.
- Azure identity validation takes 1–20 business days and uses a government ID.
  Artifact Signing does not issue EV certificates and does not support custom
  names: "CN values must always be the legal entity's validated name"
  ([Artifact Signing FAQ], [quickstart]).

SignPath Foundation terms ([signpath.org/terms], a page marked as a draft):

- "Every release needs manual approval for signing."
- "All team members must use multi-factor authentication for both SignPath and
  source code repository access."
- "A code signing policy must be specified on the project's home page", naming
  the team roles and including privacy information.
- "The project may not contain any proprietary, non open-source component",
  although "System Libraries are allowed". "You may include unsigned binaries of
  upstream OSS projects, e.g. DLL files", but SignPath may later "require that
  signed packages only include signed program and library files".
- The bundled MSVC runtime DLLs are Microsoft-signed redistributables. Under the
  owner rule they ship byte-identical. SignPath decides whether they count as
  System Libraries when the project applies.

Cadence: 11 stable desktop releases came out between 2026-10-11 and 2026-10-16,
up to 4 in one day. With SignPath, each one waits for a human approval:

- One approval per release for the zip.
- Two approvals once the installer exists: first the inner `hgs.exe` and
  `hgs-tray.exe`, then the installer that contains them.

Nightlies stay unsigned under SignPath, as Orca's dev channels do. Azure and OV
sign without a person and can also cover nightlies.

Identity consequences:

- SignPath keeps personal and company names out of every signature, but the code
  signing policy page names the team roles.
- Azure (individual) and OV certificates publish the signer's legal name in
  every signed file; an Azure individual certificate also shows the city, state
  and country.

Risks to self-update trust:

- A SignPath signature proves only "an OSS project signed through SignPath".
  Every SignPath Foundation project shares that publisher name, so a publisher
  check alone cannot tell a Zerus installer from another project's installer.
- A certificate in our own legal name (Azure or OV) does bind the publisher to
  Zerus.

Self-update therefore uses its own trust root (below) whatever the provider is.

Microsoft Store (MSIX) is listed for completeness. It needs a spike before it
can be recommended, because Windows ends a package's processes when it updates
the package. Children of a packaged app inherit the package context, and the
session host is such a child: it starts with `CREATE_BREAKAWAY_FROM_JOB` but
would still have to leave the package context. Until that is proven, a Store
update could end every local session, which the epic forbids.

**The owner chooses:**

1. The provider for stable releases: SignPath Foundation, Azure Artifact Signing
   (only with an eligible organization, or as an individual in the US or Canada),
   an OV certificate with a cloud HSM, or no signing for now.
2. Whether nightlies are signed. Under SignPath the recommendation is no.
3. With SignPath: who approves releases, and that every team member uses MFA.
4. Whether to create the self-update trust root below: a release secret in the
   `release` environment that only the owner creates.

Recommendation: SignPath Foundation for stable releases, unless the owner already
has an eligible legal entity. It is free, needs no personal identity in the
signature and fits our GitHub-hosted builds. The cost is one approval per stable.
Keep nightlies unsigned, and use the update key for self-update in either case.

## Per-user installer

Inno Setup 6, which the GitHub `windows-2025` image includes (6.7.1 on
2026-10-18), builds `zerus-<version>-windows-x86_64-setup.exe` in the Windows CI
job from the tested portable zip:

- **Payload.** The installer contains exactly the zip's files, byte for byte.
  The builder extracts the zip with the same safe-name checks as
  `windows_portable.inspect`. A validator then compares every installed file
  with its zip entry by SHA-256, so third-party DLLs and EXEs keep their bytes
  and signatures. The zip and its `zerus-windows-x86_64/` layout do not change,
  and winget keeps using the zip.
- **Location.** It installs per user without elevation
  (`PrivilegesRequired=lowest`) into `%LOCALAPPDATA%\Programs\Zerus`. It adds a
  Start menu shortcut and an uninstaller registered under HKCU, and nothing
  else: no autostart, no PATH change, no file associations.
- **What it never touches.** It never touches the staged CLI
  (`%USERPROFILE%\.local\bin\hgs.exe`), host images (`.local\share\hgs\host`),
  `.config\hgs`, `.local\state\hgs` or the desktop settings and drafts. Zerus
  already restages `hgs.exe` from the folder it starts from when that version is
  the same or newer, so an installed build behaves like a newly extracted zip.
- **Upgrades.** Restart Manager closes only processes that use files in the
  install folder: `hgs-tray.exe` and short-lived `hgs.exe` commands. The session
  host runs from its staged image, outside the install folder, so it and its
  sessions keep running, as when the zip is replaced.
- **Links.** If the install folder is a link, or contains a junction, symbolic
  link or other reparse point, Setup stops before it changes anything (exit
  code 7) and names the link. Such a link could lead installed files or the
  stale-file sweep anywhere the user can write, such as configuration or state.
  The sweep itself never follows or removes a link either. It runs only in a
  folder that is no link and whose `installation.json` is the installer's
  record.
- **Uninstall.** It removes only the installed files and the shortcut, and keeps
  the staged CLI, host images, configuration and sessions. The documentation
  points at `hgs.exe session-host uninstall` for disabling local sessions first.
- **Identification.** `installation.json` next to `hgs-tray.exe` names the kind
  `installer` and the version, and the GUI reads it. A portable zip has no such
  file, so it stays `portable`, and winget detection is unchanged.
- **Release assets.** Attaching the installer to releases also extends the
  release and nightly asset sets, `SHA256SUMS` and the mirror's asset family.
  The mirror is redeployed before the first release that carries the installer,
  as `zerus-5qaa` did for the zip. That is publication work for the integration
  owner. The v1 feed stays unchanged.

## Signing integration (provider-independent part)

`scripts/windows_signing.py` lists the files Zerus builds (`hgs.exe` and
`hgs-tray.exe`). It checks that every other file in a signed payload is
byte-identical to the unsigned candidate, and that nothing was added or removed.
Whichever provider the owner picks, its step runs between "unsigned candidate"
and "sealed zip", and this check gates sealing. Signing itself waits for the
decision above.

## Self-update for installer builds

Only an installer build updates itself. Portable and winget builds keep
link-only behavior.

The GUI never runs an installer unless all of the following hold:

1. **Signed manifest.** A Windows update manifest
   (`updates/v1/windows/<channel>.json`, a new path that older GUIs never read)
   carries an ECDSA P-256 signature. The GUI verifies it with Windows CNG
   (`BCryptVerifySignature`) and a public key compiled into it, so no new
   dependency is needed. Release CI signs the manifest with the private key from
   the `release` environment.
2. **Expected contents.** The manifest names this product, platform and channel.
   Its version is newer than the installed one, and its installer URL is the
   expected GitHub release asset.
3. **Exact download.** The download stays within the size bound and matches the
   manifest's size and SHA-256.
4. **Authenticode.** `WinVerifyTrust` reports a valid Authenticode signature
   whose publisher is the one compiled into the build. The manifest signature
   identifies Zerus even when the Authenticode publisher is a shared one such as
   SignPath Foundation.

A build compiled without both a manifest key and an expected publisher never
downloads or runs an installer. That covers every build until the owner decides,
and every build of a channel that is not signed, such as nightlies under
SignPath. Such a build shows the release page link, as portable builds do.

Unsigned installers are never run by self-update, and the GUI never calls an
unsigned file signed.

Applying an update:

1. Drafts are already durable. The GUI starts the installer with
   `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /RELAUNCH` and exits.
2. The installer replaces the files and starts `hgs-tray.exe` again.
3. The session host, agents and sessions keep running.
4. A failed install leaves the previous files in place, because Inno Setup rolls
   back.

[code signing options]: https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options
[Artifact Signing FAQ]: https://learn.microsoft.com/en-us/azure/artifact-signing/faq
[quickstart]: https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart
[signpath.org/terms]: https://signpath.org/terms

## Status (2026-10-18)

Implemented as isolated source:

- `packaging/windows/zerus.iss` and `scripts/package-windows-installer.py`.
- The Windows CI steps that build the installer and run
  `tests/test_windows_installer_survival.py`.
- `scripts/windows_signing.py`.

The GUI tells installer builds apart:

- When `installation.json` next to `hgs-tray.exe` holds the installer record,
  the installation kind is `installer`.
- Installer builds stay link-only, like portable builds.

`tray/src/WindowsUpdateTrust` provides the self-update checks:

- The trust anchors are the manifest key and the expected publisher. They are
  constants in its source and stay empty, so `selfUpdateConfigured()` is false
  in every build.
- `verifyManifest` checks an ECDSA P-256 SHA-256 signature, given as r and s of
  32 bytes each, through CNG. The Qt suite runs it against a test vector.
- `authenticodePublisher` uses `WinVerifyTrust` and checks revocation for the
  whole chain.

Remaining, in order:

1. The owner's signing decision and update key.
2. Signing in publication, gated by `windows_signing.changes`.
3. Publishing the installer as a release asset, together with the mirror's
   asset family, and documenting it in `docs/installation.md#windows`.
4. The signed Windows manifest endpoint.
5. The background download and apply flow, which uses these checks.
