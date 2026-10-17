# Retired session Graph experiment

The desktop Graph experiment was removed at the owner's request on 2026-10-16,
after testing Dev793. Its presentation did not provide a useful enough overview;
future work should start as a separate design iteration rather than restore the
current screen by default.

The annotated Git tag
[`archive/session-graph-2026-10-16`](https://github.com/ufna/zerus/tree/archive/session-graph-2026-10-16)
preserves the complete implementation immediately before removal, at
`daa06299ca5ddd72e7c48f04e351de822ffdc533`. The retirement is tracked as
`zerus-ybej`; the commit adding this note is the removal point in main history.

To inspect or continue the experiment in an isolated checkout:

```sh
git fetch origin tag archive/session-graph-2026-10-16
git worktree add -b experiment/session-graph ../zerus-session-graph archive/session-graph-2026-10-16
```

That checkout also contains other code from its historical main revision. Bring
back selected work through review; do not revert the whole retirement commit or
replace a current installation with the old checkout as an automatic recovery.

The removal deletes the desktop destination, canvas, layout/cache collectors,
inspection transport and Graph menu entries. Existing local layout/cache files
are left untouched. Session messaging, explicit independent-agent waits and the
bounded native metadata commands remain compatible with previously released
clients. No new desktop Graph polling runs.
