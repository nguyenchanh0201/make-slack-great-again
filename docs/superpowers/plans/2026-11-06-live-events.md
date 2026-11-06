# Live Event Delivery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In session workspaces, deliver Slack events from the RTM socket msga already holds straight into the Store, and cut polling (and the `conversations.history` rate limits it causes) to a slow safety net while that socket is live.

**Architecture:** `RtmPresence` keeps its socket up in every presence mode and forwards event frames and reconnect `hello`s through two new callbacks; presence modes now only gate tickles. `SlackBackend::Live` feeds those frames to its existing `apply()` (the Socket Mode path) and counts a connected RTM link as realtime push, so `Read::tick` uses the already-tested push policy plus slower open-chat/background polls. `Read::issue` merges identical throttled reads and releases them one at a time.

**Tech Stack:** C++20, msga `src/app/slack/`, msga test framework with the fake Slack server (`tests/support/fake_slack.py`, `/_ctl/ws/send` pushes a frame on the newest open WebSocket).

**Spec:** `docs/superpowers/specs/2026-11-06-live-events-design.md`

## Global Constraints

- Session (xoxc + `d` cookie) workspaces only; app-keys/Socket Mode behaviour unchanged.
- No impersonation of Slack's official client: no copied fingerprints, no `_x_mode`, no faked headers/client identity.
- `Live::apply(ev, ours)` stays the single place that turns events into Store changes.
- The socket is held whenever a session workspace runs, in every presence mode (owner accepted being shown active). Modes gate tickles only: WhileRunning unconditional tickles; WhileUsing input tickles, stops tickling after `idleMs` (socket stays); Native no tickles.
- While the RTM link is connected (push): no roster / `client.counts` / thread polls; open-chat safety poll **60 s**; background sweep **10 min** and its finds do not call `realtimeMissed()`. Socket down ⇒ today's cadences (open chat 5 s visible / 60 s hidden, background 2 min).
- One backfill (`Live::onReconnected()`) per `hello` that follows a gap; not on the very first connect.
- Rate limits: reads only. Identical pending reads (same method + form) merge into one; after a cooldown queued reads for that method are released one at a time, **1000 ms** apart. Writes (`issueApi` callers outside `Read`) are never merged or dropped.
- No new dependencies; repo clang-format clean; commits as `Chánh Nguyễn <chanhnguyen34@gmail.com>`, no AI trailer.
- Pre-existing failures to ignore: `messages_actions` (fails on master), `text_ink_lean` (Lato preference on this machine), flaky `core_slack_write` / `core_slack_features` channel-sections / `core_slack_realtime` single runs.

## Review Focus

1. An event for a conversation msga doesn't hold yet (a brand-new DM or a channel just joined) must still surface (Live's existing unknown-conversation handling), not be dropped. → Task 2 test `live rtm: a message in an unknown DM surfaces`.
2. The same message arriving by event and then by a poll/backfill must appear once. → Task 2 test `live rtm: event then poll does not duplicate`.
3. Laptop sleep: pongs stop, the link reconnects, exactly one backfill runs, polling ran at the fast cadence only while down. → Task 3 test `live poll: drop falls back to fast polling, hello backfills once`.
4. Frames before `hello` or non-event frames (`pong`, `reconnect_url`, `error`, `presence_change` for others) must not crash or be misapplied. → Task 2 test `live rtm: control frames are ignored`.
5. Presence "Native": the socket is up but the footer must not claim presence is on, and no tickle is ever sent. → Task 1 test `rtm presence: native holds the socket without tickles`.

---

### Task 1: RtmPresence holds the socket in every mode and forwards frames

**Files:**
- Modify: `src/app/slack/rtm_presence.h`, `src/app/slack/rtm_presence.cpp`
- Test: `tests/app/core/test_slack_realtime.cpp` (update the two presence-link tests at ~823 and ~846 to the new semantics; add the new cases)

**Interfaces:**
- Produces (public on `RtmPresence`):
  ```cpp
  std::function<void(const json::Value &frame)> onEvent;  // every frame except hello/pong/error/reconnect_url
  std::function<void(bool afterGap)>            onHello;  // afterGap = false on the first hello, true after any drop
  void start();        // begin holding (idempotent); Live calls it once at construction
  void reconnectNow(); // drop and re-establish (backoff respected); used by realtimeMissed()
  // existing: setMode, mode(), state(), noteActivity(), connected(), onStateChanged, Timing
  ```
- `state()` keeps meaning *presence*: Native ⇒ `Link::Off` even while the socket is up; WhileUsing after `idleMs` without input ⇒ `Link::Idle` (socket still up); otherwise Connecting/Active from the socket. `connected()` keeps meaning *socket*.

