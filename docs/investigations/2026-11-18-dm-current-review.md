# Current DM read-state implementation review

Reviewed `6ee4575..fa04f50`, covering the five implementation commits following
the approved plan. This review compares the new code with the captured live
Ha Vang evidence, not only the earlier synthetic zero-count reproductions.
Production and test implementation files were not modified in this review.

## Verdict

**Changes required.** Retain the new parsing, dispatch-context, serialized-write,
bounded-evidence and diagnostic work, but adjust observation scope and evidence
accounting before claiming the live incident fixed. The current code improves
the old permanent-badge behavior, yet can still restore the false badge.

## Findings

### 1. P1: confirmation still trusts the endpoint which contradicts the main DM

Confidence: 10/10 from captured live responses and the current merge-body replay.

`slack_backend.cpp:4305–4331` always reconciles through `conversations.info` and
applies its observation. `prepareReadObservation` at lines 4049–4050 accepts an
exact display count regardless of the activity scope that produced it:

```cpp
if (obs.display.unread.value.has_value() && obs.display.unread.exact) {
    exactUnread = *obs.display.unread.value;
}
```

The live target has info raw/display=1 with a thread reply as latest, but its
`client.counts` IM entry has `has_unreads=false`, and the thread's own read floor
covers all replies. Replaying the current prepare function gives:

```text
cleared live DM + actual info response -> unread=1
subsequent actual client.counts response -> unread=0
```

Unlike the old code, the new code can clear again, but it still lets the info
response revive the badge. Scheduling/recency guards do not resolve disagreement
between two different activity scopes, even when both use a field named display.

**Adjustment:** distinguish main-conversation observations from thread-inclusive
activity. Confirm the main DM with a comparable main-DM source; do not let an
info response override a main-DM read result solely because it was dispatched
later. Keep thread read floors and thread activity separate. Unknown-scope
fallbacks should request comparable evidence instead of inventing exact state.
Preserve genuine newer main-DM arrivals and legitimate remote Mark unread.

Also audit `Read::inject` at lines 3754–3771: normal DM thread replies still get
main-conversation deltas based on the main read cursor, and Arrival does not
retain their thread root/scope. `Read::threadRead` updates its thread maps but
does not retire such main-DM arrival evidence. The new arrival bookkeeping must
not classify an already-read thread reply as a new unread main-DM message.
Characterize genuine unread/broadcast/mention behavior rather than blanket
suppressing all thread notifications.

### 2. P1: missing-count observations can double-count an existing arrival

Confidence: 10/10 from the unchanged current prepare-function replay.

`slack_backend.cpp:4121` uses:

```cpp
newUnread = candidate.unread + disjointUnread;
```

The analogous mention fallback at line 4143 also adds the delta to current
counts. But an injected arrival is already reflected in `candidate.unread`.
With current unread/mentions=1, one recorded arrival at 400, and a roster
observation with latest=300 and no count fields, the function produces 2/2.

**Adjustment:** an unknown-count observation preserves current totals. Do not
add an arrival already included in that current total. Use protected deltas to
augment an independently supplied authoritative baseline only when they are
known not to overlap. Test missing unread and missing mention fields separately,
repeated metadata observations, and both sides of request dispatch.

### 3. P1: evidence is retired using merged local latest, not response coverage

Confidence: 10/10 from current prepare and commit function replay.

`slack_backend.cpp:4205–4206` retires an arrival when:

```cpp
return a.ts <= decision.lastRead ||
       (a.sequence <= context.arrivalEpoch && a.ts <= decision.latest);
```

`decision.latest` was computed with `max(candidate.latest, obs.latest)` at line
4045. It includes local messages which the server observation does not cover.
The replay starts with unread=1 and an arrival at 400, then applies a zero
snapshot covering only latest=300. Preparation correctly keeps one, but commit
deletes its evidence because merged latest is 400. The next identical zero
snapshot then clears the genuinely newer unread message.

**Adjustment:** carry explicit observed coverage/absorbed-evidence information
in the prepared decision. Retire only arrivals covered by an accepted read
cursor or actually absorbed into a known count-bearing baseline of the same
scope. Missing-count responses cannot claim to absorb evidence. Never use the
conversation's merged display/sorting latest as the coverage boundary. Verify
two consecutive old zero snapshots, not just the first response.

