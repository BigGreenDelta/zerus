# Guarded Worktree Cleanup Design

Tracking issue: `zerus-2gk9`.

## Goal

Let people remove Git worktrees they no longer need from Zerus, mainly from
**Projects**, without ever losing work silently. Zerus already lists and creates
worktrees; it has never removed one. This design adds:

1. An explicit **Remove worktree…** action on a worktree row.
2. A **Clean up…** review of all linked worktrees of a repository, with batch
   removal of the ones that are safe.
3. **Forget missing worktree…** for checkouts whose folder no longer exists
   (Git reports them as `prunable`).
4. A cleanup offer after the last session in a linked worktree is archived.
5. A non-destructive reminder: the Clean up button shows how many worktrees are
   ready for removal.

Non-goals: automatic or scheduled deletion, `--force`, fetching, deleting
unmerged branches, mobile clients (the `worktrees-v1` ABI stays unchanged), and
cleanup of ordinary non-Git folders.

## Safety model

Removal always goes through the `hgs` process on the machine that owns the
checkout. That process repeats every check under the repository lock that
worktree creation already uses, compares a fingerprint taken by the review the
person saw, and runs only `git worktree remove` without `--force`. Branch
deletion is separate, optional and uses `git branch -d`.

Verified Git 2.56 behavior that shapes the checks:

- `git worktree remove` refuses locked, main, modified and untracked checkouts.
- It **silently deletes ignored files** (`target/`, `.env` if ignored).
- It **silently deletes a nested worktree inside an ignored directory**,
  including that worktree's uncommitted changes, and leaves its metadata behind.
- For a checkout whose folder is gone it removes only the metadata.
- It refuses checkouts with initialized submodules unless forced.

## CLI

### Review

```
hgs [@host] worktrees review --path PATH [--worktree WORKTREE] [--json]
```

`PATH` is any folder inside the repository. `--worktree` limits the result to
one checkout. Output:

```json
{"state":"ok","machine":"arch","common_dir":"/repo/.git","base":["main","origin/main"],
 "sampled_at":1760000000.0,"partial":false,
 "worktrees":[{"path":"/repo-task","kind":"linked","branch":"task","head":"<sha>",
   "available":true,"locked":false,"prunable":false,"detached":false,
   "verdict":"ready","reasons":[],"notes":[],
   "changes":0,"untracked":0,"merged":true,"upstream":"origin/task","pushed":true,
   "ahead":0,"last_activity":1759900000.0,
   "ignored":[{"path":"target","bytes":123456,"complete":true}],"ignored_bytes":123456,
   "sessions":[],"archived_sessions":0,"processes":[],
   "fingerprint":"<sha256>"}]}
```

Catalog errors (`not_repo`, `timeout`, …) use the same `state` values as
`hgs worktrees`. The main checkout and bare entries appear with verdict
`blocked` so the dialog can explain why they are not offered.

Verdicts:

| Verdict | Meaning | Offered |
|---|---|---|
| `ready` | No blocker, no attention note | Pre-selected in Clean up |
| `review` | Removable, but has attention notes | Selectable, never pre-selected |
| `missing` | Folder is gone; only Git metadata remains | Forget, pre-selected |
| `blocked` | At least one reason below | Not selectable |

Blocking `reasons` (code and plain-English text):

