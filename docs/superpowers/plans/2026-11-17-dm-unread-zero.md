# DM Unread Zero-Count Reconciliation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clear false DM unread counts without hiding genuinely newer incoming messages or undoing intentional Read/Mark unread actions.

**Architecture:** Add Slack-private observation metadata and bounded incoming-message evidence to the existing backend. Reuse request pacing, serialized mark writes, and local-intent generations; reconcile ambiguous WebSocket events through fresh, versioned requests. No replacement read-state subsystem.

**Tech Stack:** C++20, existing CMake/Ninja build, CTest, Python fake Slack server, headless platform probes.

**Spec:** `docs/superpowers/specs/2026-11-17-dm-unread-zero-design.md` (approved).

## Global Constraints

- C++20 and existing test/fake-server infrastructure; no new dependency, broad read-state rewrite, OS notification rewrite, release, push, or deployment.
- No change to the 30-day notification policy. No already-posted OS banner removal.
- Do not add a public Backend API or cache-format change just to carry transient Slack response metadata.
- A read-state update alone does not replay message notifications.
- Never recount the whole backlog from a partially loaded history page.
- Do not invent protocol fields. Passing fake-server tests will not by itself prove the user's exact live incident is resolved.
- Preserve unrelated untracked `2026-11-17-dm-read-state*` documents. They describe the earlier fix, not this follow-up.

## Review Focus

1. Missing, negative, oversized, or nonnumeric count fields must not become a fabricated unread total (Task 1).
2. Own sends, duplicate events, synchronous observer reads, and independent thread replies must not leave incoming-arrival evidence behind (Task 2).
3. Joined/queued/retried requests and overlapping snapshots must use their actual dispatch context and eventually converge (Tasks 2–3).
4. A stale positive marked event at the same cursor must not revive a cleared badge; legitimate remote Mark unread must remain possible (Task 4).
5. Evidence overflow, reconciliation failure, and backend destruction must preserve safe state without unbounded storage or callbacks into freed state (Tasks 2–5).

---

## Execution context and file map

Work from the existing isolated worktree after the execution skill checks its
instructions and status. Use the graph tools for discovery. Reproduction and
baseline evidence: `docs/investigations/2026-11-17-dm-unread.md`; patch:
`docs/investigations/2026-11-17-dm-unread-repro.patch`. Do not blindly apply the
whole patch: restore each test in its owning task and adapt the realtime fixture
as described below.

| File | Responsibility |
|---|---|
| Create `src/app/slack/slack_read_state.h` | Small private value types for count presence, observations, dispatch stamps, and arrivals; no new service |
| `src/app/slack/slack_json.h/.cpp` | Endpoint-aware observation parsing alongside existing model mapping |
| `src/app/slack/slack_backend.h/.cpp` | Request context, bounded arrival evidence, observation merging, counts/roster/DM enrichment, paced reconciliation |
| `src/app/slack/slack_actions.cpp` | Retire incoming evidence and retain local-intent ordering on Read/Mark unread |
| `src/app/slack/slack_realtime.cpp` | Preserve observation stamps in batched patches and reconcile conflicting marked events |
| `tests/app/core/test_slack_read.cpp` | Mapper, polling, queued/retried request and snapshot races |
| `tests/app/core/test_slack_realtime.cpp` | WebSocket conflicts, reconnect, remote read/unread, lifecycle |
| `tests/app/core/test_slack_write.cpp` | Existing serialized writes and local-action regressions |
| `tests/app/screens/shell/test_shell.cpp` | Visible badges, independent threads, no banner replay |
| `tests/support/fake_slack.py` | Only if existing scripted responses/delays/control endpoints cannot express a required scenario |
| `docs/investigations/2026-11-17-dm-unread.md` | Final evidence, remaining live-validation limits, diagnostic usage |

The new header is included by existing compilation units; no new library or
source-file build registration is required. Keep existing public model and
Backend interfaces unchanged.

### Task 1: Preserve explicit zero and missing-count information

**Files:** Create `src/app/slack/slack_read_state.h`; modify
`src/app/slack/slack_json.h/.cpp`; test `tests/app/core/test_slack_read.cpp`.

