# Windows mobile gateway

Issue: zerus-eh8b (onboarding: zerus-tmc0). Base: cd28322d.

A Windows PC is an outbound gateway itself. It does not need WSL or another
computer. Reuse the Rust connector, relay API v1 and the independent ConPTY
session host. Configuration remains `hgs/mobile/connector.json`; the SQLite
journal, binding hash, durable request UUIDs and uncertain delivery receipts
retain their existing formats. No migration or owner installation is implied.

## Platform boundaries found

- The connector has a non-Unix compile error, Unix permission/ownership and
  flock calls, Unix argument decoding and Unix signal registration.
- The desktop configuration store supports only Unix and hides Mobile connection
  on Windows. Phones points an empty list at that hidden page.
- The Windows portable package contains no connector. Its native CLI advertises
  Unix mobile path, speech and peer markers even where Windows refuses them.
- The existing Windows host already owns persistent Codex, Claude and Kimi
  sessions. hgs.exe already owns the Windows SSH pipe adapter and lets the
  session host break away from short-lived CLI jobs.

## First supported capability set

List and inspect existing supported local sessions; paged history and archives;
send and answer with existing native identity guards; interrupt and supported
session actions; terminal reads/input; read marks and shared notification state.
Desktop session creation remains available. Each native inspection still decides
the actions of that exact session. Do not offer DeepSeek/OpenCode, voice/speech,
one-hop gateway routing, or mobile folder/worktree/Git/file operations in this
first Windows transport: those require separate native and mobile path contracts.
Refuse unsupported requests after durable duplicate lookup, before native work.
Unix capability discovery and one-hop routing stay unchanged.

## Credentials and lifecycle

Use a small Windows OS adapter for owner/DACL checks, reparse-point rejection,
single-link files and OS file locks. Create protected directories/files granting
only the current user, SYSTEM and Administrators; refuse unsafe existing data
rather than repairing permissions. Pin traversed directories while writing.
The desktop calls the bundled connector's bounded JSON management interface
asynchronously. Native session and SSH operations always go through hgs.exe. Credentials travel over stdin, never argv, QSettings or diagnostics.
Return only redacted configuration plus an exact revision for guarded updates.
Saving keeps old receipts and requires a fresh directory for a new relay binding.

Start is explicit, per user, without a machine-wide service or automatic login
task. Launch the connector detached from GUI/CLI jobs. Its exclusive state lock
prevents duplicate writers. Stop uses a private, generation-bound request checked
by the connector, finishes the in-flight native call and journals its outcome.
Never kill a PID by a stored number, kill a process tree, or stop the session host.
Closing the GUI leaves the connector and native agents running. Configuration
changes are refused while its connector is running. A later startup-at-login
feature needs an explicit owned registration and is outside this first slice.

## Packaging and qualification

Bundle the tested connector executable and its dependency notices in the portable
ZIP. Keep third-party DLLs unchanged. Add local Windows build/check instructions
and wire qualification into the existing hosted Windows job only as source;
root reviews workflow edits before any push or CI run.

Run pure capability/no-replay controls and Unix connector regressions locally;
cross-check Windows compilation where the target/toolchain is present. Add an
isolated Windows suite for private configuration, duplicate starts, graceful stop,
real HTTP relay claims/receipts and the independent host's survival. No owner
configuration, live phones or native sessions may be used. An unavailable Windows
runtime or a skipped suite is a qualification gap, never a pass.


## Candidate review gate (2026-10-20)

This implementation is a source candidate, not a shipped or Windows-qualified
feature. No live connector, owner installation, phone or emulator was used.

Local evidence: Rust connector unit/integration vectors and all five Python/Rust
journal handoffs; portable Windows policy/no-replay controls; black-box startup,
journal and runtime/TLS/stop suites; phone CLI tests; packaging and CI security
contracts; the focused Windows settings/relay/phone Qt suites and three integrated
Sessions window cases in the repository's normal Release configuration. The
Windows adapter, connector and CLI type-check and lint using the existing MSVC
cross-check recipe (C compilation is stubbed, so this is not native execution).
Six unchanged relay codec contracts also pass.

The minimal Debug Qt6.12 Sessions window case
`mobileConnectionSettingsOfferTheSharedDeliveryChoice` aborts on both the frozen
candidate and exact base `cd28322d`, with the same toolchain and private offscreen
environment. Both bounded GDB traces show `modelReset` calling
`SessionList::scheduleRowExpandButtons` during QWidget child deletion after the
derived lifetime ends. SessionList and WorkspaceList sources are identical.
Original failures and actual base results are retained. This preexisting defect
has a separate repair scope, `zerus-eh8b.3`; Release passes are not lifetime proof
or Windows qualification.

