# Mobile session project grouping

Android offers **Move to project…** for a current parent session when both the
relay and its selected computer advertise `project_assignment`, `project_assign`
and `session_project_assignment_v1`. This changes the same logical session
membership used by the desktop's Move to project action. It does not change the
working folder, create a folder/worktree, launch a session or restart a provider.
Live logical projects from that exact swarm are eligible without a folder. The
launch catalog's `accessible` flag is not an authorization rule for grouping.
Archived conversations and child-agent views do not own this action.

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
