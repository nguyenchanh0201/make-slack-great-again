# DM Read State Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Use superpowers:subagent-driven-development only if the executing session explicitly authorizes delegation.

**Goal:** Stop already-read DMs from producing new banners or returning as unread after delayed events and reconnects.

**Architecture:** Retain the existing Store, shared delivery path, and serialized mark-write queue. Add notification read checks and narrowly scoped reconciliation of local actions with remote cursor/count observations. Use generations for asynchronous request races; establish the marked-event ordering contract before handling backward events.

**Tech Stack:** C++20, CMake/Ninja, existing C++ test harness, Python fake Slack server, headless platform notification probes.

**Spec:** `docs/superpowers/specs/2026-11-17-dm-read-state-design.md`

## Global Constraints

- Existing C++20/native application; no new dependency or public API break.
- No broad Slack backend refactor, platform notification rewrite, release, deployment, or push.
- Do not change the 30-day notification age policy as a substitute for read-state correctness.
- Main-conversation and thread cursors remain independent.
- Already-posted OS notification removal is outside scope; cancellation of not-yet-posted notifications is in scope.
- Preserve intentional local and remote Mark unread; message timestamp order alone does not identify stale events.
- Never derive total unread counts from incomplete message history.

## Review Focus

1. Read followed by Mark unread, and the reverse, with earlier echoes arriving late: Task 2.
2. Stale positive counts and stale zero counts racing newer local messages: Task 3.
3. Reading during name/avatar resolution and workspace detach during the wait: Task 1.
4. Legitimate remote backward read changes without documented event ordering: Task 2.
5. Unloaded thread replies and partially loaded DM histories: Tasks 1 and 3.

## File responsibilities

| File | Responsibility |
|---|---|
| `src/app/screens/shell/shell_notify.cpp` | Initial and deferred notification eligibility |
| `src/app/screens/shell/shell.h` | Private notification callback/helper declarations if needed |
| `src/app/slack/slack_actions.cpp` | Local read intent and existing mark queue completion |
| `src/app/slack/slack_backend.cpp` | Private read-state reconciliation, counts/roster/enrichment application |
| `src/app/slack/slack_backend.h` | Minimal private bridge declarations between implementation halves |
| `src/app/slack/slack_realtime.cpp` | Marked events and reconnect observations |
| `tests/app/screens/shell/test_shell.cpp` | Notification regressions using `TwoWorkspaces` |
| `tests/app/core/test_slack_realtime.cpp` | Out-of-order events and reconnect regressions |
| `tests/app/core/test_slack_read.cpp` | Polling, snapshot, roster and partial-history regressions |
| `tests/app/core/test_slack_write.cpp` | Serialized local read/unread actions and failures |
| `tests/support/fake_slack.py` | Extend deterministic delay/event scripting only if existing controls cannot express a regression |

Line numbers from the investigation are navigation hints only. Prefer codebase-memory MCP for discovery; fall back to text search for insufficient graph results and test/config literals. Do not edit every listed file automatically.

## Task 0: Establish the baseline and protocol contract

**Interfaces:** Preserve `SlackBackend::markRead(ConvRef, Ts)`, `markUnread(ConvRef, Ts)`, `markThreadRead(ConvRef, Ts, Ts)` and the existing `Write::mark` serialization contract. No new cross-task interface is required; private helpers may be selected to fit the existing implementation.

- [ ] Read the spec, applicable AGENTS instructions, and current git status. Preserve unrelated work. Use the provided worktree or create an isolated worktree under the executing session's rules.
- [ ] Trace `markRead`/`markUnread` through `Write::mark`, `Read::inject`, `Live::apply`, `carryLocal`, `applyActivity`, `applyPatches`, and `enrichDmActivity`. Confirm actual thread-read-floor access and cache restore behavior.
- [ ] Inspect marked-event fixtures and characterize which ordering/correlation fields are actually available. If consulting Slack API documentation, use Context7 first; distinguish documented fields from observed internal session payloads. Record evidence in an implementation note. Do not treat `ts` as event order.
- [ ] Configure a separate test build, avoiding user credential files in logs:

```sh
cmake -S . -B build-dm-read-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMSGA_BUILD_TESTS=ON
cmake --build build-dm-read-tests --target msga_app_tests --parallel 4
ctest --test-dir build-dm-read-tests -R '^(core_store|core_slack_read|core_slack_write|core_slack_realtime|shell_tests)$' --output-on-failure
```

Expected: all baseline tests pass. Verify the fake server starts and tests execute rather than returning early through `haveServer()`. Record pre-existing failures separately.

## Task 1: Suppress already-read notifications at delivery time

**Files:** shell notification implementation/header; `test_shell.cpp`.

**Interfaces:** Keep `Shell::maybeNotify(Store &, const std::string &, ConvRef, const Message &, bool)`. If changing private `postNotification`, preserve all huddle/reminder callers and make message-specific eligibility explicit. Resolve the originating workspace by stable identity before dereferencing a Store after an async wait.

- [ ] Add failing headless tests with these assertions:
  - `notifications: a delayed already-read DM stays silent`: set the DM cursor to T, deliver an absent top-level message at T, and assert `lastNotification` does not advance and badges remain zero.
  - A message at T+1 still notifies under the same settings.
  - Read while a sender is unresolved, switch to another conversation, then finish name resolution: no banner.
  - Read while an avatar is pending, switch away, then finish decoding: no banner and no separately played sound.
  - Detach/remove the originating workspace during the avatar wait: no banner and no invalid access; switching workspaces alone still permits a genuinely unread banner from the originating workspace.
  - Reading the main DM does not suppress an unread reply solely by conversation cursor; a reply with a known thread floor covering it stays silent.
