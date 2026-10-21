# Mobile session project grouping

Android offers **Move to project…** for a current parent session when both the
relay and its selected computer advertise `project_assignment`, `project_assign`
and `session_project_assignment_v1`. This changes the same logical session
membership used by the desktop's Move to project action. It does not change the
working folder, create a folder/worktree, launch a session or restart a provider.
Live logical projects from that exact swarm are eligible without a folder. The
launch catalog's `accessible` flag is not an authorization rule for grouping.
Archived conversations and child-agent views do not own this action.
The first Windows local-gateway scope does not advertise either operation or
this feature. Its existing operation allowlist refuses both before native work,
after the durable duplicate-receipt lookup. This change does not extend that
Windows scope.

The picker loads fresh bounded pages from the selected computer. One page has at
most 100 projects; the complete set has at most 5000. Names remain literal text.
Every page must retain the same swarm, current membership, membership token and
catalog epoch; a mismatch discards the set. A loaded set can authorize an explicit
Move for at most 60 seconds and only while the selected target, navigation,
connection generation, route and native computer identity remain unchanged.
The native computer rechecks the epoch and membership under its catalog lock.
Reading choices does not initialize/synchronize the swarm or contact a provider.

## Additive API v1 operations

Both operations are session-scoped. The ordinary envelope carries its exact
`computer_id`, `session`, `request_id`, operation and payload. A read payload has:

- `request_id`: canonical nonzero UUID matching the envelope;
- `expected_run_id`, `expected_conversation_id`: exact stored parent identity;
- `expected_machine_id`: the selected computer's canonical native UUID;
- optional `cursor`: the opaque next page from this same read generation.

`project_assign` adds exactly `swarm_id`, `membership_token`, `catalog_epoch` and
`project_id`, and accepts no cursor. Proof tokens are lowercase SHA-256 strings.
No directory, full organization, archive, child agent or launch fields are
accepted. The fixed native commands are `hgs swarm session-project --json` and
`hgs swarm assign-session --json`; the exact session name travels in JSON stdin,
never shell or provider arguments. Capability probing requires both commands and
`Mobile project assignment ABI: session-project-v1`. Older peers remain disabled.

The native read returns schema 1, the exact request/name/run/conversation/native
machine, swarm, current project, membership token, catalog epoch, project page and
next cursor. Native assignments return that bound intent and `assigned`, `failed`
or `uncertain`. A confirmed result includes the chosen current project. A new
membership read cannot settle an uncertain mutation.

## Persistence, recovery and upgrade

The phone saves one immutable action UUID, target, selected project, native
identity, catalog proofs and private route fingerprint before its only POST.
A lost reply remains uncertain. **Check original request** uses GET for that
UUID through the unchanged original route; it never repeats the POST. A changed
route or native computer refuses recovery. Drafts, answers and read positions
remain separate from project metadata. Marking an unknown action reviewed is an
explicit acknowledgement, not proof of delivery or an undo.

Native assignment reuses `swarm/catalog.lock` and the existing bounded private
`launch-assignments.json` write-ahead receipt store. New entries have kind
`session-project-v1`; UUID collisions with launch assignments or different
payloads refuse. A receipt is written before the sole selected `Key::Session`
register is persisted. An interrupted intent remains uncertain and is never
applied again. A durable definite refusal also stays bound to its UUID. Duplicate
requests return their original outcome before checking a later run/membership,
while still checking the persisted native computer. Other catalog registers,
folders, defaults and local peer/binding metadata are preserved. The common
10,000-receipt bound refuses new admission rather than evicting a mutation.

Relay SQLite/PostgreSQL and both connector journals add a separate partial index
for the `project_assignment` read class. Delivered read results expire after 120
seconds. Existing persisted legacy `READ_SQL` index predicates, retention budgets
and indefinite mutation UUID tombstones remain unchanged. Opening an existing
journal/catalog does not import, replace or reset it. `project_assign` remains a
mutation for restart recovery, receipt hashing, route identity and tombstones.

The stored-binding read is bounded to 1 MiB and the existing catalog/receipt
files to 128 MiB. Missing, malformed, oversized or nonregular evidence fails
closed. The operation reads binding metadata without reconciling provider
histories, discovering processes or starting native workers.

## Isolated validation

The controller/protocol unit tests cover changing navigation, workspace and
route identities, mixed catalog epochs, expired choices, durable intent before
the single POST, duplicate taps, accepted-POST polling failures and exact-UUID
receipt recovery. The portable Windows policy fixture additionally rejects both
operations and this feature while preserving duplicate lookup before dispatch.
The native metadata tests check that other catalog registers, directories,
bindings and permanent receipts remain unchanged or are refused as appropriate.