**Interfaces (all inside `slack::readstate` unless noted):**
- `enum class Source { ConversationInfo, Roster, Counts, Marked, Reconcile };`
- `struct Count { std::optional<uint32_t> value; bool exact = true; };`
- `struct CountPair { Count unread, mentions; };`
- `struct Observation { std::optional<model::Ts> lastRead, latest; CountPair raw, display; std::optional<bool> hasUnreads; Source source; };`
- `mapjson::conversationReadObservation(const json::Value &o, readstate::Source source) -> readstate::Observation`.
- `mapjson::countsReadObservation(const json::Value &o, bool direct) -> readstate::Observation`.
- Add `readstate::Observation read` to `mapjson::Counts`. Keep its existing numeric fields as movement-detection projections, not authoritative presence information.

- [ ] **Step 1: Restore the mapper reproduction and add table-driven parsing cases.**
  Name the group `dm zero parsing`. Pin these assertions:

  ```cpp
  // Each input is an object passed to conversationReadObservation.
  // Raw and display values are not interchangeable: preserve raw total
  // for conversations.info and the endpoint-specific display semantics elsewhere.
  // input {unread_count:0, latest:300, last_read:200}
  CHECK(obs.raw.unread.value.has_value());
  CHECK(*obs.raw.unread.value == 0);
  CHECK(mappedConversation.unread == 0);
  // input omits both unread_count and any applicable authoritative flag
  CHECK(!missing.raw.unread.value.has_value());
  // input {unread_count:4}
  CHECK(positive.raw.unread.value == 4);
  // negative, overflow beyond UINT32_MAX, string/object/null => unknown, not wraparound
  CHECK(!malformed.raw.unread.value.has_value());
  // mentions omitted versus explicitly zero remain distinguishable too
  CHECK(!absentMention.raw.mentions.value.has_value());
  CHECK(zeroMention.raw.mentions.value == 0);
  ```

  **Contract gate:** first add an endpoint/field semantics table to the
  investigation, with evidence provenance. Do not establish real Slack semantics
  from hand-authored fake responses alone. Keep these domains separate:

  | Endpoint/source | Fields retained | Required characterization |
  |---|---|---|
  | `conversations.info` / roster entries | `unread_count`, `mention_count` in raw; applicable `*_display` fields in display | Which counts describe the visible badge, and which cover filtered/thread/own activity? |
  | `*_marked` | `unread_count_display`, `mention_count_display` in display | Cursor/count scope, omission, and whether a fresh endpoint can confirm the same metric |
  | `client.counts` IM/MPIM/channel entries | `dm_count`, `mention_count`, `has_unreads` with their source semantics retained | Is `dm_count` exact and equivalent to the badge metric for each conversation kind? |

  For the last row, preserve `dm_count` as source-specific raw data until its
  domain is established; `has_unreads` is a separate optional flag. A true flag
  alone is only a lower bound of one, not an exact message count. Do not label
  `dm_count` exact merely because the current code treats it as a number.

  Add raw/display disagreement fixtures (raw=2, display=0; raw=0, display=2),
  display-only, raw-only, explicit false versus absent flags, and conflicting
  flags/counts. Assert both domains survive parsing without substitution.
  Visible state uses a characterized display-compatible metric. An unavailable
  display field must not silently fall back to a non-equivalent raw total.
  If no count-bearing endpoint is known to confirm the same metric, retain safe
  state and report that limitation; do not declare convergence proven by a fake
  response. Characterization may use documented contracts or sanitized real
  captures, with all credentials and message content removed.
- [ ] **Step 2: Build and run the new group; record the expected failures.**
  Run `cmake --build build --target msga_app_tests -j 6`, then
  `PLAT_BACKEND=headless build/tests/app/msga_app_tests test_slack_read 'dm zero parsing'`.
  Missing new symbols initially fail to compile; after declaring interfaces,
  verify actual zero/missing assertions fail against the old mapper behavior.
- [ ] **Step 3: Implement the parsing interfaces and remove timestamp-only unread inference from `toConversation`.**
  Preserve existing positive model projections. Use optional observation metadata
  at merge boundaries in subsequent tasks so a missing model projection of zero
  is never treated as an authoritative zero. Do not put presence flags in the
  persisted Conversation type. Preserve `toCounts` movement detection behavior;
  its numeric projections are not an authorization to mix raw/display domains.
