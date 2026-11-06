# Live event delivery for session workspaces — design

Date: 2026-11-06 · Branch: `feat/live-events` (from `personal`) · Personal build · Status: draft for approval

## Intent and why

In **session mode** (xoxc token + `d` cookie — the only mode this user runs) msga learns about
new messages by **polling**: the open chat every 5 s, `client.counts` every 10 s, thread
replies every 20 s, roster reloads, plus background sweeps. A tracing session caught Slack
**rate-limiting `conversations.history` with a 60 s cooldown**; overlapping fetches and retries
that queue during the cooldown and fire together make it recur. While throttled, nothing new
arrives for up to a minute — the "messages sometimes come late" the user sees.

msga already holds an **RTM WebSocket** in session mode (`rtm_presence.*`), opened only so
Slack shows the user online. Slack pushes **every conversation's events** down that socket;
msga reads only `hello`/`pong`/`error` and drops the rest. slk (a terminal Slack client the user
compared against) is fast because it consumes such a socket. This change makes msga **listen
instead of ask**: socket events update the Store immediately, and `conversations.history`
is called only when there's no other way to know.

## Goals

1. **Events from the RTM socket go into the Store** in session mode: new messages (channel,
   DM, group DM, thread replies), edits, deletions, reactions added/removed, and the other
   event types the Socket Mode path already applies — through the **existing**
   `SlackBackend::Live::apply` path, not a second implementation.
2. **A session workspace with a live socket counts as push** (`hasRealtimePush()` true while
   the RTM link is connected), so the existing push behaviour applies: no roster/counts/threads
   polling, `realtimeMissed()` re-establishes the socket.
3. **The open chat's poll becomes a slow safety net while live**: every 60 s (as non-session
   workspaces already do), not every 5 s. When the socket is down, today's cadence returns.
4. **One catch-up after a (re)connect**: on every RTM `hello` after a gap (sleep, network
   change, reconnect) run the existing reconnect backfill (`resyncUnreads` / `onReconnected`
   equivalent) once, so nothing missed during the gap is lost.
5. **No retry bursts**: when Slack answers `ratelimited` for a method, msga waits the
   `Retry-After` it was told, then sends **one** request for it — duplicate pending fetches for
   the same conversation are coalesced, and retries never fire together.
6. **The socket stays up while a session workspace runs**, independent of the presence
   setting. The presence modes keep controlling only what they control today (tickles /
   auto-away behaviour); "presence off" must not turn live delivery off.

## Non-goals

- Rewriting the HTTP layer, switching to a Slack library, or changing app-keys (Socket Mode)
  workspaces.
- Startup/boot changes (`client.userBoot`-style one-call boot) — the user's pain is delivery;
  revisit separately.
- Importing the session from the Slack desktop app (slk's Keychain/cookie-DB import).
- Typing indicators, huddles, or other new UI fed by socket events (events msga has no Store
  model for are ignored, as today).

## Anti-goals

- **No impersonation** of Slack's official client: no copied protocol fingerprints, no
  `_x_mode`-style parameters, no faking client identity or headers to pass Slack's checks.
  msga keeps identifying as itself. (slk does this; msga will not.)
- No **more** requests than today in any steady state; while live, far fewer.
- No lost or duplicated messages: an event and a later poll/backfill for the same `ts` must
  converge (the Store already de-duplicates by `ts`; keep relying on it, don't bypass it).
- Never apply another workspace's events to this one (keep `Live::onPayload`'s "is it ours"
  rule semantics for RTM frames).

## Constraints

- C++20, msga's own code (`src/app/slack/`), no new dependencies; repo clang-format;
  tests in `tests/app/core/` (fake HTTP/WS servers already used by `test_slack_*`).
- `rtm.connect` works only for session tokens; the wss handshake must send the `d` cookie
  (already done in `rtm_presence`).
- RTM frames are bare event objects (`{"type":"message",…}`); Socket Mode wraps them in an
  envelope (`payload.event`). The adapter maps one onto the other; `Live::apply(ev, ours)`
  stays the single place that turns events into Store changes.
- Slack pongs RTM pings; the existing two-missed-pongs liveness rule decides "socket down".

## Decided

- Reuse `Live::apply` for RTM events (my call): one event-to-Store implementation for both
  socket kinds.
- Session + live socket ⇒ push semantics via `hasRealtimePush()` (my call): reuses the
  already-tested app-keys polling policy instead of inventing a new one.
- Open-chat safety poll 60 s while live (my call): the existing non-session value.
- Live delivery is on by default for session workspaces, no setting (my call: personal build;
  falls back to polling automatically whenever the socket is down).
- **Background sweep while live** (my call): the rotating `pollConversation(bg, false)` sweep
  (today every 2 min, `Read::tick`) slows to every **10 min** while the socket is live, and a
  message it finds does **not** call `realtimeMissed()` (no `rtm.connect` storm); only the
  open-chat safety poll's misses re-establish the socket. Socket down ⇒ today's 2 min returns.
- **Retry scope** (my call): goal 5 covers **reads only** (`Read::issue`); writes through
  `issueApi` (send, edit, react…) are never merged or dropped. For a throttled read method,
  identical pending calls (same method + form) are merged into one, and when the cooldown
  ends the queued calls for that method go out **one at a time, ~1 s apart**, not all at once.

- **Presence trade-off (owner's call): accepted.** Holding the socket makes Slack show the
  user active while msga runs; the owner is fine with that in exchange for live delivery. So:
  a session workspace holds the RTM socket whenever it runs, in every presence mode. The modes
  now control only the tickles: WhileRunning keeps its unconditional tickles; WhileUsing
  forwards input tickles and, after its idle time, stops tickling (it no longer drops the
  socket); Native (today's "off") sends no tickles at all. The footer's presence state/tooltip
  must describe this truthfully (the socket being up no longer means "presence on").

## Left to the builder

- How the RTM link exposes frames (a sink/callback beside `onStateChanged`) and how it relates
  to `Live` (a second socket kind in `Live`, or an adapter that calls `Live::apply`).
- Which RTM event types need mapping beyond what `Live::apply` handles; what "ours" means for
  an RTM frame (it's the workspace's own socket, so likely always ours, but confirm Slack
  Connect/shared channels).
- The coalescing/backoff mechanics in `web_api` / `pollConversation` for goal 5.
- Tests (at minimum): an RTM `message` frame appears in the Store without any
  `conversations.history` call; edit/delete/reaction frames apply; while connected, no
  `client.counts`/threads/roster polls and the open chat polls at 60 s; socket drop → 5 s
  polling resumes; reconnect `hello` → one backfill; a `ratelimited` reply → one retry after
  `Retry-After`, no burst; presence "off" keeps the socket for delivery.