- [ ] **Step 1: Write the failing tests:**
  - `rtm presence: native holds the socket without tickles` — Env(session), `start()`, mode Native: `pumpUntil(rtm->connected())`; `presenceLink() == Link::Off`; after pumping 2 × `alwaysTickleMs` (test timing), `Log().frame("ws:rtm","tickle")` is false; ws-open carried cookie `d=xoxd-test`.
  - `rtm presence: while-using idles without dropping the socket` — WhileUsing, idle time elapses → `presenceLink() == Link::Idle`, `rtm->connected()` true, no tickle after idle; `noteActivity()` → a tickle and `Link::Active`.
  - `rtm presence: event frames reach onEvent, hello reports gaps` — set `onEvent`/`onHello` on `presenceLinkForTest()`; first hello → `onHello(false)`; push `{"type":"message","channel":"C1","ts":"1.0","text":"x"}` → `onEvent` saw it; push `{"type":"pong","reply_to":1}` → not forwarded; force a drop (stop answering pings as the existing reconnect test does) → next hello → `onHello(true)`.
  - Update the existing test at ~823: "Native drops it" becomes "Native keeps the socket, Link::Off, no tickles".
- [ ] **Step 2: Run** `./scripts/build.sh --test && ctest --test-dir build -R core_slack_realtime --output-on-failure` → new cases FAIL.
- [ ] **Step 3: Implement**: `holding()` no longer depends on mode once `start()` was called; `setMode` only changes tickle behaviour and the reported `state()`; `onIdle` stops tickles and sets `Link::Idle` instead of `teardown()`; `onText` dispatches control frames as today and forwards the rest to `onEvent`; track whether a hello followed a drop for `onHello(afterGap)`; `reconnectNow()` = teardown + `scheduleReconnect()`. Keep `_unavailable` (rtm.connect refused) semantics: no socket, `onHello` never fires.
- [ ] **Step 4: Run** the same → PASS.
- [ ] **Step 5: Commit** `git commit -am "rtm: hold the socket in every presence mode and forward its events"`

### Task 2: Live applies RTM events and counts the link as push

**Files:**
- Modify: `src/app/slack/slack_realtime.cpp` (Live constructor ~130, `hasRealtimePush` ~190, `realtimeMissed` ~218, new RTM frame handler beside `onPayload` ~233), `src/app/slack/slack_backend.h` only if a declaration is needed
- Test: `tests/app/core/test_slack_realtime.cpp`

**Interfaces:**
- Consumes: Task 1 `onEvent`, `onHello`, `start()`, `reconnectNow()`, `connected()`.
- Produces: `bool SlackBackend::hasRealtimePush() const` returns true when the Socket Mode socket is attached **or** (`_live->rtm && _live->rtm->connected()`). `realtimeMissed()` also re-establishes the RTM link (`reconnectNow()`), under the existing `kReestablishGapMs` throttle.