- [ ] **Step 4: Run the parsing group and `ctest --test-dir build -R '^core_slack_read$' --output-on-failure`.**
  Expected: new assertions pass. If a legacy assertion relies on fabricated
  counts, replace it with a fixture carrying real count/flag information and
  preserve its original behavioral purpose.
- [ ] **Step 5: Commit only this task's files** with message
  `fix: preserve explicit Slack unread zero counts`.

### Task 2: Reconcile count polls with protected incoming evidence

**Files:** `slack_read_state.h`, `slack_backend.h/.cpp`, `slack_actions.cpp`,
`tests/app/core/test_slack_read.cpp`, `tests/app/core/test_slack_write.cpp`.

**Consumes:** Task 1 observations.
**Produces:**
- `readstate::DispatchContext { uint64_t dispatchId, readEpoch, arrivalEpoch, observationEpoch; }`.
- `readstate::Arrival { model::Ts ts; uint64_t sequence; uint32_t unread, mentions; }`.
- Private `SlackBackend::captureReadDispatch() -> readstate::DispatchContext`.
- Private `SlackBackend::recordUnreadArrival(model::ConvRef c, model::Ts ts, uint32_t unread, uint32_t mentions) -> void`.
- Private `SlackBackend::retireReadEvidence(const std::string &channel, model::Ts through) -> void`.
- Private `SlackBackend::applyReadObservation(model::ConvRef c, const readstate::Observation &obs, const readstate::DispatchContext &context, bool allowBackwardCursor = false) -> void`.
- `readstate::ReadDecision { model::Ts lastRead, latest; uint32_t unread, mentions; bool accept, refresh; const char *reason; }`.
- Private `SlackBackend::prepareReadObservation(model::ConvRef c, const model::Conversation &candidate, const readstate::Observation &obs, const readstate::DispatchContext &context, bool allowBackwardCursor = false) const -> readstate::ReadDecision`; no Store emission or mutation during preparation. `applyReadObservation` wraps this decision for standalone read-state updates.
- Private `SlackBackend::commitReadDecision(model::ConvRef c, const readstate::ReadDecision &decision, const readstate::DispatchContext &context) -> bool`; revalidate freshness, advance accepted bookkeeping/retire absorbed evidence, and queue any required refresh, without Store emission. Return false when preparation was superseded. Decision reason strings have static lifetime. The caller publishes fields only after a successful commit, or preserves current fields on rejection.
- Private `SlackBackend::queueReadReconcile(const std::string &channel) -> void`; reuse existing `reconcileConversationReadState` for dispatch.
- `Read::ObservedDone = std::function<void(const json::Document &, const std::string &, const readstate::DispatchContext &)>`.
- `Read::callObserved(std::string method, std::string form, ObservedDone done, Lane lane = Lane::Normal, bool quiet = false) -> void`.

- [ ] **Step 1: Add fake-server tests under group `dm zero polling`.**
  Use timestamps 200 (incoming), 300 (own reply), 400 (later incoming).
  Assert zero observation at read=200 clears an old count of one despite
  latest=300; delayed zero dispatched before incoming=400 preserves that
  arrival; the same delay with only own=400 clears; duplicate 400 counts once.
  Add a zero response omitting mention count: unread zero clears stale mentions
  when no protected incoming evidence remains; missing unread with mention zero
  must not clear a known backlog of five from partially loaded history.
  Add synchronous-observer Read on arrival and assert it leaves no stale evidence.
- [ ] **Step 2: Run the new group and record behavioral failures.**
  Run `PLAT_BACKEND=headless build/tests/app/msga_app_tests test_slack_read 'dm zero polling'`
  after building. Expected before the fix: stale-badge assertions fail.
