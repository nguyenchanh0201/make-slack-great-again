# DM unread zero-count reconciliation

Status: approved by the user on 2026-11-17; implementation plan requested.

## Intent and scope

Make already-read DMs lose their unread badge and stay read when MSGA receives
server counts or read events. Preserve genuinely newer incoming messages,
intentional Mark unread, and independent thread reads. The user selected the
targeted fix after comparing three approaches and requested the Superpowers flow.

This is a follow-up to commit `8a5c2ef`, not a rerun of the earlier
`2026-11-17-dm-read-state-design.md`: that document describes several problems
already addressed by that commit. The new evidence is in
`docs/investigations/2026-11-17-dm-unread.md`, with two failing reproductions in
the adjacent patch. The user's exact live incident has not been captured.

## Approaches

1. **Parser fix alone.** Preserve explicit zero instead of guessing one unread
   message. Small, but does not repair zero-count read events retaining badges.
2. **Parser plus targeted reconciliation (selected).** Preserve count presence,
   reconcile current observations with newer eligible incoming-message evidence,
   and reuse the existing request lanes and local Read/Mark unread generations.
   More tests and private bookkeeping, without replacing the Slack backend.
3. **Central read-state subsystem.** Unify the entire backend around a new state
   machine. Larger migration and regression risk than these defects justify.

## Evidence and required outcomes

The current mapper treats both a missing `unread_count` and an explicit zero as
permission to infer unread from `latest > last_read`. The current authoritative
merge preserves the previous count whenever `latest > lastRead` and the observed
count is zero. A newer own message therefore keeps a stale incoming badge alive.

Required outcomes:

- An explicit zero stays zero through parsing and every applicable merge.
- An omitted count remains unknown; it must not clear a known backlog or become
  a fabricated exact count from timestamp differences.
- A current remote read with zero counts clears the reproduced stale DM badge
  even when `latest` belongs to the user's own reply.
- A delayed snapshot cannot hide new eligible incoming messages received after
  that request left the queue, or override a later Read/Mark unread action.
- An own send, duplicate delivery, edit, historical backfill, or an independent
  thread-only event must not be mistaken for a new top-level unread arrival.
- Existing intentional local and remote Mark unread behavior remains available.
- A read-state update alone does not replay message notifications.

## Design

### 1. Carry count presence with the observation

Add a small Slack-private read-observation representation (or equivalent private
fields) for the cursor, latest timestamp, optional unread/mention counts, source,
and locally captured request context. Do not reinterpret zero as missing.
Counts and their cursor belong to the same observation.

Map fields according to the endpoint's existing contract, distinguishing raw and
display counts explicitly rather than assuming they are interchangeable. Cover
explicit zero, positive, and missing counts in fixtures. Validate numeric input
before converting to unsigned values. If an endpoint omits count information,
merge its metadata/cursor without inventing a count; obtain a count-bearing
observation when needed. Do not add a public Backend API or cache-format change
just to carry transient Slack response metadata.

### 2. Separate newer unread evidence from `Conversation::latest`

Keep `latest` for its existing conversation-activity purposes. It does not prove
unread incoming activity. Add narrowly scoped, per-conversation bookkeeping of
newly delivered eligible incoming messages, with local arrival sequence and
message timestamp, using the shared delivery path and its existing duplicate,
authorship, mute, membership, and conversation/thread counting rules.

Record accepted message evidence before notifying observers so a synchronous
local read cannot leave an arrival incorrectly counted afterward. A read action
retires evidence at/below its accepted cursor. Repeated delivery does not add a
second entry. Mark unread retains the current intentional history-recount path;
it is not reconstructed solely from the new-arrival bookkeeping.

Retain only evidence needed by active observations and current unacknowledged
arrivals. Bound storage per conversation and clean it up with the backend. An
overflow invalidates affected observation contexts and requests reconciliation;
silently discarding evidence must never permit an unsafe clear. The builder
chooses a modest limit and tests the overflow behavior.

### 3. Reconcile snapshots using dispatch-time context

At actual API dispatch, capture the existing local Read/Mark unread generation,
the incoming-arrival sequence, and a per-conversation accepted-observation
revision. Capture again on a retry that sends a fresh request. A queued request
or response callback is too early or too late for this context.

Reject observations superseded by newer local intent or an accepted remote
state observation. Route roster merges, count polls, DM enrichment, reconnect
patches, and explicit reconciliation through the same rule; batched results
must carry their own captured context rather than consult mutable global
dispatch state later.

Supersession must converge: also assign local request-dispatch identifiers. If
accepting one observation invalidates another response which could contain
newer state (including a later-dispatched overlapping request), coalesce one
fresh reconciliation under the current versions. If the rejected result is
already superseded by an accepted later-dispatched request or by a currently
queued/current-context reconciliation, do not add another request. These local
identifiers manage outstanding work; they do not establish Slack event order.
Follow-up work uses normal pacing and remains queued through errors for a later
retry, rather than leaving stale counts indefinitely or spinning on conflicts.