- [ ] **Step 1: Write the failing tests** (session Env, connected, link up; frames pushed with `push(...)` bare, no envelope):
  - `live rtm: a message frame reaches the Store without history calls` — note `count("conversations.history")`; push a `message` for C1 → `pumpUntil(findMessage)`; history count unchanged.
  - `live rtm: edits, deletions and reactions apply` — `message_changed`, `message_deleted`, `reaction_added`, `reaction_removed` frames (same shapes as the Socket Mode test at ~274) apply.
  - `live rtm: a message in an unknown DM surfaces` — DM `D9` not in the roster → after the frame the conversation exists in the Store with the message (whatever Live's unknown-conversation path does for Socket Mode).
  - `live rtm: event then poll does not duplicate` — push message ts T; then serve the same message from `conversations.history` and trigger the open-chat poll → the conversation holds exactly one message with ts T.
  - `live rtm: control frames are ignored` — push `{"type":"reconnect_url","url":"wss://x"}`, `{"type":"error","error":{"code":1}}`, `{"type":"presence_change","user":"UOTHER","presence":"away"}`, and a non-JSON-object frame → no crash, Store unchanged except what Live already does for `presence_change`.
  - `live rtm: hasRealtimePush follows the link` — false before hello, true after, false after the link drops.
- [ ] **Step 2: Run** `ctest --test-dir build -R core_slack_realtime --output-on-failure` → FAIL.
- [ ] **Step 3: Implement**: in the Live constructor wire `rtm->onEvent = [this](const json::Value &ev){ apply(ev, true); }` (the workspace's own socket: ours), `rtm->onHello = [this](bool gap){ if (gap) onReconnected(); }`, then `rtm->start()`; extend `hasRealtimePush()` and `realtimeMissed()` as above. Map any RTM-only shape `apply` doesn't accept (check `message` with `hidden`, `message_replied`) only if a test needs it.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `git commit -am "live: apply RTM events in session workspaces; a live link is push"`

### Task 3: Polling policy while live

**Files:**
- Modify: `src/app/slack/slack_backend.cpp` — constants (~48-64: add `kLiveOpenPollGapMs = 60'000`, `kLiveBackgroundPollGapMs = 10 * 60'000`), `Read::tick` (~2464-2540), the miss check in `pollConversation` (~2750)
- Test: `tests/app/core/test_slack_read.cpp`

**Interfaces:**
- Consumes: Task 2 `hasRealtimePush()`.

- [ ] **Step 1: Write the failing tests** (session Env with the test speed-up the read tests use):
  - `live poll: while live only the 60 s safety polls run` — link up; open C1 visible; pump a simulated 3 min → `client.counts` 0, thread polls 0, roster reloads 0, `conversations.history` for C1 ≈ 3 (one per 60 s, allow ±1), background sweep 0.
  - `live poll: drop falls back to fast polling, hello backfills once` — link up, then drop it (stop pongs); while down the open chat polls at 5 s and `client.counts` resumes; on the next hello exactly one backfill (`conversations.list` reload or `backfillOpen` history call, as `onReconnected` does), then the 60 s cadence returns.
  - `live poll: a background find does not reconnect the socket` — live; the 10-min background sweep finds a new message → no extra `rtm.connect` logged.
- [ ] **Step 2: Run** `ctest --test-dir build -R core_slack_read --output-on-failure` → FAIL.
- [ ] **Step 3: Implement**: in `Read::tick` choose `fgGap = push && session ? kLiveOpenPollGapMs : today's`, background gap `push ? kLiveBackgroundPollGapMs : kBackgroundPollGapMs`; in `pollConversation` call `realtimeMissed()` only when `foreground` (the open chat's safety poll).
- [ ] **Step 4: Run** → PASS; also `core_slack_realtime`.
- [ ] **Step 5: Commit** `git commit -am "read: slow safety polls while the live link is up"`

### Task 4: Throttled reads merge and release one at a time

**Files:**
- Modify: `src/app/slack/slack_backend.cpp` — `Read::issue` (~495-540) and the `Read` struct members (a per-method pending queue and release timer)
- Test: `tests/app/core/test_slack_read.cpp` (extend near "a rate-limited call waits out Retry-After and retries", ~797)

**Interfaces:**
- Consumes: nothing new. Produces: no API change; callers' `done` callbacks all still fire (a merged call's waiters all receive the one answer).

- [ ] **Step 1: Write the failing tests:**
  - `slack read: identical throttled reads merge` — first `conversations.history` C1 answers `ratelimited` retry_after 1; meanwhile request C1's history 3 more times → after the cooldown exactly **1** further `conversations.history` C1 call is logged and all 4 callers' `done` ran with that answer.
  - `slack read: a cooldown releases queued reads one at a time` — throttled; queue history for C1, C2, C3 → after the cooldown the three calls are logged in order with consecutive log timestamps ≥ 1000 ms apart (fake-server log times; scale by the test speed-up).
  - `slack read: writes are never merged` — two identical `chat.postMessage` sends during a `conversations.history` cooldown → both sent.
- [ ] **Step 2: Run** `ctest --test-dir build -R core_slack_read --output-on-failure` → FAIL.
- [ ] **Step 3: Implement** in `Read`: key `method + "\n" + form`; while `readyAt[method]` is in the future, a call with an already-pending key appends its `done` to that entry instead of queueing; when the cooldown ends, release the method's queue through one timer that issues the next entry every 1000 ms (scaled by `speed` like other timers); a `ratelimited` answer during release re-arms `readyAt` and keeps the rest queued.
- [ ] **Step 4: Run** → PASS; full suite once: `./scripts/build.sh --test`.
- [ ] **Step 5: Commit** `git commit -am "read: merge throttled reads and release them one at a time"`

### Task 5: Personal build

**Files:** none in the repo beyond merging.

- [ ] **Step 1: Merge** `feat/live-events` into `personal` (worktree `~/orca/workspaces/make-slack-great-again/personal`), build `./scripts/build.sh --test` → only the known failures.
- [ ] **Step 2: Release** `./scripts/release.sh`, re-sign (`codesign --force --deep -s -`), DMG via `hdiutil`, install over `/Applications/msga.app` (quit msga first), relaunch; push `personal` and `feat/live-events` to the `fork` remote.
- [ ] **Step 3: Check by hand**: a message from a colleague appears within ~1 s; the Dock badge and notification fire; after waking the laptop, missed messages appear.
