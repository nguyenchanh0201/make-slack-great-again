# DM Read Scope and Arrival Accounting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the captured already-read DM badge returning, without double-counting arrivals or hiding genuinely newer unread messages.

**Architecture:** Extend the existing private reconciliation code with explicit observation scope and response coverage. Main-DM observations control the main badge; independent thread floors control thread replies; activity-only responses cannot overwrite either with incompatible counts. Preserve current dispatch/intent guards, request lanes, bounded journals and prepare/commit/Store publication ordering.

**Tech Stack:** C++20, existing CMake/Ninja and CTest infrastructure, Python fake Slack server, native/headless platform notification and badge probes.

**Spec:** Approved `docs/superpowers/specs/2026-11-17-dm-unread-zero-design.md`, refined by the accepted targeted approach and `docs/investigations/2026-11-18-dm-current-review.md`. Live evidence: `docs/investigations/2026-11-17-live-dm-badge-evidence.json` and its companion report. This follow-up starts at `fa04f50`; it does not rerun the five already completed zero-count tasks.

## Global Constraints

- C++20 and existing test/fake-server infrastructure; no new dependency, broad read-state rewrite, OS notification rewrite, release, push, or deployment.
- No change to the 30-day notification policy. No already-posted OS banner removal.
- Do not add a public Backend API or cache-format change just to carry transient Slack response metadata.
- A read-state update alone does not replay message notifications.
- Never recount the whole backlog from a partially loaded history page.
- Keep local and remote Read/Mark unread ordering, per-workspace isolation, independent thread reads, existing mute/focus rules and deferred-notification lifetime checks.
- Keep the current request pacing and one automatic reconciliation in flight per workspace. No HTTP scheduler rewrite or request per incoming message.
- Do not modify the installed app, live read markers, real messages, credentials or user caches during automated tests. No live credentials are needed.
- Preserve unrelated untracked documents and concurrent edits. Commit only explicitly owned task files.

## Review Focus

1. A later `conversations.info` count of one must not override a valid main-DM read result when its latest activity is a fully read thread (Tasks 2–4).
2. Omitted counts preserve existing totals without adding arrivals already included in those totals (Task 1).
3. Two consecutive old zero snapshots must both preserve a newer eligible arrival; merged local latest must not retire its evidence (Task 1).
4. Thread read floors must suppress covered delayed replies without suppressing genuinely new replies, broadcast messages or main-DM messages (Task 3).
5. Unavailable/omitting count endpoints, reconnects, reverse responses, failures and multi-DM bursts must converge safely without erasing unknown state or starving foreground work (Tasks 2, 4–5).

---

## Fixed evidence and acceptance criteria

Use anonymized test IDs `D1`, `UPEER`, `UME` with the **actual captured timestamps**:

```text
root       = 1794909869.919749    // main timeline's latest, 17:04:29 +07
mainRead   = 1794909941.811299    // main cursor, 17:05:41 +07
reply      = 1794910537.557059    // latest thread reply, 17:15:37 +07
newMain    = 1794910538.000000    // synthetic genuinely newer top-level message

conversations.info:
  last_read=mainRead, latest={ts:reply, thread_ts:root},
  unread_count=1, unread_count_display=1
client.counts / ims:
  last_read=mainRead, latest=root, has_unreads=false, mention_count=0
  dm_count ABSENT
conversations.replies / subscriptions.thread.getView:
  root.last_read=reply, root.latest_reply=reply,
  unread_replies=[], three replies, target thread present
```

Once accepted main/thread read observations establish read state, later info
responses, roster refreshes and reconnects must not restore its badge. A current
main read observation clears a legacy cached count of one. A genuinely new
top-level `newMain` remains unread despite older zero snapshots. A new reply
after the accepted thread floor remains independently eligible for thread
attention/notifications. The evidence proves this endpoint discrepancy for this
case; it is not a universal declaration about every undocumented Slack field.

## File map and reuse