Root must review the Windows OS adapter, lifecycle/config management, GUI,
portable packaging and checks.yml diff before a hosted run. The concrete native
gate is the `windows_gateway` Rust test, `test_windows_mobile_gateway.py` on an
isolated Windows profile outside a job that forbids breakaway, all Windows Qt
suites, and the newly packaged ZIP's `--run --selftest` checks. The command lines
are in CONTRIBUTING.md; the existing hosted Windows job wires them without any
new credentials, runner class or cache limit. No hosted run has been requested.
Keep zerus-eh8b open until native Windows behavior is qualified and root accepts
the source. Publication and owner deployment need separate root instructions.


## Independent source review corrections

The original 58-source packet is preserved in commit `35e680fa`, with every
postimage verified against manifest `5afda4dc`. Corrections form a separate child;
the packet and its evidence remain unchanged.

- Directory ACL admission validates inherit-only allow recipients too. Such an
  ACE can grant access to future SQLite sidecars even when the existing DB has
  its own protected private ACL. Unsafe directories are refused before writes;
  no permissions are repaired. Inherit-only ACEs on files remain inert.
- Start passes the observed configuration revision to child admission. The
  child compares it while holding the management guard, before opening the
  journal or constructing a runtime. Its control record carries that revision;
  parent readiness requires the same revision. Already-running Start remains
  idempotent for that revision. A held admission/change/release control exercises
  the common routine locally; the real Windows Save/admission ordering case is
  present but UNRUN here.
- The settings panel validates boolean types, configured revision, server and
  state identity, and running generation before adopting results or clearing
  replacement text. Start and Stop bind the prior identity; the real
  `stopping`+generation acknowledgement stays locked pending a valid explicit
  reload. Malformed responses preserve unknown state and replacement text.
- Windows archive pages and direct targets use the same Codex/Claude/Kimi rule.
  The current authoritative archive reader already rejects other providers
  before indexing and counting. Scoped lookup supplies the provider from the
  archive record. The connector refuses an inconsistent mixed/unknown-provider
  page as a whole, with no row admission and no invented filtered totals or
  cursors. Successful pages retain their exact native totals/cursors. Every
  Windows direct archive dispatch checks authoritative lookup even after a
  cached admission; live dispatch checks fresh native provider and identity.
  Durable duplicate lookup remains first. Unix calls/responses are unchanged.
- The native fixture reserves custody before Start, including launcher failure
  or timeout. Recovery requires its private revision/generation/control and the
  unique copied process image, then pins a process handle and rechecks control.
  Cleanup confirms handle exit; failed final waits preserve both handle custody
  and the private root. Forced termination uses only that verified handle and
  fails the graceful-stop gate. No PID/tree fallback is allowed. The separate
  workflow gate uses `outside-job.py --custody-only`, without `--end`; timeout
  retains the wrapper/fixture and reports an unknown outcome.

## Concrete native gate awaiting independent re-review

All actual Windows checks below remain **UNRUN**. Root must review the child
source, workflow and portable packaging before granting any execution. Use a
disposable Windows user/profile with an already provisioned toolchain, exact
reviewed source and no owner installation, services, agents, keys or phone.
Set `CARGO_BUILD_JOBS=2` and `ZERUS_BUILD_JOBS=2` for the approved local gate.

1. Build/lint the connector and CLI natively. Run the two
   `windows_management::tests` library cases explicitly for actual lock
   contention and expiry, then the six `windows_gateway` integration cases,
   including the unsafe inheritable directory with private existing DB and lock
   files/absent sidecars, held Start revision A versus Save B, and same-revision
   already-running Start, plus independent temporary fixture roots. Retain both
   raw logs. Zero tests/skips do not qualify
   either suite.
   Also select `-p zerus-windows-private --lib` through the connector manifest
   and its existing lockfile. Its two launch-builder cases must actually report
   2 passed, 0 failed and 0 skipped on Windows; retain
   `windows-gateway-launch-builder.log`. Cross-target checks only type-check
   those test bodies and do not satisfy this requirement.
2. Run the Windows Qt settings fixtures, including all malformed status/save/
   start/stop cases and real successful response forms. Run supported native
   host/agent contracts only under a separately reviewed runtime scope.
3. Build the exact fake agent and use the isolated HTTPS gateway fixture via
   the custody-only outside-job command in CONTRIBUTING. It must confirm the
   connector process handle exits and the same native host/session survives
   graceful Stop. On any custody uncertainty, retain the root and report the
   exact unknown identity; never broaden cleanup or retry input.
4. Assemble the portable ZIP from those same reviewed binaries and pinned ConPTY
   pair. Inspect its contents/notices and run `check-windows-portable.py --run
   --selftest` in its private profile. That selftest stages a host image only in
   the temporary profile; it does not start an owner host. Preserve exact source,
   binary/ZIP hashes and raw results. A partial/skipped gate remains incomplete.

No source-only, Linux, stubbed compilation or Release smoke result substitutes
for this gate. Keep `zerus-eh8b` and `zerus-tmc0` open until acceptance.