`SessionProjectSmokeInstrumentation` renders the real project dialog and runs the
real controller with a memory-only network and persistence fixture. It checks
literal project labels and current membership; selection and Close without a
write; a held durable save with Move disabled and no POST; a single uncertain
POST followed only by GET of its original UUID; changed-route refusal; and a
late read after navigation. It starts no server, connector, native agent or
namespace helper. It is UI/controller coverage, not an end-to-end relay or real
provider qualification.

For a reviewed exact source, the ordinary Android checks are run from
`mobile/android` with a pinned JDK 17/SDK 36 and offline Gradle dependencies:

```sh
./gradlew --offline --no-daemon --max-workers=2 \
  -PzerusSigningProperties=/private/fixture/absent-signing.properties \
  testDebugUnitTest lintDebug assembleDebug
```

The path above must be an absent file in the allocated private fixture; never
use the owner's signing configuration. A separately approved test-APK build uses
that same source, private `ANDROID_USER_HOME`, explicitly allocated debug signer
and `-PzerusInstrumentationRunner=app.zerus.mobile.SessionProjectSmokeInstrumentation`
with `assembleDebug assembleDebugAndroidTest`. Check the resulting application
and test APK certificate equality and exact instrumentation manifest, rather
than inferring them from a keystore pathname. No product code or old external
producer approval is replaced by this fixture.

An actual UI run requires a new disposable emulator profile, a reviewed image
and executable set, an owned ADB server and an exact serial. Install only into
that new profile after verifying that it has no existing app/account data. Run
the exact custom runner once using `am instrument -w -r`; preserve the raw
successful stream and exit, and require the runner's result code. Keep native
session and owner-phone operations out of this gate. Resource allocation,
signing, installation and execution are separate from source and unit-test
acceptance; this document grants none of them.

## Disposable Android UI qualification

`.github/workflows/android-project-ui.yml` runs on relevant pull requests and
offers an explicit exact-commit manual run. It has read-only repository access,
no secrets, publication environment or caches, and uses SHA-pinned actions on a
standard disposable GitHub-hosted Ubuntu VM. The finite reviewed image list is
`20261012.334.1` and `20261018.339.1`, which GitHub currently assigns during its
rolling update. Both official image releases list the same required Java and
SDK tool versions; all individual guards still apply. Other images or JDKs are
refused pending review. Manual package inputs must equal the checked-in pins;
there is no latest-version fallback.

Admission retains the bounded actual JDK `release` file and names the failing
guard before checkout. The exact Temurin Java 17.0.20.1 build 1 must be proved by the
version fields; only the explicit `-LTS` spelling is normalized. Missing build
proof, conflicting fields and unknown suffixes remain refusals. The helper uses
the same checked validation as the pre-checkout admission step.
The runner summary table abbreviates this as 17.0.20+1; the retained actual
release and official Adoptium Linux build metadata both include patch component
1. That component is required, not discarded during normalization.

The stable package selection in `scripts/ci/android-project-ui-pins.json` is
Emulator 37.2.12 and API 35 Google APIs x86_64 revision 9. Google repository XML
URLs, SHA-256 identities and selected archive sizes/official SHA-1 checksums are
recorded there. Source preparation read only those two XML documents. The job
uses the official stable-channel `sdkmanager`, refuses different installed
revisions, and records the actual installed tool/image hashes. Archive bytes are
not independently re-hashed by this helper; the metadata receipt and installed
hashes are different evidence. This selection does not map a development host's
older platform-tools binary to an upstream source revision.

The helper creates private HOME/XDG/Gradle/Android/AVD state, uses an absent
signing-properties path and a new synthetic debug signer, and checks the actual
app/test certificate equality, manifest runner/target and defined DEX classes.
An explicit foreground ADB server and one fresh KVM emulator use empty fixed
loopback ports. Its port-only `-L tcp:5037` listener retains the default localhost
binding without `-a`; clients explicitly use 127.0.0.1. Other attached devices
cause refusal. App networking is disabled
before installing the fixture. No owner device, existing profile, retained APK,
keystore, native agent, relay or gateway is involved.
Before installation, filtered package listings for the exact app/test IDs must
succeed with empty stdout and stderr on the fresh user 0 profile. A package
presence, command failure or diagnostic output is a refusal.

Exactly one custom-runner invocation must report all six ordered case IDs:
`current_selection`, `close_no_write`, `durable_single_post`, `route_refusal`,
`original_receipt`, and `stale_navigation`. Failed, incomplete, duplicate or
unknown result fields fail qualification. Cleanup addresses only the unreaped
foreground leaders through their pidfds, and requires their exit. This is a
whole disposable VM boundary, not proof of all descendant ownership or a
general installed SDK/kernel isolation contract. The VM is retired after the
job; failed or uncertain steps retain raw evidence and never authorize replay.

Offline helper controls run with `python3 -I -B tests/test_android_project_ui.py`.
Passing those controls or compiling the APK does not establish an actual UI
pass: the raw instrumentation result and cleanup outcome from this hosted job
are required separately.