- [ ] **Step 3: Implement dispatch-aware calls, incoming evidence, and the common merge.**
  `Call` carries an immutable completion stamp set in `Read::send` immediately
  before `b.api`, refreshed on each actual retry. Adapt existing `ApiDone`
  callbacks through a wrapper without changing the public callback type;
  joined calls receive the same actual-request context. Update `join`, final
  error completion, and cancellation/lifecycle handling consistently.

  Keep workspace-global monotonic epochs with per-conversation latest local
  intent, accepted-observation epoch/dispatch ID, and arrival entries. This
  avoids copying every conversation into every count request. Existing
  `_readEpoch` remains the local-intent clock; new clocks do not replace it.

  Record only deltas eligible under `Read::inject`'s existing rules, before
  `addMessage`/`announceReply` notifies observers. Preserve duplicate detection;
  retire covered evidence on local reads and accepted read cursors. Own sends,
  edits and historical page loading create no incoming evidence. Independent
  thread state keeps its existing owner.

  Set a private limit of **256 arrival entries per conversation**. On overflow,
  bump an evidence-invalidated epoch, retain conservative unread state, and
  request a paced refresh. Do not use a truncated journal to clear counts.
  A subsequent successful request dispatched after invalidation, with no new
  arrivals during flight, may establish a fresh baseline and release the
  invalidation; failed/overlapped requests cannot do so.
  After a successful authoritative baseline, discard evidence it safely absorbs
  even when those messages remain unread. Pruning only by the read cursor would
  cause a busy unread DM to overflow repeatedly. Test steady-state positive
  snapshots as well as local-read retirement.

  Apply accepted explicit zero to old counts; protect eligible post-dispatch
  arrivals still above the accepted read cursor. Unknown counts preserve the
  old backlog unless accepted cursor coverage independently proves it read.
  For positive totals, add definitely disjoint newer deltas only once; for
  uncertain overlap use a conservative maximum rather than summing duplicates,
  and request reconciliation. Inexact `has_unreads` is a lower bound, not an
  authoritative total that can shrink a larger known backlog.

  Reject results predating local intent/pending writes or accepted observations.
  Rejection of potentially newer overlapping work schedules one refresh unless
  already covered by an accepted later dispatch/current-context refresh.
  Keep failed refresh work queued for a paced retry; do not recurse immediately.
  Automatic reconciliation uses the existing **Background** lane, quiet mode,
  existing `kPacedMs` spacing, and **at most one automatic reconciliation in
  flight per workspace**. Keep one deduplicated waiting entry per conversation;
  drain fairly through the existing tick/queue machinery. A persistent failure
  backs off under existing request error policy and cannot block other DMs or
  promote automated work onto the Normal lane. User mark-write ordering and
  foreground requests retain their existing priority. Do not create a separate
  timer per incoming message or retry indefinitely inside one callback.
- [ ] **Step 4: Add ordering/lifecycle cases to the same test group and make them pass.**
  Force a request to wait behind a 429, act locally while queued, and verify
  context is captured at dispatch. Repeat with a transport retry and a new
  incoming message before retry dispatch. Verify joined calls share context;
  reverse-completing requests converge after at most one outstanding follow-up;
  overflow at entry 257 does not erase unread state and eventually recovers.
  Destroy the backend during a delayed response and check no callback touches it.
  Use fake-server request counters/pumpUntil, not fixed sleeps as proof of order.
  Add `exact count decreases`: accepted display-compatible total five becomes
  two at a newer read cursor even though latest remains ahead of that cursor;
  do not preserve five with `max`. With one definitely disjoint newer arrival,
  result is three; with overlap unresolved, do not double-count it, and refresh.
  Check unread and mention totals independently. A lower-bound-only observation
  of one cannot shrink a known exact total of five.
- [ ] **Step 5: Run `core_slack_read` and `core_slack_write`, and commit** as
  `fix: reconcile DM count snapshots with incoming arrivals`.
  Record the known Grid token-routing failure separately if unchanged.

### Task 3: Carry observations through roster, DM enrichment, and reconnect

**Files:** `slack_read_state.h`, `slack_json.h/.cpp`, `slack_backend.h/.cpp`,
`slack_actions.cpp`, `slack_realtime.cpp`, `tests/app/core/test_slack_read.cpp`,
`tests/app/core/test_slack_realtime.cpp`.

