# Live Ha Vang DM badge investigation

**Retry result:** read-only live Slack responses and an original-code replay now
confirm an endpoint-scope/reconciliation failure for this target. See the
confirmed retry section below; the initial access limitations were resolved.
The historical callback which first created the badge was not recorded.

Captured 2026-11-17 while the user reported a Dock badge of 1 and a sidebar
badge of 1 for Ha Vang. No DM was opened, marked read/unread, or modified; the
running app was not restarted. Concurrent production/test edits in this
worktree were left untouched.

## Captured state

- Running PID: 35589, `/Applications/msga.app/Contents/MacOS/msga`.
- Process launch: 2026-11-17 15:12:18, local UTC+07.
- Executable modification: 15:11:52, local UTC+07.
- Installed executable SHA-256 matches the existing worktree app executable:
  `7b28c0ad4c3c3214a92c61465d7e131cbdadfcbe4f9bb53e73d5e8228a3adb1d`.
- Target conversation cache: `D0BNXTRMBS7`; peer `U0BCU1ECFLJ`, resolved to Ha Vang.
- Persisted unread=1, mentions=1, member=true, muted=false, default notify level.
- Persisted main-conversation read cursor: `1794909941811299`, or
  **2026-11-17 17:05:41.811299 +07:00**.
- Persisted latest activity: `1794910537557059`, or
  **2026-11-17 17:15:37.557059 +07:00**.
- That latest activity exactly equals a cached root's `latestReply` value.
  The root is `1794909869919749` (**17:04:29.919749 +07:00**), with three replies.
- The 50 cached messages contain no top-level message after the main read cursor.
  The root has `threadTs == ts`, which the model correctly treats as a root,
  not a reply. Full reply history is not present in the conversation cache.
- A second sample retained the same unread/mention/read/latest values.

Only target metadata was extracted. No message bodies or credentials were
written to the diagnostic evidence. A reduced JSON snapshot is at
`/tmp/msga-live-ha-vang-evidence.json` (mode 0600).

## Source trace and interpretation

Use the original `8a5c2ef` source when comparing with this build; the worktree
currently contains newer uncommitted implementation changes.

1. `ConvRow::refresh` (`sidebar.cpp:648`) uses `cv.unread` for a Slack DM's count.
2. `workspaceAttention` (`sidebar.cpp:1855`) adds that same DM unread count to
   important attention, then `Shell::updateAttention` forwards the aggregate to
   the native Dock badge. Two badges therefore do not imply two separate bugs.
3. `Read::inject` (`slack_backend.cpp:3622–3677`) compares eligible message
   timestamps with the conversation read cursor and may count DM thread replies;
   it also advances conversation `latest` for replies.
4. `applyAuthoritativeReadObservation` (`slack_backend.cpp:3842`) starts from old
   counts and retains them on an incoming zero when `max(current.latest,
   observed.latest) > acceptedLastRead`.
5. `reconcileCounts` (`slack_backend.cpp:3806`) also uses combined latest/read
   comparisons when deciding whether stale counts can be cleared.

The captured timestamps make **thread activity interacting with stale DM-count
reconciliation** the leading explanation: latest activity is a thread reply,
while the main read cursor is earlier and the persisted DM badge remains one.
The existing zero-count merge can preserve that one if Slack supplies a zero.
This is a source-supported hypothesis tied to real target state, not a recovered
trace proving which callback originally set the count. The cache is partial and
cannot by itself prove that every thread reply was read, that the count was
created by inject rather than a snapshot, or what Slack currently reports.

## Limits encountered

- The app's stdin/stdout/stderr descriptors all point to `/dev/null`; no runtime
  read-state log was available to recover callback/event ordering.
- The Orca CLI was unavailable. Native CUA selection of the installed app timed
  out while obtaining its accessibility state; it was not used to navigate the DM.
- A read-only `conversations.info` probe initially failed Python TLS certificate
  validation. A public Slack connectivity check through system curl succeeded,
  but subsequent Keychain credential lookups timed out. No authenticated server
  response was captured. TLS verification was not disabled.
- The newer worktree changes are not in the currently running installed binary.

## Regression and next capture

Extend the approved zero-count regression to use this real metadata shape:
main cursor at 17:05:41, root at 17:04:29, latest reply at 17:15:37, DM count one.
Exercise main-conversation and thread-read observations independently, then a
fresh count-domain-compatible zero. The main DM must not retain a false count
merely because its latest activity came from the thread; genuinely unread thread
state must retain the intended independent notification behavior.

Before calling the live cause confirmed, obtain a read-only server snapshot
with raw/display counts and the relevant thread read cursor, and correlate it
with read-state decision logs from an instrumented run. Confirm with the user
whether the replies were read in MSGA or official Slack. Preserve this captured
state before any restart; do not relaunch a second client against the same cache
as a substitute for inspecting the running process.

## Confirmed retry: 2026-11-17 23:41–23:43 +07:00

The retry used macOS Security.framework with authentication UI disabled to read
the existing app credential in memory, then Foundation URLSession with normal
TLS verification. It succeeded without a credential prompt or a restart. Only
the existing Slack workspace was contacted, using read-only endpoints. Tokens
were not printed or written to the evidence files.

Live results:

