# Search Jump and Thread Reply Resolution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix same-conversation message jumps (`conv == _current`) and resolve missing `thread_ts` for thread replies returned by search so they open in the thread panel instead of only switching to the DM.

**Architecture:** Reuse the existing `MessageList::jumpTo` / `checkEdges` paging loop for same-DM jumps, reuse `Backend::loadMessage` (`conversations.replies` with `limit=1`) as an on-miss fallback when a main-timeline jump cannot find `ts`, and check `_store.findMessage` in `SlackBackend::search` for locally cached replies. No new `Backend` virtual methods are needed.

**Tech Stack:** C++20, Catch2-style test macros (`TEST`, `CHECK`, `REQUIRE`), CMake/Ninja, `ctest`.

---

### Task 1: Fix `MessageList::jumpTo` to schedule edge checks and notify on main-timeline miss

**Files:**
- Modify: `src/app/screens/messages/message_list.h`
- Modify: `src/app/screens/messages/message_list.cpp`
- Test: `tests/app/screens/messages/test_messages.cpp`

- [ ] **Step 1: Write failing regression tests in `tests/app/screens/messages/test_messages.cpp`**
  - Add a test `"anchoring: jumpTo — older unloaded message in already-open scrollable DM"` where a DM has enough messages so `firstVisible() > 3`, `hasMoreBefore = true`, `showConversation(dm)` has already settled, and `e.list->jumpTo(olderTs)` triggers `loadHistory` and flashes `olderTs`.
  - Add a test `"anchoring: jumpTo — main-timeline miss invokes onJumpMiss"` verifying `onJumpMiss(conv, ts)` is called both when `ts` is within the loaded range (`!jumpNeedsOlder()`) and when `hasMoreBefore` is exhausted / paged past `ts` without finding it.

- [ ] **Step 2: Run the new tests to verify they fail**
  - Run: `cmake --build build --target msga_app_tests -j8 && ./build/tests/app/msga_app_tests test_messages anchoring`

- [ ] **Step 3: Implement `MessageList::jumpTo` and `onJumpMiss`**
  - In `src/app/screens/messages/message_list.h`: add `std::function<void(ConvRef, Ts)> onJumpMiss;` on `MessageList`.
  - In `src/app/screens/messages/message_list.cpp`:
    - In `MessageList::jumpTo(Ts ts)` (around line 3067):
      - When `!threadMode()` and `jumpNeedsOlder()` is `true`, call `scheduleEdgeCheck();` so `checkEdges()` starts paging older history even when `conv == _current`.
      - When `!threadMode()` and `!jumpNeedsOlder()`, clear `_jumpTs = 0;` and invoke `if (onJumpMiss) onJumpMiss(_conv, ts);`.
    - In `MessageList::checkEdges()` (around line 2126), where `!applyJump() && !jumpNeedsOlder()` clears `_jumpTs = 0`:
      - Capture `const Ts missed = std::exchange(_jumpTs, 0);` and invoke `if (missed && onJumpMiss) onJumpMiss(conv, missed);` (note: also handle the case where `c.hasMoreBefore` became `false` on the loaded page).

- [ ] **Step 4: Run `messages_*` tests to verify pass**
  - Run: `ctest --test-dir build --output-on-failure -L messages`

---

### Task 2: Check local store in `SlackBackend::search` when `thread_ts` is omitted

**Files:**
- Modify: `src/app/slack/slack_actions.cpp`
- Test: `tests/app/core/test_slack_write.cpp`

- [ ] **Step 1: Write failing test in `tests/app/core/test_slack_write.cpp`**
  - In or next to the `search.messages` test (around line 515), add a case where `search.messages` returns a match with neither `m["thread_ts"]` nor `?thread_ts=` in `permalink`, while `_store` already has that message with `threadTs != 0`, and assert `hits[...].thread == rootTs`.

- [ ] **Step 2: Run the test to verify it fails**
  - Run: `cmake --build build --target msga_core_tests -j8` (or the `test_slack_write` test target) and run the `search.messages` test.

- [ ] **Step 3: Update `SlackBackend::search` in `src/app/slack/slack_actions.cpp`**
  - After checking `m["thread_ts"]` and `permalink` (around line 1910), if `!h.thread`:
    ```cpp
    if (const model::Message *cached = _store.findMessage(h.conv, h.ts);
        cached && cached->threadTs && cached->threadTs != h.ts)
        h.thread = cached->threadTs;
    ```

- [ ] **Step 4: Run core/slack tests to verify pass**
  - Run: `ctest --test-dir build --output-on-failure -R "slack"`

---

### Task 3: Wire `Shell::jumpToMessage` overlay cleanup and `loadMessage` thread fallback

**Files:**
- Modify: `src/app/screens/shell/shell.h`
- Modify: `src/app/screens/shell/shell.cpp`
- Test: `tests/app/screens/shell/test_shell.cpp`

- [ ] **Step 1: Write failing tests in `tests/app/screens/shell/test_shell.cpp`**
  - Test `Shell::jumpToMessage(dm, replyTs, 0)` when `replyTs` is a thread reply not present in the main DM list: `onJumpMiss` triggers `_ctx.backend.loadMessage(dm, replyTs, ...)`, which returns `m` with `m.threadTs = rootTs`, opening the thread panel (`h.sh->threadOpen()`) and jumping to `replyTs`.
  - Test `Shell::jumpToMessage(dm, ts, 0)` and `Shell::jumpToMessage(dm, replyTs, rootTs)` when `conv == _current` and an overview page (`Saved`/`Activity`) or `Canvas` is open: overlays are dismissed and the message area (and thread panel when `thread != 0`) is visible.
  - Test stale-callback guard: if the user switches conversation (`_current != conv`) or workspace before `loadMessage` completes, the fallback does not open the old thread.

- [ ] **Step 2: Run `shell_tests` to verify the new tests fail**
  - Run: `cmake --build build --target msga_app_tests -j8 && ctest --test-dir build --output-on-failure -R "shell_tests"`

- [ ] **Step 3: Update `Shell` in `src/app/screens/shell/shell.h` and `src/app/screens/shell/shell.cpp`**
  - In `Shell::jumpToMessage(ConvRef conv, Ts ts, Ts thread)` (`src/app/screens/shell/shell.cpp:2446-2467`):
    - When `conv == _current`, call `leaveTeammate()`, `leavePages()`, `showCanvas(false)`, and `showMessageArea()` before jumping or calling `openThread(conv, thread)`.
    - Track a generation/target (`_pendingJumpConv`, `_pendingJumpTs` or a `uint64_t _jumpGen`) incremented on `open(conv)` / `jumpToMessage(...)` / `leaveWorkspace()` so an in-flight `loadMessage` fallback cannot fire after navigation changes.
  - Where `_messages` (`screens::MessageList`) is constructed in `Shell` (under `#ifdef MSGA_HAVE_MESSAGES`), wire `list->onJumpMiss`:
    - Guard against re-entry for the same `(conv, ts)` (e.g., only resolve when `(conv, ts)` matches the active jump or hasn't already been probed).
    - Call `_ctx.backend.loadMessage(conv, ts, [this, alive, gen, conv, ts](bool ok, model::Message m) { ... })`.
    - Inside the callback: verify `!alive.expired()`, `gen == _jumpGen`, `_current == conv`, `ok`, and `m.threadTs && m.threadTs != ts`; then call `openThread(conv, m.threadTs)` and `p->list().jumpTo(ts)`.

- [ ] **Step 4: Run all affected suites and verify**
  - Run: `ctest --test-dir build --output-on-failure -R "messages|shell|unified_search|slack"`