### 4. P2: current tests do not establish the live user-visible fix

Confidence: 9/10; independently reviewed by the testing specialist.

- `test_slack_realtime.cpp:1581` and `:1656` script info display=0. They omit the
  actual info=1 / main-counts=read / thread=read disagreement.
- `test_shell.cpp:4511` assigns counts to zero directly; `:4528` calls
  `setUnreadThreads(1)`. These verify rendering of manually assigned state,
  not backend reconciliation or independent thread read-floor eligibility.
- `test_slack_realtime.cpp:1747` checks eventual completion for 20 DMs without
  measuring maximum in-flight reconciliation or foreground progress.

**Adjustment:** add a fake-server-to-backend-to-Store regression with the saved
target payloads and timestamps, both response orders, reconnect/roster refresh,
and one genuinely new main-DM arrival. Verify sidebar and workspace/Dock
attention counts and no replayed banner. Add the missing-count and two-snapshot
evidence cases above. Strengthen the multi-DM test with delayed responses,
maximum in-flight <=1, foreground completion, and eventual fair progress.

## Verification performed

`cmake --build build --target msga_app_tests -j 6`: up to date, succeeded.

Fresh affected-suite run:

```text
core_slack_read      PASS
core_slack_realtime  PASS
shell_tests         PASS
core_slack_write    FAIL — existing Grid token-routing case, lines 1064–1078
```

The existing failure matches the earlier baseline and is not attributed to
these implementation commits. No production/live app changes were made.

`2026-11-18-dm-current-review-replay.cpp` contains the unchanged current prepare
and commit function bodies with a minimal host model. It is a deterministic
logic replay, not an end-to-end test or a runtime recording. Run:

```sh
clang++ -std=c++20 docs/investigations/2026-11-18-dm-current-review-replay.cpp -o /tmp/msga-current-review
/tmp/msga-current-review
```

Expected current output:

```text
current merge: cleared live DM + actual info response -> unread=1
current merge: subsequent actual client.counts response -> unread=0
current merge: one counted arrival + missing-count roster -> unread=2 mentions=2 (expected 1,1)
first zero response older than known arrival -> unread=1
commit accepted=1, retained arrival entries=0 (expected 1)
second identical old zero response -> unread=0 (expected 1)
```

Exit 1 is expected: the replay asserts the genuinely newer unread message must
survive. Both extracted function bodies were checked against the reviewed source.

## Recommended adjustment order

1. Add regressions for the captured cross-endpoint/thread-scope disagreement,
   missing counts, and repeated old snapshots; observe them fail.
2. Define main/thread observation coverage and precedence explicitly, then fix
   unknown-count accounting and evidence absorption using those boundaries.
3. Update ambiguity confirmation, DM thread-arrival classification/retirement,
   and reconnect/roster sources to use that shared policy.
4. Replace setter-only UI claims with the backend-driven regression, preserve
   all current race/Mark unread tests, then rerun the affected/full suites.
5. Update the original investigation's semantics table with the real captured
   endpoint discrepancy. It must not imply that every display field or a fresh
   info response necessarily describes the same main-DM badge metric.

No rewrite of the request lanes, renderer, native Dock integration, or the whole
read-state subsystem is needed to address these findings.


## Resolution (2026-11-18)

All four review findings were addressed in commits `e78bd07`..`7f6ed0d`:
1. Added explicit `slack::readstate::Scope` and `Coverage` (`UnknownActivity`, `MainConversation`, `Thread`), keeping `conversations.info`/roster/`im_marked` as `UnknownActivity` and routing `pumpReadReconcile` through `confirmMainReadState` (`client.counts` with bounded `confirmMainReadStateFromHistory` fallback).
2. Fixed unknown-count arithmetic so missing unread/mention counts preserve current totals without double-counting arrivals.
3. Replaced `decision.latest` evidence retirement with explicit `absorbedSequences` from same-scope complete observations or accepted read cursors, and prevented missing-count responses from clearing overflow invalidation.
4. Separated `Scope::Thread` arrivals from main-DM unread deltas in `Read::inject`, persisted `threadReadFloor` from `conversations.replies` and `subscriptions.thread.getView`, added `retireThreadReadEvidence`, added `/_ctl/request_stats` concurrency assertions, extended `MSGA_SLACK_READ_TRACE=1`, and added `slack_dm_read_tests`.