For an eligible zero-count snapshot, remove pre-existing stale counts. Preserve
only eligible incoming evidence not safely covered by that observation. In
particular, post-dispatch arrivals remain protected when server ordering is
unknown, even if `latest` happens to include their timestamps. Reconcile again
to settle that ambiguity; local receipt order is not proof of server snapshot
order. A newer own message contributes no such evidence.

For positive counts, preserve the authoritative backlog and newer evidence
without double-counting overlap. If coverage is ambiguous, keep a conservative
bound and requery instead of claiming an exact total from a partial page.
Missing-count observations preserve counts unless cursor/message evidence
independently establishes that they are read. Never recount the whole backlog
from a partially loaded history page.

Use the existing request pacing and in-flight/queued reconciliation coalescing.
Do not create a request per message or an immediate retry loop. On errors keep
the current safe state and allow paced retry; do not erase pending intent or
unread evidence merely because a request failed.

### 4. Handle WebSocket events without inventing ordering guarantees

Keep existing marked-event and intentional Mark unread guards. A message cursor
is not an event sequence number. Do not assume a local arrival sequence or a
timeout establishes Slack's event ordering.

When any marked event conflicts with accepted state/newer activity and its
coverage is ambiguous, use it to trigger a fresh count-bearing reconciliation
under the dispatch-time rules above. A contradictory backward cursor continues
through the existing remote-Mark-unread reconciliation path. This permits a
short reconciliation delay instead of permanently retaining a false badge or
immediately erasing a real new message.

This applies to positive counts too: a delayed positive echo at the same cursor
must not restore a badge after an accepted zero/read observation merely because
`latest` is a newer own message. A conflicting positive event without new
eligible incoming evidence triggers reconciliation instead of being immediately
treated as a new unread backlog. A legitimate remote Mark unread still works
through the fresh-response path, including when history is not loaded.

The realtime regression must exercise this full event-to-reconciliation path
with a matching fake-server read-state response. The original reproduction
remains evidence of the defect, but its instantaneous-clear expectation may
need to become eventual-clear under the selected safe reconciliation policy.

### 5. Notification and UI integration

Keep the existing initial/deferred notification eligibility checks, top-level
read cursor, separate thread read floor, and workspace-lifecycle safety. Update
the Store atomically for accepted cursor/count changes, so the sidebar and
workspace badge see the same state. Do not implement a visual-only badge filter.

Add opt-in diagnostic read-state logging using the existing logging facilities:
source, local conversation reference, cursor/count before and after, captured
versions, and accept/defer/reject reason. Exclude message bodies, names,
credentials, and full API responses. This is to identify which update causes
the user's live occurrence if it survives the deterministic fixes.

## Validation

Restore the two saved reproductions as permanent regression tests before changing
production code, updating the realtime fixture to provide the fresh zero-count
response required by this design. Add tests for:

- Explicit zero versus omitted counts, including mention-count presence.
- Remote read with a newer own reply; equivalent count-poll and reconnect paths.
- A genuinely new incoming message during a delayed zero response; an own send
  during that same delay; duplicate delivery.
- Competing snapshots completing in reverse order and a remote update while a
  request waits in the queue; retries capturing fresh context.
- Rejected overlapping observations causing a single paced fresh reconciliation,
  with eventual convergence; accepted zero/read followed by a delayed positive
  marked event at the same cursor.
- Read then Mark unread, Mark unread then Read, and legitimate remote Mark unread.
- Missing counts with an unloaded backlog; independent thread reads; bookkeeping
  overflow and backend teardown.
- No banner replay from reconciliation, and preserved delayed-notification
  suppression for an already-read top-level message.

Run affected read/realtime/write/shell suites and the available full CTest suite.
The investigation baseline passed read/realtime/shell but failed the existing
Enterprise Grid token-routing write test at `test_slack_write.cpp:1064–1078`.
Report that separately if still present; it is not part of this fix.

## Constraints and builder choices

C++20 and existing test/fake-server infrastructure; no new dependency, broad
read-state rewrite, OS notification rewrite, release, push, or deployment.
No change to the 30-day notification policy. No already-posted OS banner removal.

The builder chooses private type layout, evidence storage limit, and exact
logging switch. Endpoint payload presence and coverage are implementation
characterization work: test against existing fixtures, and explicitly report
any guarantee unavailable without a live sanitized capture. Do not invent
protocol fields. Passing fake-server tests will not by itself prove the user's
exact live incident is resolved.

## Builder review

A fresh builder review identified two missing rules: convergence after rejecting
overlapping snapshots, and stale positive marked events after an accepted zero.
Both are resolved above and included in validation. These are proposed technical
choices by the author, not additional user requirements.
