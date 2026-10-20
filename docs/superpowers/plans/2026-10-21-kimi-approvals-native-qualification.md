# Kimi Activity approvals: macOS and Windows native qualification

Parent task: `zerus-pvfa.6.1`; native-platform child: `zerus-pvfa.6.1.2`.
This is a source candidate and a finite execution proposal, not a runtime grant.
Desktop/Android/iOS synthetic approval UI coverage is a separate child.

## Source scope

The existing Linux fixture's seven method bodies move unchanged into
`ApprovalCases`. Linux keeps its `/proc` proof and its original case IDs. The
macOS class runs those same bodies, through the existing lifecycle/private tmux
fixture, with the reviewed `test_kimi_native.py` exec shim and its actual
environment, argument-area, owner, image, microsecond birth and supervisor
oracle. No substitute process predicate is added. macOS compiles that same shim
once per case, fourteen times over the two versions.

Windows uses the same six case bodies and a host-specific seventh clipped-
unviewed body: there is no tmux window-size option on Windows. A short explicit
attach/detach sets the real ConPTY size to 80x12. The case verifies no client is
left, calls the actual Activity inspection, requires the pane to grow and
rejects the real request. The viewed case holds a real host attach client and
requires the original no-resize/no-answer refusal. Approval once/session,
reject, feedback reaching the model, and the dangerous-command disclosure all
read Kimi's own wire resolutions and actual filesystem/tool results.

The Windows fixture stages this source's `hgs.exe` and the repository-pinned
ConPTY pair into a new profile, runs `session-host install` there, requires
`bundled`, and copies the pinned Kimi bytes rather than hard-linking them.
Git Bash is the exact installed standard runner executable passed by the gate;
it stays in its full Git installation, with its real usr/bin Bash, MSYS runtime
and bounded bin/usr-bin DLL inventory hashed before/after. Its bin directory
precedes System32 and exact PATH resolution is checked: no WSL bash fallback
or lone copied bash.exe. This is not a complete system-DLL closure proof.
No real account, shared Kimi installation or production
service is touched. All thirteen private home/platform keys are checked and
credential prefixes/extra Kimi overrides are refused. Both model and platform
are loopback fixtures; platform requests receive the written 404. Update and
telemetry switches stay off; an offline proxy catches other ordinary HTTP(S)
traffic on Windows. This is not a kernel egress enclosure.

## Windows environment oracle: explicit new boundary

`kimi_windows_process.py` retains an owned process handle and verifies its full
creation ticks, same-user TokenUser SID, exact running image and supervisor PID.
It reads the running process's environment twice, requires equal bounded copies
and identical before/after process metadata, then checks the thirteen keys. It
does not claim an exec-time capture or immutable environment.

The environment pointer uses the internal native-x64 Windows PEB and
RTL_USER_PROCESS_PARAMETERS offsets 0x20/0x80. These are not public ABI promises.
The gate therefore runs a separate known-private-environment Python child on
the exact Windows runner first. Every key must match, no partial read/error or
skip is accepted, and no Kimi image is downloaded or started until it passes.
WOW64, another architecture, unknown layouts, inaccessible memory, duplicate
case-insensitive environment keys and changed metadata refuse. This preflight
is an oracle qualification, not one of the fourteen native Kimi cases.

API references: Microsoft's [NtQueryInformationProcess](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntqueryinformationprocess),
[PEB](https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-peb),
[RTL_USER_PROCESS_PARAMETERS](https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-rtl_user_process_parameters),
[ReadProcessMemory](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-readprocessmemory)
and [VirtualQueryEx](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualqueryex).
The public structures alone do not prove the private environment offset.

## Finite proposed hosted gate

`.github/workflows/kimi-approvals.yml` runs on main-targeting PRs for its explicit
gate/fixture/pin/ConPTY paths and supports manual dispatch. Checkout and source
identity use the exact PR head, never the synthetic merge revision. It is
contents-read and cache-free. Root must review the exact commit and workflow
before the push or dispatch that starts execution. Its only
machines are standard `macos-15` and `windows-2025`; no development-machine
runner, larger paid runner, publication environment or signing material is used.

1. Run the finite source/memory controls. These do not count as native results.
2. Build the reviewed tmux privately on macOS or fetch the already pinned ConPTY
   data package on Windows. Build the exact CLI with Rust 1.99.1, maximum two
   Cargo jobs. This is a mixed compiler/native grant, not a source-only step.
3. On Windows, qualify the environment oracle before any Kimi download.
4. Download exactly Kimi 2.1.1 and 2.2.0 from the retained official manifest
   identities. Only one HTTPS 302 from `code.kimi.com` to the identical path on
   `cdn.kimi.com` is admitted. No installer, extraction, version probe or retry.
   The added Windows sizes were observed by HEAD only: 159601440/160725792;
   hashes 28096756…/5120f698… are already in the retained official manifests.
5. Reserve the native campaign before execution. Run exactly seven named cases
   per version, in fixed order, once. Each version has one original 900-second
   suite cap and a 64 MiB log cap. There is no automatic retry or resume. Each
   case also retains its existing bounded waits and helper limits.
6. Accept only exact started/completed lists, fourteen executed Kimi cases per
   OS, zero skips/errors/failures/unexpected outcomes, unchanged source/tools/
   images and successful per-case owned cleanup. Preserve raw logs, result and
   custody JSON, copied wire/screens/records and every immutable image.