**Consumes:** Tasks 1–2 metadata and merge methods.
**Produces:**
- `readstate::ConversationObservation { model::Conversation info; Observation read; DispatchContext context; }`.
- Private `SlackBackend::sweepInfo(const std::string &id, std::function<void(const readstate::ConversationObservation *)> done) -> void`; null still means failure, pointer callback-scoped.
- `Read::ObservedPage = std::function<void(const json::Value &, const readstate::DispatchContext &)>`.
- `Read::paginateObserved(std::string method, std::string form, std::string key, ObservedPage onPage, std::function<void(const std::string &)> onDone, int maxPages = 0, Lane lane = Lane::Normal, bool quiet = false) -> void`.
- Private `SlackBackend::mergeConversation(model::Conversation fresh, const readstate::Observation *obs = nullptr, const readstate::DispatchContext *context = nullptr) -> model::ConvRef`.
- `Read::carryLocal(model::Conversation &fresh, const model::Conversation &old, const readstate::Observation *obs = nullptr, const readstate::DispatchContext *context = nullptr) const -> readstate::ReadDecision`. Metadata-only callers without observation/context preserve existing read fields and return a preserve decision; they must not reinterpret numeric defaults as server read information. Otherwise use Task 2's non-emitting `prepareReadObservation` against the merged candidate metadata and return its decision. The caller uses `commitReadDecision` immediately before its Store merge; on rejection refresh the candidate's read fields from the current Store. Do not emit a read update before publishing the metadata merge.

- [ ] **Step 1: Add group `dm zero sources` in both read/realtime suites.**
  Repeat zero/missing/own-newer cases through roster reload, enrichment, and
  reconnect patches. For a delayed batched patch, perform local Mark unread
  after HTTP completion but before patch application: assert it survives.
  Load two roster pages with different dispatch contexts and assert a later
  page cannot lend its freshness to an older page's read observation.
  Add synchronous observer tests: one changed conversation emits a single
  coherent Meta update, including its final membership/mute/count/cursor fields.
  An observer on the first roster entry marks a later entry unread; publishing
  that later entry must recheck its current intent and preserve that action.
- [ ] **Step 2: Build and run each suite's `dm zero sources` group.**
  Expected: paths still using numeric maxima fail at least their missing/zero
  or stale-batch assertions before migration.
- [ ] **Step 3: Migrate all remaining read-observation boundaries.**
  Extend the existing Pager to deliver optional observed-page callbacks; preserve
  normal pagination behavior and retain each page entry's own context. Parse
  roster read observations before converting them to plain Conversation values.
  Pass observations alongside `carryLocal`/`mergeConversation`; no consumer may
  infer presence from a model zero. Migrate `enrichDmActivity`, `applyActivity`,
  `sweepInfo`, `Live::Patch`/`applyPatches`, and explicit reconciliation.
  Prepare read and metadata fields without emitting, then publish the coherent
  conversation once using existing Store APIs. Advance accepted bookkeeping
  before the emit, and never write an old prepared candidate after a callback.
  For existing roster entries, prepare/revalidate/commit each entry immediately
  before its Store merge; do not prepare all entries and publish them later.
  Batch only newly added entries to preserve cold-start roster efficiency.
  Reentrant callbacks must not invalidate a held Store reference or overwrite a
  later conversation's newer action. No new public Store transaction API is
  needed for this ordering.
  Audit every `toConversation` caller, including `createChannel` and
  `loadChannelCanvas` in `slack_actions.cpp`: metadata-only responses must not
  overwrite existing read fields. In `client.counts` tests use its actual
  `dm_count`/`has_unreads` fixture contract, not an invented `unread_count` field.
  Remove superseded read-count maxima once all callers use the common policy.
  Keep notification delivery and metadata/member/section merges unchanged.
- [ ] **Step 4: Add missing-count and source-failure regressions.**
  A failed response must not clear; an absent conversation in `client.counts`
  must not be assumed read without a characterized complete-snapshot contract.
  Preserve unknown unread state and obtain an explicit fresh observation when
  necessary. Empty snapshots must not silently clear every conversation.
  Verify all source groups and existing read/realtime suites pass.
- [ ] **Step 5: Commit** as `fix: share DM read reconciliation across snapshot sources`.

### Task 4: Reconcile conflicting marked events and preserve user intent

**Files:** `slack_backend.h/.cpp`, `slack_realtime.cpp`, `slack_actions.cpp`,
`tests/app/core/test_slack_realtime.cpp`, `tests/app/core/test_slack_write.cpp`.

**Consumes:** Common observation parser, dispatch context, paced reconciliation.
**Produces:** Existing private
`applyMarkedEvent(const std::string &channel, model::Ts ts, uint32_t unread, uint32_t mentions)`
is replaced by `applyMarkedEvent(const std::string &channel, const readstate::Observation &obs) -> void`
so omitted fields survive event parsing. Preserve public mark APIs and their
serialized/coalesced write queue.