- `main_checkout`, `bare`, `locked` (with Git's reason)
- `processes`: a process of this user has its working directory inside
  (Linux `/proc/*/cwd`, macOS `PROC_PIDVNODEPATHINFO`; up to five
  `{pid, command}` samples). This covers agents, shells in tmux panes, editors
  and file managers. The reviewing `hgs` process itself is excluded.
- `sessions`: a Zerus binding (running, paused or saved; Claude, Codex, Kimi or
  DeepSeek) has its folder inside. Archive them first.
- `changes`, `untracked`: Git status is not clean.
- `nested`: another worktree lives inside this folder.
- `submodules`: initialized submodules (Git would require force).
- `detached_unreachable`: a detached HEAD whose commit is on no branch, tag or
  remote branch.
- `status_unavailable`: status or metadata could not be read in time.
- `processes_unavailable`: this computer cannot read other processes' working
  directories (Unix systems other than Linux and macOS, and Windows for now), so
  a program may still use the folder.

Attention `notes`:

- `not_merged`: the branch has commits not in the base. Removing the worktree
  keeps the branch; branch deletion is not offered.
- `archived_sessions`: archived sessions still refer to this folder; restoring
  them there would no longer work.
- `recent`: activity in the last 24 hours (a freshly created worktree looks
  "merged" because it has no commits yet).
- `no_base`: no base branch could be determined, so `merged` is unknown.

The base is the main checkout's branch plus `origin/HEAD` when present. A branch
is `merged` when its HEAD is an ancestor of any base ref. `ignored` lists
top-level ignored entries reported by `git status --ignored`; sizes are measured
with a bounded walk (`complete:false` when cut short). Activity is the newest
modification time of the checkout's `HEAD`, `index` and `logs/HEAD` metadata.

The fingerprint is a SHA-256 over path, head, branch, lock/prunable state,
status entries, nested paths, bound session names, and ignored entry names.
Sizes, activity and process samples are not part of it.

Budgets: three seconds for the catalog, five seconds per Git call, twenty
seconds for the whole review, at most 64 linked worktrees evaluated (the rest are
`blocked` with `status_unavailable` and `partial: true`), at most 200 000
entries or two seconds per ignored size walk.

### Remove

```
hgs [@host] worktrees remove --path WORKTREE --common-dir DIR --fingerprint HASH
    [--delete-branch] [--request-id ID] [--json]
```

1. Canonicalize; refuse relative paths and `--dry-run` (as `create` does).
2. Take the repository lock shared with `worktrees create`.
3. Re-run the review for exactly this worktree. Refuse when the repository
   differs from `--common-dir`, the worktree is not a linked entry of it, the
   verdict is `blocked`, or the fingerprint changed (`Worktree changed since the
   review. Review it again.`).
4. Run `git worktree remove -- WORKTREE` from the main checkout (or the common
   directory). Never `--force`.
5. Confirm that Git no longer lists the worktree and that the folder is gone.
6. With `--delete-branch`, only when the review said `merged`, run
   `git branch -d -- BRANCH`. A branch failure is reported in `branch_error`;
   the worktree removal still counts as done.
7. Drop the cached catalog for the repository.

Result: `{"status":"removed"|"forgotten","path","branch","branch_deleted",
"branch_error","common_dir","request_id","machine"}`. Exit status is non-zero
with a readable message when nothing was removed. There is no automatic retry;
an uncertain result is resolved by reviewing again.

Removing a large checkout can take time: the Git call may run for up to five
minutes. Remote calls use the existing `@host` SSH routing; a machine running an
older `hgs` answers with a usage error, which the GUI explains as "Update Zerus
on that computer".

## Desktop

### Client

`HgsClient` gains:

- `reviewWorktrees(host, path, worktree = {})` → `worktreeReviewReady(request,
  host, path, data)` / `worktreeReviewFailed(...)`. Results are cached for five
  minutes per machine and repository for the reminder count.
- `removeWorktree(host, path, commonDir, fingerprint, deleteBranch)` →
  `worktreeRemoved(request, ok, result, error)`. Success marks catalog snapshots
  and the review cache for that repository stale, as creation does.

### Worktree panel (Projects and the session inspector)

- A **Clean up…** button joins the panel's action row (the compact Projects
  heading row included). It shows `Clean up (N)` when a cached review found `N`
  ready or missing worktrees, and is hidden in the folder picker.
- The review for the reminder runs lazily: only while the panel is visible, the
  catalog lists at least one linked worktree, and no fresh cached review exists.
- A context menu on worktree rows offers **Remove worktree…** (linked rows),
  **Forget missing worktree…** (prunable or unavailable linked rows) and
  **Copy path**.

### Clean up dialog

`WorktreeCleanupDialog` shows one row per worktree: a checkbox, folder and
branch, verdict, reasons and notes, merged state, ignored data size and last
activity. Ready and missing rows start checked; review rows are checkable;
blocked rows are disabled and explain why.

**Also delete merged branches** applies only to merged rows. The choice is
remembered in `workspace/worktreeCleanupDeleteBranches` (default off).

**Remove selected…** opens a confirmation listing the number of worktrees, the
ignored data that will be deleted with them, and the branches that will be
deleted. Removals then run one at a time, each with its own request ID and
fingerprint, updating that row with the result. A refusal does not stop the
remaining rows. Afterwards the dialog reviews again.

Opened for a single worktree (from the row menu or the archive offer), the same
dialog shows only that row.

### After archiving

When **Move to archive** succeeds for a session whose folder is a linked
worktree, and no other non-archived session on that machine uses that worktree,
the status bar shows a **Clean up worktree…** button for one minute. It opens the
dialog for that single worktree. Nothing is removed without that dialog.

## Testing

- `tests/test_worktrees.py` with real isolated repositories: each verdict and
  reason; processes inside; bindings and archives in a private `HGS_STATE_DIR`;
  nested worktree in an ignored folder; fingerprint mismatch; branch deletion of
  merged and unmerged branches; forgetting a missing checkout; locked and main
  refusals; `--dry-run` refusal.
- `tray/tests/test_worktreepanel.cpp` with the fake `hgs`: reminder count,
  dialog pre-selection, blocked rows, confirmation, sequential removal, failure
  of one row, single-worktree mode and branch preference.
- Archive offer: `test_sessionswindow` covers appearance only for the last
  session in a linked worktree.

## Documentation

`docs/reference.md` (Worktrees and ordinary folders, CLI section) and the `hgs`
usage text.