Every cleanup path attempts evidence, attached-client stop/reap, the private
host/session stop, original process-handle waits, model stop and all after-
shutdown-attempt seals. A failed step keeps roots/images; no numerical PID/tree
or broad process-name kill is used. A handle termination needed after a cleanup
deadline fails the case. A timed-out outer suite is UNKNOWN and cannot qualify.
macOS unknown roots get the existing bounded no-follow diagnostic archive.
Windows unknown roots remain in place with exact custody paths/IDs in retained
logs; a race-safe archive of an unknown Windows writer is not implemented or
claimed. The disposable VM is its outer boundary. No root is removed by an
outer fallback, and no unknown cleanup becomes a native pass.

Successful source and native qualification can close only this platform child.
Actual desktop second Confirm, Android/iOS native UI, owner profiles and public
or installed delivery remain distinct acceptance boundaries.

## First hosted attempt and source corrections

The exact `5f565f1e` attempt failed on both dedicated platforms. Windows stopped
in source/memory controls before compiler, layout preflight, Kimi download or
native execution: source reads used its default code page, a mocked image path
lacked the production reader's normalization, and an official manifest hash
check refused the checkout. That attempt did not retain the observed manifest
bytes or hash, so CRLF conversion is an unproven cause. The manifest-only Git
attribute now preserves upstream bytes under every checkout setting; a future
hash refusal reports observed hash, size and newline counts without changing
the expected hashes. Gate/source reads explicitly use UTF-8, and the workflow
selects Python UTF-8 mode. The process proof predicates are unchanged.

macOS reached the first real 2.1.1 danger-warning case. Its initial and approval
environment/image proofs passed, but the exact cwd disclosure assertion expected
`/tmp` while Kimi displayed its canonical `/private/tmp` path. That assertion
failed before an answer; the other six cases and 2.2.0 were not executed. The
helper now compares the full resolved project path; all seven semantic method
bodies remain unchanged. Per-case cleanup reported no error; the diagnostic
post-snapshot found neither declared case root nor image copy, and retained only
the empty outer runtime. This scoped observation does not establish global
process quiescence. These source corrections require a new reviewed exact head
and grant; they do not qualify either platform or replay the spent attempt.

## Second hosted attempt and Windows job boundary

On exact `ab870da3`, macOS Kimi 2.1.1 passed all seven native approval cases.
Kimi 2.2.0 stopped during the first case's setup: the strict trust chooser was
given the `/tmp` project alias while the actual complete chooser displayed
`/private/tmp`. Its initial environment/image proof passed and no trust key was
sent. The call now supplies the resolved project path; the strict chooser and
seven semantic method bodies stay unchanged.

Windows passed all fifteen source/memory controls and its actual private-child
environment-layout oracle before both pinned Kimi downloads. The first native
launch then received the host's intentional refusal to escape a CI job that
forbids breakaway. No Kimi session or approval semantics were reached. An
independent evidence step also failed to capture the absent session. The
fixture and image roots remained retained, with unchanged image seals; no
successful Windows runtime archive or complete campaign is claimed.

Each Windows native version now invokes the existing reviewed
`scripts/ci/outside-job.py --custody-only` harness. The private-child oracle
remains direct and precedes every Kimi download. The wrapper uses its existing
WMI handoff and private breakaway-enabled job only for this hosted fixture;
the product's job guard stays unchanged. It receives the exact Python child
argv and private harness environment. Its only additional PATH entry is the
fixed system Windows PowerShell directory, and that executable is included in
the tool seals. There is no `--end`, wildcard image cleanup or PID/tree kill.

The original parent 900-second wait includes wrapper handoff and suite work;
the wrapper also receives a 900-second ceiling. A timeout stops the campaign
without renewing that budget and retains unknown wrapper/fixture state. The
parent owns only its original Popen handle, not the WMI child's numeric PID.
Neither timeout nor wrapper exit proves detached native cleanup. Acceptance
still requires the exact suite result plus wrapper success and all ordinary
post-seals. The followed log and native wrapper log together must fit the
original 64 MiB cap before acceptance. Log/deadline guards remain cooperative;
they do not promise hard preemption of host I/O stalls.

Evidence may omit a screenshot only when launch was never acknowledged, its
durable binding is empty and the capture returns typed `not_found`; the exact
absence response is retained instead. An acknowledged launch, an existing
binding or another refusal still fails evidence capture. The primary launch
failure and every independent custody step remain intact. This is source-only
preparation for a separately reviewed fresh head, not permission to replay
either failed attempt.

## Windows startup envelope after the job-boundary repair

Exact `ed8ebb81` reached a real private Windows Kimi 2.1.1 process outside the
runner job. The known-child oracle and initial Kimi image, parent, creation-time,
owner and thirteen-key environment checks passed. Startup trust then failed
before input: its fixture reused the normal approval payload, whose unbound
conversation was JSON null; the unchanged CLI correctly requires a string.
No approval scenario was reached. Image seals and independent cleanup reported
no errors; the wrapper retained its failure status directory. Its scoped
survivor observation does not establish global process quiescence.

Only Windows startup trust now uses the existing Unix startup envelope with
an explicit empty expected conversation ID. Both card and binding must still
be unbound and their current runs must agree; the exact card ID/hash and
explicit trust choice are retained. Bound conversations and run mismatches
refuse before the answer call. Generic approval payloads and all product
validation stay unchanged. This source child needs separate root review and
a fresh hosted grant; the earlier native attempt is not replayed.

On exact `ad24b647`, Windows reached its first real approval card after startup
trust, with both initial and approval process/environment proofs passing. Its
helper then refused the full cwd disclosure because Python displayed native
backslashes while Kimi displayed forward slashes. No approval answer was sent.
The helper now requires the complete case-preserving resolved path in its
POSIX display form as a whole `cwd:` line. Unix display bytes stay unchanged.
A wrong directory, a sibling that merely shares the expected path prefix, a
trailing child or a different drive-letter case still refuses. This changes
only fixture display verification; native process/input guards and all seven
semantic method bodies remain intact.