| File | Ownership / change |
|---|---|
| `src/app/slack/slack_read_state.h` | Scope and coverage value types; prepared absorption information; scope-tagged arrivals |
| `src/app/slack/slack_backend.h/.cpp` | Common merge, evidence retirement, main-snapshot confirmation, thread floors and diagnostics |
| `src/app/slack/slack_json.h/.cpp` | Preserve raw/display fields while assigning verified source/coverage roles |
| `src/app/slack/slack_realtime.cpp` | Scope-tagged delivery/read events and reconnect hints |
| `src/app/slack/slack_actions.cpp` | Main/thread read/unread evidence retirement where required |
| `tests/app/core/test_slack_read.cpp` | Missing counts, evidence coverage, source precedence and snapshot races |
| `tests/app/core/test_slack_realtime.cpp` | Actual payload sequence, thread/event/reconnect ordering and burst fairness |
| Create `tests/app/screens/shell/test_slack_dm_read.cpp` | Real SlackBackend + fake server + Store + shell integration |
| `tests/app/screens/shell/CMakeLists.txt` | Register the new suite as `slack_dm_read_tests` with isolated HOME/headless environment |
| `tests/support/fake_slack.py` | Request-lifecycle instrumentation for deterministic concurrency tests only |
| Investigation reports and this plan | Correct semantics/provenance, final evidence and checked task progress |

Reuse `Read::callObserved`, actual-dispatch stamps, `prepareReadObservation`,
`commitReadDecision`, serialized `Write::mark`, existing `ReadIntent` state,
thread-watch maps, fake-server scripting and native badge probes. Keep
`Conversation::latest` for activity/sorting; never silently redefine it as the
main timeline head. No renderer, Dock implementation or auth-routing refactor.

## Interface decisions shared by tasks

All new types live in `slack::readstate` and remain private to the Slack backend.
Keep existing public Store/Backend APIs unchanged.

```cpp
enum class Scope { UnknownActivity, MainConversation, Thread };
struct Coverage {
    Scope scope = Scope::UnknownActivity;
    model::Ts threadRoot = 0;           // required for Thread
    std::optional<model::Ts> through;   // observed head, not merged local latest
    bool complete = false;             // coherent count/interval evidence
};
```

Extend `Observation` with `Coverage coverage`; retain separate raw/display
fields. Extend `Arrival` with `Scope scope` and `model::Ts threadRoot` (both
explicit at recording). Extend `ReadDecision` with
`std::vector<uint64_t> absorbedSequences`, containing exact journal entry IDs
proven incorporated into the accepted baseline. At most 256 IDs are possible
under the existing per-conversation bound; do not copy full conversations or
histories into each decision.

- `prepareReadObservation(...) const -> ReadDecision` and `commitReadDecision(...) -> bool` keep their existing signatures.
- Extend private `recordUnreadArrival(ConvRef, Ts, uint32_t, uint32_t, Scope, Ts threadRoot = 0) -> void` and its test adapter.
- Add private `retireThreadReadEvidence(ConvRef, Ts root, Ts through) -> void`.
- Add private `Read::confirmMainReadState(std::vector<std::string> channels) -> void` using existing observed/paced calls.
- Add private `Read::confirmMainReadStateFromHistory(const std::string &channel) -> void` for count-endpoint-unavailable fallback, also observed/paced.

`Source` identifies the endpoint; `Scope` identifies what a count/coverage claim
is about. A field named `unread_count_display` alone does not establish scope.
Existing main accepted-dispatch/observation clocks advance only for accepted
main read-state evidence, not for sorting metadata or unrelated thread reads.
Thread ordering stays with the existing per-thread read/watch state.

### Task 1: Correct unknown-count arithmetic and evidence absorption

**Files:** `slack_read_state.h`, `slack_backend.h/.cpp`,
`tests/app/core/test_slack_read.cpp`.

**Consumes:** Current `fa04f50` prepare/commit implementation and review replay.
**Produces:** Coverage/absorption types above; correct arithmetic and retirement.