- [ ] **Step 1: Restore the realtime reproduction under group `dm zero events`.**
  Provide a matching `conversations.info` response with cursor=200, latest=300,
  explicit unread=0 and mention=0. Send the original zero `im_marked` event.
  Include display zero fields in the fresh response when the characterized
  contract requires them; raw zero alone cannot stand in for display zero.
  Assert eventual unread=0/mentions=0 with pumpUntil after reconciliation, and
  observe no Append/Arrived emitted by the state change. Do not accept a test
  which simply invokes the merge helper and bypasses the WebSocket path.
- [ ] **Step 2: Add stale-positive and intent cases and run them red.**
  After accepted read/zero at cursor 200 with newer own=300, send positive count
  at the same cursor; script fresh zero and assert it cannot revive the badge.
  Add an eligible incoming=400 during that reconciliation: it must remain unread.
  Preserve local Read→Mark unread and Mark unread→Read ordering, remote backward
  Mark unread with unloaded history, and isolation between DMs/workspaces.
- [ ] **Step 3: Implement conflict classification at `applyMarkedEvent`.**
  Parse event display fields as optional values with source Marked. Retain
  existing pending-intent/echo barriers. Ambiguous zero, backward cursors, and
  contradictory positive same-cursor events use `queueReadReconcile`; do not
  mutate counts from those ambiguous events. The fresh response may move the
  cursor backward only after the captured intent/context checks pass. Missing
  event cursor/count data cannot overwrite valid state with fabricated zero.
  Fresh confirmation must use the same count domain established by Task 1;
  raw unread=2 is not confirmation that display unread=0 was stale. Exercise
  raw=2/display=0 through the actual marked-event-to-refetch path, not just a
  parser unit test. If confirmation lacks a comparable field, preserve safe
  state and surface that unresolved contract in diagnostics rather than guess.
- [ ] **Step 4: Test repeated conflicts, failures, and recovery.**
  A burst of 20 conflicting events coalesces to one in-flight reconciliation
  plus at most one necessary queued refresh. Script an initial failure, then
  success; safe counts persist until success and converge afterward. Check
  no immediate request loop and preserve serial mark writes and thread reads.
  Repeat with 20 distinct DMs: assert automatic reconciliations stay at one
  in flight per workspace, foreground calls can still dispatch, and every
  queued DM eventually gets a turn after the failure clears. Verify absorbed
  arrival evidence does not accumulate again across unchanged positive polls.
- [ ] **Step 5: Run realtime/write suites and commit** as
  `fix: reconcile ambiguous Slack DM marked events`.

### Task 5: Verify visible behavior and add opt-in read-state diagnostics

**Files:** `slack_backend.h/.cpp`, `tests/app/core/test_slack_read.cpp`,
`tests/app/screens/shell/test_shell.cpp`, `docs/investigations/2026-11-17-dm-unread.md`.

**Consumes:** Completed observation/arrival policy.
**Produces:** Private `SlackBackend::traceReadObservation(model::ConvRef c, const readstate::Observation &obs, const readstate::DispatchContext &context, model::Ts beforeRead, uint32_t beforeUnread, uint32_t beforeMentions, const char *decision) const -> void`.
Enable only when `MSGA_SLACK_READ_TRACE=1`; emit through existing `LOG_INFO`
tag `slack-read`, with local numeric conversation reference, source, before/after
cursor/counts, dispatch/epoch values and decision. Never emit names, Slack IDs,
message text, credentials, forms or full responses.

- [ ] **Step 1: Add visible-state tests under group `dm zero ui`.**
  Assert a remote read with newer own reply removes the DM count and workspace
  attention count; a real newer incoming DM retains them. Reconciliation emits
  no desktop banner, and existing already-read delayed name/avatar notification
  suppression remains passing. A thread reply with its own unread floor remains
  independently eligible. Use existing TwoWorkspaces and headless probes.
- [ ] **Step 2: Add a diagnostic output test and implement the trace helper.**
  Capture existing base logger output to an isolated temporary file, enable the
  flag, exercise accept/defer/reject paths and assert expected decision/context
  fields exist. Insert sentinel message text/name/ID/token into the fixture and
  assert none appear in `slack-read` records. Disabled mode emits no such records.
  Call the helper at common observation decisions, not from every UI repaint.