- [ ] Run `shell_tests` and confirm the new read/defer tests fail on unmodified production code.
- [ ] Check top-level read eligibility in `maybeNotify`; revalidate message eligibility immediately before OS posting after avatar completion. Reuse the existing mute/focus/settings rules without recursively scheduling another avatar wait. Keep reminders and huddles on their existing path.
- [ ] Run `shell_tests`; confirm new assertions and existing notification, sound, reminder, huddle and workspace tests pass.
- [ ] Review the diff and commit only this task's files if commits are authorized in the executing session.

## Task 2: Preserve the newest local read intent across event races

**Files:** Slack action/backend/realtime implementation and minimal private header declarations; `test_slack_write.cpp`, `test_slack_realtime.cpp`.

**Interfaces:** Existing public read methods remain unchanged. Track a per-conversation local intent generation and desired cursor, including backward Mark unread. Integrate with the current serialized/coalescing write queue; keep request completion and realtime echo distinct. Private helper names and storage layout are implementer choices.

- [ ] Add failing fake-server regressions for these ordered sequences:
  - Read A, Read B, then an older marked event for A: keep B and the correct counts.
  - Read B, Mark unread A, then the earlier read acknowledgement/echo for B: keep A-1 and the unread badge.
  - Mark unread A, Read B, then the earlier unread acknowledgement/echo: keep B with no stale badge.
  - Retry/failure of an earlier write followed by a queued newer action: preserve write order and newest local intent; completion alone must not be mistaken for an authoritative echo.
  - No local pending intent: an authoritative remote read clears the badge; a legitimate remote Mark unread can lower the cursor without replaying stored-message banners.
  - Repeat on DMs/group DMs and verify state is isolated between conversations/workspaces.
- [ ] Run `core_slack_write` and `core_slack_realtime`; observe failures for stale updates before fixing.
- [ ] Apply the protocol contract established in Task 0. Protect newer local generations from older acknowledgements. For backward events that cannot be ordered/correlated, schedule/coalesce fresh authoritative reconciliation with a captured generation. A new local action invalidates that response. Do not use an arbitrary delay, `max(lastRead, ts)` everywhere, or an indefinitely retained set of timestamps as the ordering mechanism.
- [ ] Define bounded pending-state cleanup for success, failure, coalescing and backend teardown. Verify remote Mark unread after a completed local read remains possible. If the protocol cannot distinguish a required case, record the exact evidence and the unresolved tradeoff before claiming completion.
- [ ] Run both suites; all existing serialization and new race tests must pass. Review and commit this task's changes if authorized.

## Task 3: Reconcile cursor/count snapshots consistently

**Files:** `slack_backend.cpp`, `slack_realtime.cpp`, minimal private declarations if needed; `test_slack_read.cpp`, `test_slack_realtime.cpp`.

**Interfaces:** Reuse Task 2's local intent/generation policy. Preserve `mergeConversation`, `Read::carryLocal`, `Read::applyActivity`, `Live::applyPatches` and `Read::enrichDmActivity` responsibilities, routing cursor/count decisions through one consistent private policy rather than independent maxima.

- [ ] Add failing regressions:
  - A reconnect/info response reports cursor 100, latest 300, unread 2; local Read to 300 occurs before its application: final cursor 300, unread/mentions zero.
  - Repeat through roster carry, activity/count first-seen seeding, and DM enrichment where those paths apply read state.
  - A snapshot reports zero through T; an unread message T+1 arrives while it is in flight: T+1 remains unread and notifies once.
  - Genuine unread backlog outside the loaded history page retains its count; do not replace server totals with an incomplete recount.
  - A stale snapshot cannot undo local Mark unread, including a request queued before the action but dispatched afterward: capture the relevant generation at actual dispatch.
  - Fresh remote read through the current latest converges to zero; normal unread messages above the accepted cursor remain counted.
- [ ] Run read/realtime suites and verify the stale-positive-count regressions fail before production changes.
- [ ] Merge each snapshot's cursor, latest and counts as a related observation. Reject stale generations; prevent positive top-level counts entirely covered by the accepted cursor; retain newer local deliveries. Keep conversation and thread accounting separate and respect incomplete histories.
- [ ] Run `core_store`, `core_slack_read`, `core_slack_write`, `core_slack_realtime`, and `shell_tests`; review and commit this task's changes if authorized.

## Task 4: Final verification and handoff

- [ ] Build `msga_app_tests` again and run the focused suite command from Task 0.
- [ ] Run `ctest --test-dir build-dm-read-tests --output-on-failure --parallel 4`. Confirm fake-server tests executed and report unrelated failures accurately.
- [ ] Review the complete diff against the spec: no unread replay on manual Mark unread; no global cursor maximum; no erased newer messages; independent thread floors; safe deferred workspace access; bounded reconciliation state.
- [ ] Run `git diff --check`. Report changed files, red/green evidence, exact test results, and protocol/platform validation limits. Do not claim the original live incident was reproduced unless it actually was.
- [ ] Leave implementation local for review. Do not push, create a PR, merge, or deploy under this handoff.