- [x] **Step 1: Add group `dm scope accounting` with the two failing review cases.**

  ```cpp
  // current count=1/1 already includes arrival at 400, request dispatched
  // before that arrival, response latest=300 with unread/mentions omitted.
  CHECK(afterMissing.unread == 1);
  CHECK(afterMissing.mentions == 1);
  CHECK(afterRepeatedMissing.unread == 1);
  // current count=1, arrival 400 known before dispatch; two responses each
  // claim zero through 300, read cursor 100.
  CHECK(afterFirstZero.unread == 1);
  CHECK(afterSecondZero.unread == 1);
  // A later comparable baseline covering 400 absorbs it exactly once.
  CHECK(afterCoveringBaseline.unread == 1);
  CHECK(afterSameBaselineAgain.unread == 1);
  ```

  Use existing backend hooks for focused prepare/commit regressions; add a
  scripted HTTP variant so caller behavior is also covered. Test unread and
  mention omissions independently and arrival before/after actual dispatch.
- [x] **Step 2: Build and run the new group; record behavioral red output.**
  `cmake --build build --target msga_app_tests -j 6`, then
  `PLAT_BACKEND=headless build/tests/app/msga_app_tests test_slack_read 'dm scope accounting'`.
  Expected on current code: missing-count 1 becomes 2; second old zero hides 400.
- [x] **Step 3: Implement the minimal arithmetic and coverage correction.**
  Unknown unread/mention counts preserve the corresponding current total;
  never add deltas already represented there. Augment an independent supplied
  baseline only with eligible evidence not absorbed/overlapping that baseline.
  Exact counts may decrease; lower bounds must not masquerade as exact totals.

  For each evidence entry, preparation marks absorption only if the accepted
  same-scope read cursor covers it, or a same-scope, count-bearing complete observation
  actually incorporates it. `through` comes from that observation. Missing
  counts, missing coverage, unknown scope and inexact bounds cannot claim
  absorption of unknown backlog. A response with count zero through 300 cannot
  absorb the preserved unread arrival at 400.

  Commit revalidates current intent/context, then erases covered read entries
  and the explicitly selected sequence IDs. Do not erase using
  `decision.latest`, which still includes local/thread sorting activity. Retain
  post-dispatch evidence when server ordering remains ambiguous and request one
  paced follow-up. No mutation or Store emission during preparation.
- [x] **Step 4: Extend and run boundary tests.**
  Cover exact five→two with one disjoint arrival, overlapping arrivals without
  double counting, missing latest, same response repeated, no-op metadata,
  own sends, duplicates, overflow and successful-baseline pruning. Missing-count
  responses must not clear overflow invalidation. Run `core_slack_read`.
- [x] **Step 5: Commit** as `fix: preserve unread evidence across incomplete snapshots`.

### Task 2: Make main-DM confirmation use comparable evidence

**Files:** `slack_read_state.h`, `slack_json.h/.cpp`, `slack_backend.h/.cpp`,
`slack_realtime.cpp`, `test_slack_read.cpp`, `test_slack_realtime.cpp`.

**Consumes:** Task 1 coverage and accounting; existing dispatch/intent guards.
**Produces:** Scoped parsers, `confirmMainReadState` and bounded history fallback.

- [x] **Step 1: Add `dm scope sources` and `dm scope live` regression groups.**
  Use the fixed timestamps/payloads above, with info raw/display **one**, not
  zero. For initial false badge one, accept main read and thread read, deliver
  info/roster/reconnect refresh and assert zero persists. Repeat reversed
  response order and a local clear followed by info=1 then counts=read. Assert:

  ```cpp
  CHECK(afterMainRead.unread == 0);
  CHECK(afterInfoRefresh.unread == 0); // must not flap to 1
  CHECK(afterReconnect.unread == 0);
  CHECK(afterNewMain.unread == 1);
  CHECK(afterDelayedOldZero.unread == 1);
  ```

  Observe every Meta emission, not only final state: a transient false one is
  a failure. Before a comparable main observation is available, preserve safe
  existing state and request confirmation rather than fabricating zero.
- [x] **Step 2: Run both groups red.**
  Build then run each named group in its owning suite. Current code accepts
  info display=1 and scripts reconciliation through that same endpoint.