- [ ] **Step 3: Build the test executable and run affected suites.**
  Run `cmake --build build --target msga_app_tests -j 6` then
  `ctest --test-dir build -R '^(core_slack_read|core_slack_realtime|core_slack_write|shell_tests)$' --output-on-failure -j 2`.
  Expected: new and existing relevant regressions pass; separately report any
  unchanged Enterprise Grid baseline failure, never silently mark it passing.
- [ ] **Step 4: Build all configured targets and run the available full suite.**
  Run `cmake --build build -j 6`, then
  `ctest --test-dir build --output-on-failure -j 2` and `git diff --check`.
  This build directory already has `MSGA_BUILD_TESTS=ON`; on a fresh tree configure
  with `cmake -S . -B build -G Ninja -DMSGA_BUILD_TESTS=ON` first. Report skipped
  platform/live checks and pre-existing failures with exact output and scope.
- [ ] **Step 5: Update the investigation with test evidence and tracing instructions.**
  Retain original red-test evidence. Distinguish confirmed synthetic regression
  coverage from an uncaptured live incident. Document how to run the newly built
  app with `MSGA_SLACK_READ_TRACE=1` and capture stderr locally; no uploading or
  reading live credentials is required. Remove misleading claims that the saved
  reproduction patch still applies after the permanent tests have landed.
- [ ] **Step 6: Commit** as `test: verify DM unread convergence and trace read decisions`.

## Self-review and handoff

Coverage checked: parsing/presence (Task 1); incoming evidence, boundedness,
dispatch/retry/join semantics and local intent (Task 2); all snapshot sources and
page/batch context (Task 3); both signs of ambiguous marked events and eventual
convergence (Task 4); UI/banner/thread behavior, diagnostics and full verification
(Task 5). Each Review Focus item has an owning test step above.

All new interfaces are private; later tasks consume the names defined in earlier
Interfaces blocks. No implementation was started while writing this plan.
Execution method remains to be chosen. Recommend **Native** execution because
Tasks 2–4 share private request/state interfaces and should evolve sequentially
in one context, followed by an independent whole-change review.

## Engineering review amendments

The user approved keeping the full scope, then authorized applying all review
recommendations. This review concerns the plan, not an implemented fix.

| Finding | Priority / confidence | Evidence in reviewed revision `d869fee` | Applied change |
|---|---|---|---|
| A1: count-domain confirmation was unspecified | P1 / 8 of 10 | Plan lines 78–100 preserve raw totals, while lines 254–267 use display events followed by a raw-count fixture | Task 1 contract table, independent raw/display fields, disagreement tests, and Task 4 same-domain confirmation |
| C1: read publication before metadata merge permits reentrant intermediate state | P1 / 9 of 10 | Plan line 209 said to apply read changes before metadata merge; `Store::updateConversation` emits immediately and `Store::emit` invokes observers synchronously | Non-emitting preparation, freshness-checked bookkeeping commit, one coherent Store publication per existing entry, and cross-entry observer tests |
| T1: positive count decreases were not pinned | P2 / 9 of 10 | Task 2 tested zero clears and missing counts, not a current exact count declining from five to two while latest remains ahead | Task 2 exact-count/mention decrease, disjoint-arrival, overlap and lower-bound assertions |
| P1: automated reconciliation had no workspace-wide concurrency bound | P2 / 9 of 10 | Plan reused `reconcileConversationReadState`; current implementation dispatches on Normal and coalesces only per conversation | Background/quiet automatic reconciliation, one in flight per workspace, fair deduplicated waiting work, 20-DM burst and steady-state evidence-pruning tests |

### What already exists

- `Read::send`, paced/held queues, retry policy and `join`: extend these with
  observation context; do not build a second HTTP scheduler.
- `_readEpoch`, `_readIntent`, serialized `Write::mark`: retain local Read/Mark
  unread ordering; the new evidence clock solves a different race.
- `Read::inject`: reuse its delivery eligibility and duplicate checks.
- Existing `applyMarkedEvent` and reconciliation queue: extend their conflict
  rules and pacing, not their public API.
- `Store::mergeConversation` and synchronous observers: reuse publication APIs
  with prepare/commit ordering; avoid adding a public transaction API.
- Existing fake server, C++ tests, headless notification probes and base logger:
  reuse them for regression evidence and opt-in diagnostics.

