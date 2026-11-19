# Implementation agent prompt

Implement the targeted DM read-state and notification fix in this repository.

Read these files first:
- `docs/superpowers/specs/2026-11-17-dm-read-state-design.md`
- `docs/superpowers/plans/2026-11-17-dm-read-state.md`
- Applicable `AGENTS.md` instructions and the current git status.

The user approved the targeted approach and requested this implementation handoff. Follow the plan task by task using `superpowers:executing-plans`. You are not alone in the codebase: preserve others' changes and adapt to the current checkout. Do not delegate unless the executing session authorizes it.

The symptoms are already-read DMs producing banners and returning as unread. Investigation found a missing notification read check, direct marked-event cursor rollback, and independent maxima merging stale unread counts with newer read cursors. These are source findings plus isolated update-logic reproductions, not a captured live Slack incident.

Start by running the existing tests and characterizing marked-event ordering/correlation. Add failing regression tests before production fixes, using the existing fake Slack server and headless notification probes. Do not stop at adding a banner guard: implement the complete targeted synchronization fix.

Critical requirements:
- Recheck message eligibility after asynchronous name/avatar work, immediately before posting; suppress sound too when cancelled.
- Preserve Read and Mark unread intent across delayed writes, echoes, snapshots and retries.
- Do not globally maximize read timestamps: intentional backward changes must work locally and remotely.
- Message timestamp is not event order. Verify ordering fields rather than inventing them. Use local generations for request races and fresh reconciliation for ambiguous events; report any genuinely unresolved protocol limitation with evidence.
- Keep cursor/count observations coherent without losing newer unread messages or recounting incomplete history as the full total.
- Keep thread read floors independent of the main conversation and preserve reminders/huddles.
- Keep deferred callbacks safe across workspace switch, detach and removal; keep bookkeeping bounded.

Use codebase-memory MCP for code discovery and Context7 if consulting library/API documentation, per project instructions. Keep changes narrowly scoped; no new dependencies or broad backend refactor. Adjust private helper names to fit current code, but preserve the behavior and acceptance tests in the design.

Complete the focused and full available CTest checks, verify fake-server tests actually ran, and run `git diff --check`. Return a concise report with root causes addressed, changed files, regression red/green evidence, test results and any remaining validation limits. Leave the work local; do not push, open a PR, merge or deploy.