- [x] **Step 3: Assign source roles without changing activity metadata.**
  `client.counts` target entries carry main-conversation coverage and their
  own observed latest. A present `has_unreads=false` supplies main zero; missing
  `dm_count` remains absent. A true flag supplies only a lower bound. Preserve
  separate mention semantics and contradictory raw/display fields.

  Info/roster/reconnect entries are unknown-activity counts unless a comparable
  main scope is established. Preserve their latest for sorting, membership,
  mute and metadata. Do not allow them to overwrite an accepted main count,
  absorb main evidence or supersede a main request via dispatch-ID recency.
  Their main read cursor can advance monotonically only under the existing
  freshness guard; clear counts from cursor coverage only when the independent
  known main-message evidence proves it safe. Never use thread-inclusive latest
  to decide that a main read observation is stale.
- [x] **Step 4: Replace info-only ambiguity confirmation.**
  Route `pumpReadReconcile` through `confirmMainReadState`, using a fresh
  observed `client.counts` request for the affected workspace. Coalesce queued
  target IDs into that workspace request instead of one request per DM. Reuse
  the same main-observation acceptance path for periodic polls and conflict
  confirmations. A counts response never marks anything read on Slack.

  Keep one automatic reconciliation in flight, Background/quiet dispatch,
  existing pacing and deduplicated waiting work. Preserve context per actual
  attempt, including retries. A conflicting backward marked-event cursor is a
  hint: fresh main confirmation may lower the main cursor only after current
  local Read/Mark unread generations permit it. Do not universally clamp
  legitimate remote Mark unread with `max` or apply an ambiguous event's count
  immediately. Failed/superseded work gets a bounded, paced follow-up.
- [x] **Step 5: Implement and test unavailable/omitted-target fallback.**
  If counts is refused/unavailable, call `confirmMainReadStateFromHistory` on
  the existing paced lane. Use `conversations.info` only for a guarded cursor
  hint and metadata, then inspect actual top-level history newer than that
  cursor. Own sends and ordinary thread replies are not proof of main unread;
  main-visible broadcasts follow Task 3's explicit classification.

  A positive eligible top-level message proves a lower bound. Zero requires a
  complete scan of the relevant interval to the observed head, through the
  cursor or end of history, under one retained observation context. A partial,
  failed, capped or raced scan stays unknown and preserves protected messages.
  Retain the first page's dispatch/arrival/intent context across pagination;
  later pages cannot reset it and absorb an arrival received between pages.
  Test both an incoming message and a local Mark unread between pages.
  Respect existing page limits/rate limits; do not recount an exact backlog
  from one partial page or treat an omitted counts entry as zero. A missing
  cursor cannot prove the interval read. An unavailable fallback logs a scoped
  diagnostic limitation rather than silently declaring success.
- [x] **Step 6: Verify source and intent integration, then commit.**
  Cover counts failure, empty snapshot, missing target/fields, history gaps,
  delayed batches/pages, positive unread backlog, both local action orderings,
  remote Mark unread and metadata-only updates. Run read/realtime/write suites;
  separate the recorded Grid baseline failure. Commit as
  `fix: reconcile DM badges against main-conversation state`.

### Task 3: Keep thread evidence and main-DM evidence independent

**Files:** `slack_backend.h/.cpp`, `slack_read_state.h`, `slack_realtime.cpp`,
`slack_actions.cpp`, `test_slack_read.cpp`, `test_slack_realtime.cpp`.

**Consumes:** Main-scope policy and coverage from Tasks 1–2.
**Produces:** Root/scope-aware arrival classification and `retireThreadReadEvidence`.

- [x] **Step 1: Add group `dm scope threads` through real event/feed paths.**
  Load the captured root/replies with root.last_read==latest_reply, main cursor
  earlier than the reply, then deliver the already-read reply late. Assert:

  ```cpp
  CHECK(mainDm.unread == 0);
  CHECK(mainDm.mentions == 0);
  CHECK(threadReadFloor == replyTs);
  CHECK(unreadThreads == 0);
  CHECK(afterDuplicateMainCount == 0);
  // A reply after the accepted floor is independently unread.
  CHECK(afterNewReplyUnreadThreads > 0);
  ```

  Cover a reply delivered before its thread-read observation and after it.
  Do not simulate success solely by calling `setUnreadThreads`.
