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

A Debug Qt6.12 Sessions window run aborts at SessionList teardown with
`Called object is not of the correct type`; its base-revision behavior is not
established. The same selected tests pass in the normal Release configuration.
This Debug failure remains separate evidence and is not a Windows pass claim.

Root must review the Windows OS adapter, lifecycle/config management, GUI,
portable packaging and checks.yml diff before a hosted run. The concrete native
gate is the `windows_gateway` Rust test, `test_windows_mobile_gateway.py` on an
isolated Windows profile outside a job that forbids breakaway, all Windows Qt
suites, and the newly packaged ZIP's `--run --selftest` checks. The command lines
are in CONTRIBUTING.md; the existing hosted Windows job wires them without any
new credentials, runner class or cache limit. No hosted run has been requested.
Keep zerus-eh8b open until native Windows behavior is qualified and root accepts
the source. Publication and owner deployment need separate root instructions.
