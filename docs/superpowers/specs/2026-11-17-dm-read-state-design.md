# DM read state and notification correctness

## Intent

Keep a DM read after the user reads it, and prevent delayed delivery from announcing an already-read message. Preserve intentional Mark unread and independent thread read state. The user selected the targeted synchronization approach on 2026-11-17 and requested an implementation plan and a prompt for another agent.

## Approaches

1. Notification guard only: small, but leaves false unread badges.
2. Targeted synchronization fix (selected): guard notification delivery and reconcile the existing read events, local actions, and snapshots together.
3. Central read-state manager: larger architectural refactor; unnecessary for this fix.

## Observed behavior

- `Shell::maybeNotify` does not compare a top-level message timestamp with the conversation read cursor. Name resolution retries re-enter this function; avatar completion posts without rechecking eligibility.
- `SlackBackend::Live::apply` assigns marked-event cursors and counts directly, allowing an older event to replace newer local intent.
- `Read::carryLocal` and `Live::applyPatches` independently maximize cursors and unread counts. This can combine a fully-read cursor with stale positive counts.
- Polling and realtime share `Read::inject`; missing from memory does not mean unread. Its existing duplicate check must remain.
- `Write::mark` already serializes writes per conversation and coalesces waiting cursors. Preserve this ordering.

These are source findings. An isolated C++ harness using the existing update bodies reproduced cursor rollback and stale-count restoration. The original user occurrence has not been captured from a running Slack session.

## Required behavior

- Suppress a top-level notification when `message.ts <= conversation.lastRead`, including after asynchronous name/avatar work.
- Preserve normal notifications for genuinely unread messages and existing mute, focus, age, own-message, and settings rules.
- An acknowledgement or snapshot predating a local action cannot undo that action, whether it is Read or Mark unread.
- A deliberate Mark unread moves the cursor backward and survives earlier acknowledgements. Mark unread alone does not replay desktop notifications for stored history.
- Reads on another client converge to this app. A deliberate remote Mark unread must remain possible; never globally replace every cursor assignment with `max`.
- A snapshot covering no messages newer than the accepted read cursor cannot restore positive top-level unread counts. Counts and their cursor must be evaluated as one observation.
- Do not erase evidence of newer unread messages merely because an older snapshot reports zero.
- Main-conversation and thread cursors remain independent. Do not suppress a thread reply solely because its timestamp is below the conversation cursor. Use a known thread read floor for reply suppression; do not assume an absent floor means read.
- Deferred notification callbacks must resolve the originating workspace safely, including detach/switch/removal during the wait. Reminders and huddle notifications retain their existing behavior.

## Reconciliation strategy

Keep the public backend API and existing per-conversation write queue. Add small private reconciliation state only where required. Track local action generations when requests are dispatched, not when they complete; capture a generation when a read-state request actually leaves the queue. A delayed response from an older generation cannot replace newer local intent.

Realtime marked events do not inherently carry the local generation. Characterize their actual payload and the existing fake-server contract before selecting event ordering or acknowledgement correlation. A message cursor (`ts`) is not an event sequence number. Do not invent a payload field or use a timeout as proof that an acknowledgement is current. For an ambiguous backward event, reconcile through a fresh read-state request with a captured local generation rather than blindly accepting or discarding the event. Check how acknowledgement echoes and write failures affect the barrier before retiring pending intent.

Use the same merge policy for roster reload, activity/count snapshots, reconnect patches, and DM enrichment. Carry an observation's cursor/count relationship through the merge. Preserve newer locally delivered messages. Avoid recounting all unread from a partially loaded history page. Keep any acknowledgement bookkeeping bounded and clean it up on workspace/backend lifecycle changes.

## Constraints and exclusions

- Existing C++20/native application; no new dependency or public API break.
- No broad Slack backend refactor, platform notification rewrite, release, deployment, or push.
- Do not change the 30-day notification age policy as a substitute for read-state correctness.
- Already-posted OS notification removal is outside scope; cancellation of not-yet-posted notifications is in scope.
- Missing protocol ordering information is a real design constraint. Report any remaining ambiguity with evidence instead of silently dropping remote Mark unread support.

## Validation

Use the existing headless shell notification probes and local fake Slack server. Reproduce the defects in regression tests before production changes, cover both local action orderings and remote updates, then run affected suites and the full available CTest suite. No live Slack credentials or user messages are needed. Report any unavailable platform or protocol validation explicitly.