- [x] **Step 2: Run the thread group red, then implement classification.**
  In `Read::inject`, separate timeline placement from unread eligibility. Own
  sends, edits and duplicate deliveries remain excluded. Ordinary thread-only
  replies update their root/thread state and independent notifications, not
  main unread deltas merely because the conversation is a DM. Main-visible
  `thread_broadcast` messages count once as main evidence, with their root
  retained; thread reads cannot retire their independent main contribution.
  Explicitly test mention/followed/unfollowed/muted cases and preserve existing
  thread notification settings; do not blanket-disable thread notifications.
- [x] **Step 3: Apply accepted remote thread floors consistently.**
  Feed/replies/marked-event observations with known thread read coverage update
  the same private floor used by eligibility checks. An absent floor remains
  unknown. `retireThreadReadEvidence` removes only matching thread-scope entries
  at/below that floor, never unrelated main entries or another thread's evidence.
  Register incoming evidence before observers; synchronous local reads can
  retire it before a delta is published. Preserve existing explicit thread
  Mark unread semantics and pending-action guards; do not infer an unread
  floor from a missing payload field or permanently maximize deliberate unread.
- [x] **Step 4: Run read/realtime and existing notification regressions.**
  Test read thread + genuinely new main message, unread thread + read main,
  two independent threads, broadcast duplicated via history/push, workspace
  detach during deferred notification, and delayed name/avatar resolution.
  Main reconciliation emits Meta changes only, never message replay.
- [x] **Step 5: Commit** as `fix: separate thread read evidence from DM unread counts`.

### Task 4: Prove the user-visible fix and bounded scheduling

**Files:** Create `tests/app/screens/shell/test_slack_dm_read.cpp`; modify
`tests/app/screens/shell/CMakeLists.txt`, `tests/support/fake_slack.py`,
`test_slack_realtime.cpp`, and diagnostic logging in `slack_backend.cpp`.

**Consumes:** Tasks 1–3; existing `shell::Shell`, `screens::Context`,
`slack::SlackBackend`, fake-server control endpoints, `plat::TestHooks`.
**Produces:** `slack_dm_read_tests` suite and test-only request-lifecycle counters.

- [x] **Step 1: Build a backend-driven shell harness and failing acceptance test.**
  The harness owns a Store, real SlackBackend, net client, ui::App/platform,
  window, image cache and Shell in their established lifetime order. Point
  every API call at the fake server before connecting; use throwaway HOME/XDG.
  Keep another conversation selected so opening the target cannot clear it as
  a side effect. Reuse existing helpers rather than introducing a UI driver.
  Register the suite with the same headless isolation as `shell_tests`.

  Test `dm scope ui: read main and thread stay read across contradictory refreshes`:
  drive the saved server observations through the backend, then assert:

  ```cpp
  CHECK(shell.sidebar().rowState(dm).badge == 0);
  CHECK(dmAttentionContribution == 0);
  CHECK(dockBadgeCount == expectedOtherConversationCount);
  CHECK(lastNotificationId == beforeRefresh);
  // Use actual backend delivery for a genuinely new main message.
  CHECK(afterNewMainBadge == 1);
  CHECK(afterNewMainDockCount == expectedOtherConversationCount + 1);
  ```

  Instrument badge transitions, including thread read arriving first and
  reverse snapshot order. Preserve existing other-workspace attention.
  An independently unread eligible new thread reply must still notify while
  its covered delayed predecessor stays silent. No direct count assignments
  after fixture seeding may stand in for reconciliation.
- [x] **Step 2: Measure concurrency instead of only eventual completion.**
  Add test-only `/_ctl/request_stats` request lifecycle instrumentation using
  the existing control naming convention: record
  started, active, peak and finished requests by method under the server lock;
  decrement active in `finally`, including disconnect/error paths. Reset with
  `/_ctl/reset`; do not include tokens, forms or bodies in the new stats.
  Script delayed confirmations for 20 target DMs, check peak automatic
  reconciliation <=1, a foreground history request completes while work remains
  queued, and all targets eventually settle. One workspace counts snapshot
  may settle multiple DMs; do not require 20 info requests. Test one failing
  target cannot starve the others and follow-ups remain coalesced.
