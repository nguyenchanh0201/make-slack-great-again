# Search Jump and Thread Reply Resolution

## Intent

Ensure that jumping to a message from search (`UnifiedSearch`) or any `jumpToMessage` caller:
1. Works reliably when the user is **already viewing the target DM/channel** (`conv == _current`), including when the target message is older than currently loaded history or when an overview page/canvas is covering the message area.
2. Opens the **conversation thread** (`openThread` + jump inside `ThreadPanel`) when the target message is a thread reply, even if Slack's `search.messages` response omits `thread_ts` from both the match object and `permalink`.

## Observed Behavior

1. **Same-conversation jump (`conv == _current`) fails for unloaded older messages (reproduced in `msga_app_tests`):**
   - `Shell::jumpToMessage(conv, ts, thread)` (`src/app/screens/shell/shell.cpp`) skips `open(conv)` when `conv == _current` and calls `MessageList::jumpTo(ts)` directly.
   - `MessageList::jumpTo(Ts ts)` (`src/app/screens/messages/message_list.cpp`) sets `_jumpTs = ts`, calls `applyJump()`, and checks `jumpNeedsOlder()`. It never calls `scheduleEdgeCheck()`.
   - When `conv != _current`, `open(conv)` calls `MessageList::showConversation(conv)`, which calls `scheduleEdgeCheck()`, causing `checkEdges()` to page older history until `_jumpTs` is found or passed.
   - When `conv == _current` and the list is scrolled below the top (`firstVisible() > 3`), `checkEdges()` is idle. `jumpTo(ts)` leaves `_jumpTs` set without scheduling `checkEdges()`, so `loadHistory` never runs and the list never moves.
   - A temporary regression test in `tests/app/screens/messages/test_messages.cpp` (scrollable DM with `hasMoreBefore = true`, `firstVisible() > 3`, calling `jumpTo` to an older unloaded `ts` without leaving the DM) reproduced this: `historyLoads` did not increment and `flashing(ts)` remained `false`.

2. **Same-conversation jump (`conv == _current`) with overlays:**
   - When `conv == _current` and `thread == 0`, `Shell::jumpToMessage` calls `showMessageArea()` without `leavePages()` or `showCanvas(false)`.
   - When `conv == _current` and `thread != 0`, `Shell::jumpToMessage` calls `openThread(conv, thread)` without `showMessageArea()`, `leavePages()`, or `showCanvas(false)`.

3. **Thread reply from search moves to the DM without opening the thread:**
   - `SlackBackend::search` (`src/app/slack/slack_actions.cpp`) only populates `SearchHit::thread` from `m["thread_ts"]` or `?thread_ts=...` in `m["permalink"]`.
   - When Slack's `search.messages` match omits `thread_ts` in both places, `h.thread` is `0`.
   - `Shell::jumpToMessage(conv, ts, 0)` then opens the DM and calls `MessageList::jumpTo(ts)` on the main DM timeline (`_root == 0`), which only contains top-level messages.
   - If `ts` falls within or after loaded messages (`ts >= c.messages.front().ts`), `jumpNeedsOlder()` is `false` and `jumpTo` immediately clears `_jumpTs = 0` and does nothing. If `ts < c.messages.front().ts`, it pages through DM history looking for a reply that only exists in a thread.

## Approaches Considered

1. **Local store + `scheduleEdgeCheck()` only:**
   - Smallest change, but fails whenever a thread reply from `search.messages` is not yet cached in `_store` and Slack omits `thread_ts`.
2. **Targeted jump + on-miss `conversations.replies` fallback (selected):**
   - Fix `MessageList::jumpTo` to call `scheduleEdgeCheck()` when `jumpNeedsOlder()` is true.
   - Fix `Shell::jumpToMessage` to clear pages/canvas/teammate overlays consistently when `conv == _current`.
   - Check `_store.findMessage(h.conv, h.ts)` in `SlackBackend::search` when `h.thread == 0`.
   - Add an on-miss thread-resolution fallback when a main-timeline jump cannot find `ts` in the loaded range (or after history is exhausted), querying `conversations.replies` for `(conv, ts)` to discover `thread_ts` and opening the thread if `thread_ts != 0 && thread_ts != ts`.
3. **Eagerly query `conversations.replies` inside `SlackBackend::search` for every hit with `h.thread == 0`:**
   - Unnecessary API fan-out (up to 20 extra requests per search query) and rate-limit risk for top-level messages.

## Required Behavior

- **Same-conversation jump to unloaded older message:** Calling `jumpTo(ts)` on an already-open conversation (`conv == _current`) must schedule `checkEdges()` whenever `jumpNeedsOlder()` is true, paging older history until `ts` is found, `ts` is passed, or `hasMoreBefore` becomes false.
- **Same-conversation jump with overlays:** Calling `Shell::jumpToMessage(conv, ts, thread)` when `conv == _current` must dismiss any open overview page (`Activity`, `Threads`, `Saved`, `Scheduled`), `TeammatePage`, or `Canvas` and ensure the message area is visible.
- **Cached thread reply in search:** In `SlackBackend::search`, if `m["thread_ts"]` and `permalink` do not yield a `thread`, check `_store.findMessage(h.conv, h.ts)` and use `cached->threadTs` if present and `!= h.ts`.
- **Uncached thread reply with `thread == 0`:** When `MessageList::jumpTo(ts)` in main-timeline mode (`_root == 0`) determines `ts` is absent from the main timeline (either immediately because `!jumpNeedsOlder()`, or after `loadHistory` pages past `ts` / exhausts `hasMoreBefore`), it must invoke a miss callback rather than silently dropping the jump.
- **Thread fallback resolution:** On a main-timeline jump miss for `(conv, ts)`, the shell/backend queries `conversations.replies` (`channel`, `ts`, `limit=1`) to check whether `ts` belongs to a thread.
  - If a message with `ts` is returned and its `thread_ts` is non-zero and `!= ts`, open that thread (`openThread(conv, thread_ts)`) and jump to `ts` inside the thread panel (`p->list().jumpTo(ts)`).
  - If `ts` is not a thread reply (deleted or `thread_ts == ts` / absent), do nothing further (no infinite retry loop).
- **Lifecycle safety:** Async fallback callbacks must be guarded against workspace switch, detach, conversation change (`conv != _current`), or a newer user jump/open superseding the missed jump.

## Constraints and Exclusions

- Existing C++20 native application; preserve `Backend` public contract where possible (or add a narrow virtual method with a default no-op implementation so fake/other backends are unaffected).
- Do not make extra Slack API calls for normal top-level message jumps that succeed in `MessageList`.
- Avoid redundant `conversations.replies` calls for the same missed `(conv, ts)` if a jump is already resolving it.

## Validation

- Add a regression test in `tests/app/screens/messages/test_messages.cpp` covering `jumpTo` to an older unloaded message while already in a scrollable DM (`firstVisible() > 3`).
- Add regression tests in `tests/app/core/test_slack_write.cpp` and `tests/app/screens/shell/test_shell.cpp` covering:
  - `SlackBackend::search` resolving `thread` from local `_store` when `search.messages` omits `thread_ts`.
  - `Shell::jumpToMessage(conv, replyTs, 0)` falling back to `conversations.replies` on main-timeline miss, opening the thread panel, and jumping to `replyTs`.
  - `Shell::jumpToMessage(conv, ts, thread)` when `conv == _current` with an overview page or canvas open.
- Run affected test suites (`messages_*`, `shell_*`, `unified_search_*`, `slack_*`) via `ctest`.
