# Older Thread Search Results Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Selecting a search hit inside an older thread opens the channel or DM, displays the parent and replies, and highlights the selected reply.

**Architecture:** Keep the parent returned by the thread request alongside its replies in the Store. The thread panel can use that copy when the parent is outside the loaded channel history. Keep a jump pending through the initial load and apply it after the panel has rebuilt its rows.

**Tech Stack:** C++20, existing MSGA Store and Slack backend, fake Slack server, headless CTest suites.

**Spec:** This draft extends [Search Jump and Thread Reply Resolution](../specs/2026-11-18-search-jump-and-thread-resolution-design.md) using the live evidence below.

## Global Constraints

- Preserve the existing Backend interface and cache format.
- Keep fetched parents outside the main timeline when they fall outside loaded history. Preserve the history pagination boundary, latest activity and unread counts.
- Keep the existing search routing and obtain the parent from the existing thread request.

## Review Focus

1. An uncached channel parent is returned alongside the selected reply. Both render after the asynchronous load (Tasks 1–2).
2. A parent appears later in ordinary channel history. The thread displays the timeline version once, including subsequent updates (Task 1).
3. Two threads in one conversation have different parents. Each panel uses its own parent and replies (Task 1).
4. A user switches threads before the first request completes. The old request cannot steal the newer search jump (Task 2).
5. A failed request or unavailable parent terminates the pending jump. If Slack supplies valid replies without the parent, the replies remain visible (Task 2).

---

## Evidence and design decision

The Nov 19 search result from a private channel had its thread ID in the permalink. A live click in the installed build opened the thread panel and left it on **“No messages yet.”** A read-only request using the same parent timestamp and limit as MSGA returned the March 23 parent and all 36 replies, including the selected one. The channel cache held 50 messages going back to Oct 13.

`SlackBackend::Read::loadThreadPages` already receives the parent. Its `Store::updateMessage` call returns false when the parent is absent. `MessageList::rebuild` requires a parent to render the reply rows. The earlier search fix in `65fe183` addresses resolving the thread and reaching the panel.

The proposed Store change keeps the existing timeline message authoritative when present and gives the thread its own fallback parent. An isolated insert into the timeline could move the oldest loaded timestamp across an unfilled gap. Rendering the replies alone would preserve navigation but would lose the parent that Slack already supplied.

### Task 1 — Preserve the fetched parent with the thread

**Files:** Modify `src/app/model/types.h`, `src/app/model/store.h`, `src/app/model/store.cpp`, and `src/app/slack/slack_backend.cpp`. Extend `tests/app/core/test_store.cpp` and `tests/app/core/test_slack_read.cpp`.

**Interfaces:** Add `void Store::setThreadRoot(ConvRef conv, Ts root, Message message)` and `const Message *Store::threadRoot(ConvRef conv, Ts root) const`. Use an optional parent snapshot on the existing `Thread` record, scoped by conversation and root timestamp. The accessor prefers the root in the main timeline.

- [ ] **Write the failing backend regression.** Extend the test named `slack read: an open thread whose root left the head page asks for its root alone`. Seed and load only the newer channel page first. Use parent `1700000200.000200`, reply `1700000300.000000`, and loaded timeline message `1700009000.000000`. The fake replies request returns the parent and reply. After the request, assert that the parent is accessible to the thread, the reply is in the thread, and the timeline still contains only `1700009000.000000` with the same pagination boundary, activity timestamp and unread counts. Before adding the accessor, confirm the failed parent lookup through the existing Store.
- [ ] **Run the baseline.** Build `msga_app_tests` and run `ctest --test-dir build -R '^core_slack_read$' --output-on-failure`. Expect the added parent assertion to fail with a successful reply load.
- [ ] **Implement the Store and backend changes.** Validate that the fetched parent matches the requested root. Preserve the existing metadata refresh for a parent held in the timeline. Otherwise resolve its author and store it in the thread before publishing the replies. Emit a thread-scoped Update when the fallback parent changes so existing observers can redraw. Keep the fallback out of generic message lookup and main history operations. Drop a fallback on a known parent deletion and let conversation reset or removal discard the thread record. Failed loads preserve existing data.
- [ ] **Test Store behavior.** Cover two parent snapshots in one conversation, the same root timestamp in two conversations, an ordinary history load that makes the timeline parent authoritative, and clearing a known deleted parent. Run `core_store` and `core_slack_read`.
- [ ] **Commit** as `fix: retain fetched Slack thread parents outside channel history`.

### Task 2 — Display the whole thread and land on the search result

**Files:** Modify `src/app/screens/messages/message_list.cpp` and `src/app/screens/messages/message_list.h` as needed. Extend `tests/app/screens/messages/test_messages.cpp` and `tests/app/screens/shell/test_shell.cpp`. Use the existing search callback in `src/app/screens/shell/unified_search.cpp` and `Shell::jumpToMessage`.

**Interfaces:** Keep `MessageList::showThread(ConvRef conv, Ts root)` and `MessageList::jumpTo(Ts ts)`. Use `Store::threadRoot` for the parent in both row construction and row lookup.

- [ ] **Write a failing delayed-load test.** Open the synthetic result above from another conversation through the shell search route. Hold thread completion until the panel has opened and queued its jump. Release a fetched parent and reply through the Store path. Assert that the main view is on the correct conversation, the thread list displays both messages, and `flashing(replyTs)` becomes true after the rows are built. Use the search fixture with `SearchHit::thread` filled, since that is the shape the installed app receives after parsing the permalink.
- [ ] **Run the baseline.** Build `msga_app_tests` and run `messages_thread` and `shell_tests` through CTest. Expect the new assertion for visible rows or the pending jump to fail before the UI changes.
- [ ] **Implement rendering and jump completion.** Build the run from the resolved parent when available plus the replies. Resolve the root row through the same accessor used to build it. Keep the jump queued during the initial thread load. On a matching successful completion, rebuild the list and call `applyJump()` after rows are ready. Clear the jump when the load ends without the target or fails. Use a per-load generation alongside the existing conversation, thread, and lifetime guards so reopening the same thread cannot let an old completion clear a new jump. Preserve the existing loading and retry behavior.
- [ ] **Verify boundary cases.** Test switching threads while a load is pending, a failed load, and valid replies with an unavailable parent. Verify that a known parent renders once, an already-loaded reply still jumps immediately, and observer updates can redraw the parent row. Run `messages_thread`, `messages_loading`, `shell_tests` and `unified_search_tests`.
- [ ] **Commit** as `fix: show older search threads and highlight matching replies`.

### Final verification

- [ ] Build and run the targeted suites together.

  ```sh
  cmake --build build --target msga_app_tests -j 6
  ctest --test-dir build -R '^(core_store|core_slack_read|messages_thread|messages_loading|shell_tests|unified_search_tests)$' --output-on-failure -j 2
  git diff --check
  ```

- [ ] Build the app bundle. When testing the updated app, verify the running executable comes from that build and repeat the search-result click from another conversation with the older timeline still unloaded. Success is the parent and replies appearing with the matching reply in view. Repeat with a recent thread and a top-level search result to check both navigation paths.
