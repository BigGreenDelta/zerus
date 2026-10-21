# Compatible shared-read hot path

Task: `zerus-rk5b.4.1.1`, under the still-open bounded-storage task
`zerus-rk5b.4.1`. This repair retains the existing version-1 file and writer
lock. It does not migrate a host, change API v1, acknowledge a new read edge,
or establish installed performance.

The retained synthetic Release baseline measured one successful read write at
100, 1,000 and 10,000 saved owners, with one and eight conversations per owner.
It opened saved binding files `4N+2` times inside the write. At 10,000 owners the
median was 366.179 ms / 546.063 ms; the eight-conversation JSON was 12.66 MB.
Those observations belong to the exact historical baseline source, not to this
candidate. The comparison gate must retain fresh raw timings and syscall counts.

## Default behavior

Admission still takes `read-marks.lock` before capturing native state. A reader
waiting behind another writer therefore captures after that writer, rather than
applying an earlier snapshot. Exact run/conversation/current-reply/signature
checks, unknown-question refusals and writer order remain unchanged.

For a tracked terminal session, archive reconciliation opens the selected
binding only. It uses the existing archive metadata index to reject an exact
archived identity, including a crash-left binding whose pane is still present.
The existing clean-exit, paused, pending-resume, live-pane and process guards
still decide whether that selected binding is archived. Ordinary listings keep
their global lifecycle reconciliation. Reading A no longer archives unrelated B
as a side effect. A cold or changed archive directory still rebuilds the existing
archive index; that is explicitly not a constant-time path.

DeepSeek uses the same listing projection for one selected binding, retaining
the host's existing list RPC, native identities and generation checks. It no
longer opens every saved DeepSeek binding just to select one. This does not
change or qualify the native host's own list cost. OpenCode keeps its current
inspection and read-mark-before-binding-lock rename order.

Pruning the global owner map occurs only when an absent owner is admitted.
That path still obtains the authoritative tracked journal owners and retains
DeepSeek/OpenCode owners before inserting the new one. Existing-owner writes
cannot increase the owner count and still bound that owner's conversations to
eight. Abandoned owner entries can survive until the next new-owner admission;
they are not deleted from a stale snapshot or an unverified name-to-owner guess.
Global owner pruning is therefore amortized over admission, not eliminated.

An acknowledgement whose accepted fields already equal the stored fields still
reports `applied: true`, with the unchanged mark and original `at`. It neither
rewrites `read-marks.json` nor refreshes an acknowledgement timestamp without a
new read edge. A changed accepted field retains the other field and updates
`at`, as before. Changed writes still atomically replace and sync the v1 file.

Writers now refuse unreadable, malformed, unsupported-version or invalid-root
storage instead of treating it as empty and overwriting other owners. Missing
storage remains a first write. Open is nonblocking and does not follow a final
link; the open handle must be private, regular and single-link before reading,
so a FIFO is refused without waiting and a dangling link is not treated as a
new store. OpenCode's already-strict rename preflight uses
the same decoder, and DeepSeek's transfer reuses that guard without changing
its binding/read-writer order. Listing's existing unavailable-file behavior is
unchanged; no legacy-format migration or repair is inferred from a read.

## Compatibility and remaining costs

Old and current writers still share the same lock, parse the same v1 map and
replace the same file. There is no sidecar authority, timestamp-based merge,
directory tombstone or hidden cutover. Exact legacy/current writer and rename
controls must retain fields written by the preceding writer.

This is a compatible hot-path improvement, not fully bounded storage. Global
JSON decoding and changed-write serialization remain O(number of saved marks).
New-owner admission still scans tracked bindings. Cold archive-index refresh,
legacy supervisor name-route synchronization, terminal pane discovery and
provider-specific native I/O retain their existing costs. Acknowledging many
owners does not make those costs free; report them separately.

Fully indexed writes require a separately selected, versioned storage protocol.
The old reader maps unreadable/non-v1 files to an empty map, so a marker alone
cannot make it refuse safely. Keeping a frozen v1 mirror makes old readers
silently stale. Neither is implemented here. The parent task stays open until
that compatibility boundary has a reviewed solution.

## Finite comparison gate

No owner state, provider, native service, installed CLI or live gateway belongs
to the gate. Use the retained synthetic paused bindings at 100/1,000/10,000
owners and one/eight conversations. Keep the 50,000 population data-only.

First run Rust read-mark/OpenCode rename controls and the private CLI read,
archive and DeepSeek suites, including a synthetic legacy writer holding the
real v1 lock. Run full Rust tests and strict all-target Clippy in the allocated
isolated build. Do not replace errors, ignores or unknown cleanup with a pass.

After sealing the exact source/tool/test-binary inputs, measure five raw changed
writes and one separately traced changed write for each population/conversation
pair. Every changed arm starts with `reply_id: 0:90.0` and must actually advance
to `1:100.5`, never short-circuit as a duplicate. Measure one separately traced
identical-repeat arm per pair, starting with the current reply already stored;
it must retain the exact file and show zero mark replacements/fsyncs. Retain
end-to-end in-function duration, separate wall duration, capture/parse/apply/
prune/serialization/fsync components, successful and failed syscall counts,
and the selected-versus-total binding opens.

The comparison uses the same immutable binding/request/mark fixture bytes as
the historical baseline. New-owner admission, changed archive catalogs and
legacy-name routes are functional guard cases, not hidden in a claim that all
paths are constant-time. The independently reviewed private runner supplies
finite deadlines, output bounds and owned-child cleanup; its grant is separate
from this source document. Stop at the first unexpected result without replay.