### Data flow and failure coverage

```text
HTTP queue -> dispatch stamp -> response parser -> raw/display observation
                 | retry recaptures context        |
WS marked -> missing/ambiguous/conflicting? -> paced confirmation
                 |                                  |
                 +-------------------------+--------+
                                           v
incoming delivery -> eligible/deduplicated evidence -> prepare decision
local Read/Mark unread -> newer intent guards ------> | (no Store emit)
                                           v
                    freshness recheck -> reject/defer -> coalesced refresh
                                           |
                                         accept
                                           v
                      commit bookkeeping -> one coherent Store publication
                                           v
                          sidebar/workspace badge; no message replay
```

Add a short inline version of the prepare/commit/publish invariant near the
implementation in `slack_backend.cpp` and the actual-dispatch context capture.

| Production failure | Handling required by the plan | Owning regression / user effect |
|---|---|---|
| Raw/display semantics differ or count absent | No cross-domain substitution; obtain comparable evidence or report limitation | Task 1 + Task 4 disagreement tests; transient conservative badge possible |
| Reply/message arrives during a delayed response | Protect eligible post-dispatch evidence, preserve local intent | Task 2 races; new unread remains visible |
| Observer changes another entry while roster publishes | Per-entry fresh preparation and guarded commit | Task 3 cross-entry reentrancy; no stale overwrite |
| Retry, joined request, reverse completion or stale patch | Actual-attempt immutable context and a coalesced fresh follow-up | Tasks 2–3 queue/order tests; eventual convergence |
| Conflicting event, rejected mark, failed confirmation | Retain safe state, existing write barriers and paced retry | Task 4 intent/failure tests; no false success or banner replay |
| Evidence overflow or long-lived unread backlog | Invalidate unsafe evidence; absorb/prune successful baselines | Task 2 overflow + Task 4 steady-state tests; bounded memory |
| Many DMs reconcile at once | Background/quiet, one in flight per workspace, fair draining | Task 4 multi-DM burst; foreground work keeps priority |
| Backend destroyed during response | Existing lifetime cancellation retained throughout adapters | Task 2 teardown; no stale callback access |

No unhandled silent failure was identified in the revised plan; actual protocol
semantics remain an explicit implementation characterization gate, not a claim
that the live bug is solved.

### NOT in scope

- A replacement read-state service, generic HTTP scheduler, or public Store
  transaction API: existing mechanisms suffice with the added invariants.
- Repairing the unrelated Grid token-routing baseline failure: report it,
  but do not broaden this fix to authentication.
- New badge styling, changes to mute/thread policy, distribution or release:
  this work corrects the existing synchronization behavior.

### Implementation Tasks from this review

- [ ] **R1 (P1)** — Task 1/4: characterize count domains and test disagreement end to end; verify parsing and event test groups.
- [ ] **R2 (P1)** — Task 2/3: prepare without emission, commit current bookkeeping, publish once; verify synchronous and cross-entry observer tests.
- [ ] **R3 (P2)** — Task 2: pin five-to-two exact-count decreases and overlapping newer arrivals; verify polling tests.
- [ ] **R4 (P2)** — Task 2/4: cap workspace reconciliation and retire absorbed evidence; verify 20-DM fairness and steady-state tests.

All four actions are integrated into the main tasks above; these checkboxes are
review traceability, not additional implementation workstreams. No new deferred
TODO is needed. Sequential implementation, no parallelization opportunity.

## GSTACK REVIEW REPORT

| Review | Runs | Status | Findings |
|---|---:|---|---|
| Engineering plan review | 1 | CLEAR for implementation planning | 4 findings applied: architecture 1, code quality 1, tests 1, performance 1 |
| Independent outside voice | 0 | Skipped | No independent second review performed in this pass |

Scope retained as approved. Data-flow/failure coverage recorded above. Four test
coverage additions are assigned to their owning tasks. No implementation or
runtime tests were performed in this documentation review; the earlier baseline
and its unrelated Grid failure remain as recorded in the investigation.

**VERDICT:** ENG PLAN REVIEW CLEAR — implement in task order, with Task 1's
count-semantics characterization gate satisfied before cross-domain behavior
is changed. This does not certify production correctness or resolve the
uncaptured live incident.

NO UNRESOLVED DECISIONS