| Source | Target result |
|---|---|
| `conversations.info` at 23:41:46 | `unread_count=1`, `unread_count_display=1`, `last_read=1794909941.811299`; latest is reply `1794910537.557059` in thread `1794909869.919749` |
| `client.counts` comparison starting 23:43:03 | IM entry present; `has_unreads=false`, `mention_count=0`, `last_read=1794909941.811299`, latest=`1794909869.919749`; `dm_count` absent |
| `conversations.replies` | Three replies; root `last_read=1794910537.557059` equals `latest_reply`; `has_more=false` |
| `subscriptions.thread.getView` | Target thread present; same read/latest-reply cursor; `unread_replies=[]` |

These responses agree that the main DM is read under `client.counts` and the
thread is read under its independent cursor. `conversations.info` nevertheless
reports one and includes the newer thread reply as latest. This establishes a
real endpoint discrepancy in this case; it does not prove universal semantics
for every Slack conversation or count field. The earlier raw-versus-display
hypothesis alone is insufficient here: both info counts are one.

### Original-code replay using those captured values

The complete `SlackBackend::reconcileCounts` function body was extracted unchanged
from `8a5c2ef` and compiled in a standalone C++ harness with a minimal Conversation
record. Inputs are the target cache and actual server responses, not invented
timestamps or a guessed zero `conversations.info` response.

```text
1. Current badge + live client.counts(read): 1 -> 1
2. Locally cleared badge + live conversations.info: 0 -> 1
3. Next live client.counts(read): 1 -> 1 (mentions=1)
Blocking comparison: thread-inclusive latest <= top-level latest is false
```

The function starts with existing counts. Its first clear branch fails because
the thread-inclusive current latest (17:15:37) is newer than the main read cursor
(17:05:41). Its counts-specific clear branch also fails: it requires
`cur.latest <= obsLatest`, comparing 17:15:37 with the top-level latest 17:04:29.
The positive-count branch does not run on the read `client.counts` entry, leaving
the previous one intact. Conversely, an info observation with the same main
cursor and unread one can restore a locally cleared count to one.

This confirms a mechanism for **clear -> restore -> refuse to clear** under the
live target's conflicting activity scopes. It is not a recording of the running
process's original callback order. The previous event cannot be recovered from
`/dev/null`, and neither cache polling nor this replay identifies which live
refresh first supplied the count.

Artifacts:

- `2026-11-17-live-dm-badge-evidence.json`: reduced local/server metadata.
- `2026-11-17-live-dm-badge-replay.cpp`: isolated replay of the original merge.

Run from the repository root:

```sh
clang++ -std=c++20 docs/investigations/2026-11-17-live-dm-badge-replay.cpp -o /tmp/msga-live-dm-replay
/tmp/msga-live-dm-replay
```

Exit **1 is expected** on the original function: the harness requires the final
count to be zero, and the captured read-state inputs leave it at one.

### Implication for the fix

The regression must include a read `client.counts` entry whose latest is the
thread root, a read thread with a later reply, and a contradictory positive
`conversations.info` result. Re-fetching `conversations.info` alone cannot resolve
this case; it will return the same one. Distinguish conversation activity scope
from thread activity and retain their separate read floors. Apply a comparable
main-conversation read observation without letting thread-inclusive latest
permanently veto it, while preserving genuinely newer incoming messages.

Production code, installed app, current DM selection and read markers were not
changed in this retry. An instrumented future run is still needed to record the
actual callback sequence if the user wants a historical-style runtime event log.


## Scoped fix validation & controlled live-run handoff (2026-11-18)

- **Implementation commits:** `e78bd07` (Task 1), `3ac995e` (Task 2), `bb73051` (Task 3), `7f6ed0d` (Task 4).
- **Synthetic validation (fake Slack + headless Shell/Dock):**
  - `core_slack_read`: PASSED (47 cases, 0 failed).
  - `core_slack_realtime`: PASSED (44 cases, 0 failed in affected-suite run).
  - `shell_tests`: PASSED.
  - `slack_dm_read_tests`: PASSED (new end-to-end `SlackBackend` -> `Store` -> `Shell` -> `TestHooks::badgeCount` suite).
  - `core_slack_write`: FAILED on pre-existing Grid token-routing case (`test_slack_write.cpp:1064–1078`).
  - Full CTest: 128/133 passed; failures are `core_slack_write` (pre-existing Grid), `text_ink_lean`, `legacy_tests`, `settings_tests` (pre-existing unrelated), and an intermittent timing race in `core_slack_realtime` under `-j 2`.
- **Live confirmation:** Pending user-coordinated switch (synthetic tests do not replace live confirmation).
- **Instrumented build for controlled live run:**
  - Binary path: `build/msga.app/Contents/MacOS/msga`
  - Build revision: `7f6ed0d7f3d05fdd41d50aa080b14d55343e70b8`
  - Binary SHA-256: `be8abdcf2405245e856ad868fe098d2d32d49ea7bcb6c88f51da602206c6e75e`
  - Suggested trace file: `~/Desktop/msga-read-trace.log`
  - Procedure: Quit the running `/Applications/msga.app` single-instance process first (do not run a second authenticated writer against the same cache), then launch:
    `MSGA_SLACK_READ_TRACE=1 ./build/msga.app/Contents/MacOS/msga 2>~/Desktop/msga-read-trace.log`
    and inspect `slack-read:` lines for `scope=main` vs `scope=unknown` decisions before and after refreshes.