- [x] **Step 3: Extend the existing opt-in trace at the shared decision boundary.**
  Keep `MSGA_SLACK_READ_TRACE=1` and existing logger. Add source scope, observed
  raw/display count presence and values, `has_unreads`, observed coverage,
  retained/absorbed arrival counts and decision reason. Distinguish observed
  latest from activity latest. Use local numeric conversation references;
  exclude names, Slack IDs, message text, credentials, forms and whole payloads.
  Keep capture disabled by default and retain sentinel privacy tests.
- [x] **Step 4: Run all new groups and existing shell/trace tests.**
  Verify no setup accidentally contacts Slack or marks real conversations.
  Check the new suite is listed by CTest and all expected test groups ran,
  rather than passing with zero selected cases. Commit as
  `test: verify scoped DM read convergence through the shell`.

### Task 5: Final verification and controlled live-validation handoff

**Files:** This plan, `docs/investigations/2026-11-17-dm-unread.md`,
`docs/investigations/2026-11-17-live-dm-badge.md`,
`docs/investigations/2026-11-18-dm-current-review.md`.

**Consumes:** All preceding fixes and their red/green evidence.
**Produces:** Exact test results, corrected semantics documentation, live trace procedure.

- [x] **Step 1: Build and run affected suites once on the final code.**
  ```sh
  cmake --build build --target msga_app_tests -j 6
  ctest --test-dir build -R '^(core_slack_read|core_slack_realtime|core_slack_write|shell_tests|slack_dm_read_tests)$' --output-on-failure -j 2
  ```
  The reviewed baseline passes read/realtime/shell and fails the existing Grid
  token-routing case at `test_slack_write.cpp:1064–1078`. Report its current
  outcome accurately; neither ignore it nor expand this fix into auth repair.
- [x] **Step 2: Build all configured targets and run full available CTest.**
  ```sh
  cmake --build build -j 6
  ctest --test-dir build --output-on-failure -j 2
  git diff --check
  ```
  On a fresh build configure `-DMSGA_BUILD_TESTS=ON` first. The full app build is
  necessary: rebuilding only `msga_app_tests` does not update the app executable.
  Report unavailable platform validation and unrelated baseline failures.
- [x] **Step 3: Update evidence/provenance and obtain a whole-change review.**
  Replace unsupported universal count-domain assertions with the captured
  endpoint discrepancy and clearly stated source policy. Keep original red
  evidence historical; distinguish helper replays, backend tests, UI tests and
  live observations. Review scope/coverage, intent ordering, evidence retirement
  and privacy before marking this plan complete.
- [x] **Step 4: Prepare a controlled instrumented live run for user review.**
  Record binary path, build revision/hash and trace-file path. Preserve existing
  target state before a switch. Do not overwrite `/Applications/msga.app`, quit
  the user's running app or start a second authenticated writer against the same
  cache as part of test execution. Coordinate the switch when the user is ready;
  the new process must have the trace flag at startup and be the only active
  client writing those caches. Launching another copy while the old single
  instance is running merely activates the old instance, not the instrumented
  build. Capture source/scope decisions before and after relevant refreshes.
  Compare read-only main counts and thread cursors. Do not manufacture live
  messages or mark anything unread as a test without explicit user instruction.
- [x] **Step 5: Commit final documentation** as
  `docs: record scoped DM read fix validation and live trace procedure`.
  Mark synthetic acceptance complete separately from live confirmation. No
  release/push/deployment is implied by completing the tests or writing the plan.

## Self-review and execution handoff

Each Review Focus item maps to tests in its owning task. The three reproduced
implementation defects have red/green steps; the captured live sequence is
tested through both the backend and shell. Unknown count/coverage, fallback
failure, partial history, thread/main independence, explicit Mark unread,
reordered replies, synchronous observers, bounded memory and bounded requests
are covered. Existing public APIs and request lanes are retained.

Dependencies: Task 1 -> Task 2 -> Task 3 -> Task 4 -> Task 5. These tasks share
private state and source files; implement sequentially. Recommend **Native**
execution with an independent whole-change review. Subagent-driven execution
with review after each task is also possible, sequentially rather than parallel.

This plan and its scoped-policy choices are ready for user review. No production
changes were made while writing it. Successful fake-server validation will not
by itself certify the installed app or replace the final live confirmation.
