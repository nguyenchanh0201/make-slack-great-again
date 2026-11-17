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
- `struct Observation { std::optional<model::Ts> lastRead, latest; Count unread, mentions; Source source; };`
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
  CHECK(obs.unread.value.has_value());
  CHECK(*obs.unread.value == 0);
  CHECK(mappedConversation.unread == 0);
  // input omits both unread_count and any applicable authoritative flag
  CHECK(!missing.unread.value.has_value());
  // input {unread_count:4}
  CHECK(positive.unread.value == 4);
  // negative, overflow beyond UINT32_MAX, string/object/null => unknown, not wraparound
  CHECK(!malformed.unread.value.has_value());
  // mentions omitted versus explicitly zero remain distinguishable too
  CHECK(!absentMention.mentions.value.has_value());
  CHECK(zeroMention.mentions.value == 0);
  ```

  Also pin `client.counts` fixtures: DM `dm_count` is an exact count;
  `has_unreads=true` without an exact count is an inexact lower bound of one;
  `has_unreads=false` is zero; absent fields remain unknown. Conflicting fields
  retain positive evidence and trigger later reconciliation, rather than
  treating a contradictory zero as proof of read. Characterize existing
  raw/display fixture differences before choosing their precedence.
- [ ] **Step 2: Build and run the new group; record the expected failures.**
  Run `cmake --build build --target msga_app_tests -j 6`, then
  `PLAT_BACKEND=headless build/tests/app/msga_app_tests test_slack_read 'dm zero parsing'`.
  Missing new symbols initially fail to compile; after declaring interfaces,
  verify actual zero/missing assertions fail against the old mapper behavior.
- [ ] **Step 3: Implement the parsing interfaces and remove timestamp-only unread inference from `toConversation`.**
  Preserve existing positive model projections. Use optional observation metadata
  at merge boundaries in subsequent tasks so a missing model projection of zero
  is never treated as an authoritative zero. Do not put presence flags in the
  persisted Conversation type. Preserve `toCounts` movement detection behavior.
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
- [ ] **Step 4: Add ordering/lifecycle cases to the same test group and make them pass.**
  Force a request to wait behind a 429, act locally while queued, and verify
  context is captured at dispatch. Repeat with a transport retry and a new
  incoming message before retry dispatch. Verify joined calls share context;
  reverse-completing requests converge after at most one outstanding follow-up;
  overflow at entry 257 does not erase unread state and eventually recovers.
  Destroy the backend during a delayed response and check no callback touches it.
  Use fake-server request counters/pumpUntil, not fixed sleeps as proof of order.
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
- `Read::carryLocal(model::Conversation &fresh, const model::Conversation &old, const readstate::Observation *obs = nullptr, const readstate::DispatchContext *context = nullptr) const -> void`. Metadata-only callers without observation/context preserve existing read fields; they must not reinterpret numeric defaults as server read information. Apply accepted read changes through `applyReadObservation`, then carry the accepted fields into the metadata merge.

- [ ] **Step 1: Add group `dm zero sources` in both read/realtime suites.**
  Repeat zero/missing/own-newer cases through roster reload, enrichment, and
  reconnect patches. For a delayed batched patch, perform local Mark unread
  after HTTP completion but before patch application: assert it survives.
  Load two roster pages with different dispatch contexts and assert a later
  page cannot lend its freshness to an older page's read observation.
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
- [ ] **Step 4: Test repeated conflicts, failures, and recovery.**
  A burst of 20 conflicting events coalesces to one in-flight reconciliation
  plus at most one necessary queued refresh. Script an initial failure, then
  success; safe counts persist until success and converge afterward. Check
  no immediate request loop and preserve serial mark writes and thread reads.
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
