# Unified search bar (Slack-style) — design

Date: 2026-11-06 · Branch: `feat/unified-search` · Status: draft for approval

## Intent and why

msga splits "find something" across two hidden shortcuts: **⌘F** (an overlay that
searches messages only, newest 20) and **⌘K** (a quick switcher over conversations).
There is no visible search bar, and people outside your DMs can't be found at all.
Slack users reach for one wide **"Search <workspace>"** bar at the top of the window
that finds places, people and messages from a single box. This change gives msga
that bar, so someone coming from Slack finds everything where they expect it,
without learning two shortcuts.

## Goals

1. A **wide search bar in the title bar**, as in Slack: "Search <workspace name>",
   always visible, between the traffic lights (macOS) / window edge and the right
   end of the title bar.
2. Clicking it, **⌘F** or **⌘K** opens **one search panel** anchored under the bar.
3. The panel, with an **empty query**, lists **Recent places**: recent conversations
   (same order the quick switcher uses today), plus msga's own pages (see Decided).
4. While **typing**, results appear in groups, in this order:
   - **People & DMs** — workspace users and DMs/group DMs whose names match
     (instant, from the loaded store; includes people you have never DMed).
   - **Channels** — conversations you are a member of whose names match (instant).
   - **Messages** — Slack message search for the query (async), the first few hits.
   - A final row **"Search all messages for "<query>""** that opens the full message
     results (today's ⌘F result list, reused).
5. Slack search modifiers (`in:`, `from:`, `has:`, `before:`, `after:`, quotes, `-word`)
   keep working: the query is passed to Slack unchanged, as today.
6. Keyboard: ↑/↓ move across all groups, **Enter** opens the selected row, **Esc**
   closes; the first row is selected by default (Slack's "Enter" hint on it).
7. The **conversation name moves out of the title bar** into a **header row** at the
   top of the message pane (name + topic left; members and actions right), the way
   Slack does it and the way msga already lays out its header on Linux/Windows.

## Non-goals (first version)

- Searching **files**, workflows, canvases or apps.
- **AI search**, saved searches, search history, filter chips/UI.
- Channels you are **not** a member of (Slack's directory); joining from search.
- Changing what the message search returns beyond showing it in the panel.

## Anti-goals (would be a failure even if it works)

- Typing feels laggy: the People/Channels groups must update on every keystroke
  without waiting on the network; only the Messages group is async.
- A network request per keystroke: message search runs debounced (or on a pause),
  and a stale response never overwrites newer results.
- Losing what exists: everything ⌘K and ⌘F do today must still be reachable
  (multi-workspace tabs in the switcher, full message results, Slack modifiers).
- Breaking window dragging: empty title-bar space (outside the bar) still drags the
  window on macOS; the traffic lights stay clear (see the image-viewer fix on
  `fix/image-viewer-titlebar`).

## Constraints

- C++20, msga's own UI toolkit (`src/ui`), no new dependencies.
- macOS unified title bar (`Shell::buildTitleBar`, `TitleStrip`, `ConvHeader` under
  `#ifdef __APPLE__`); Linux/Windows draw their own title bar with window controls.
- Multi-workspace: search applies to the **open workspace** (as the bar's label
  says); the existing switcher's per-workspace tabs keep working for ⌘K-style jumps.
- All new UI strings go through `tr()` and the Japanese catalog
  (`src/tools/i18n.py update`; the `i18n_up_to_date` test enforces it).
- Formatting by the repo's clang-format; tests in `tests/app/screens/shell/`.

## Decided

- **Placement A** (chosen in the mockups): the bar fills the title bar like Slack;
  the conversation header becomes its own row in the message pane on macOS too
  (reuse the non-Apple `ConvHeader` layout path instead of the centred unified
  header).
- **One panel for ⌘F and ⌘K.** Both shortcuts open the same panel; their entries in
  the shortcuts table stay (renamed "Search" / "Jump to a conversation" both
  opening it) so user key overrides keep working.
- **Reuse, don't rewrite:** recent order and name matching from
  `quickSwitchOrder` / `quickSwitchFilter` / `quickSwitchNames` (fuzzy_match), message
  results from `Backend::search` and the existing `MessageSearch` result list, opening
  a DM with a new person through `Context::messageUser`.
- **Same bar on all platforms** (my call): Linux/Windows get the bar in their own
  title strip too, left of the window controls, so the layout stays one design.

- **Several workspaces** (my call): the panel keeps the switcher's workspace tabs
  (shown only when more than one workspace runs). Every group — People, Channels and
  Messages — follows the selected tab's workspace (its own Store and Backend), and
  typing still re-aims the tab at the workspace with the best name match, as ⌘K does
  today. ⌘F, ⌘K and a click all open the same panel in the same mode, on the open
  workspace's tab.
- **Full message results live in the panel** (my call): "Search all messages" grows
  the panel into a scrollable list of the full results (today's 20 hits, same rows),
  under the same title-bar field; Esc or clearing the query returns to the grouped
  view. The old ⌘F overlay and its separate field are removed, and the header's
  search button focuses the bar. This works on every page (Threads, Saved, Activity…),
  not only where a message area exists. One `search.messages` call per settled query
  feeds both the grouped view and the full list.
- **Recent places** (my call): recent conversations plus the pages msga actually has
  (Threads, Saved, Activity, Scheduled), not Huddles, which msga has no page for.

## Left to the builder

- Exact sizes, debounce interval, number of rows per group (suggest ≤ 5 people,
  ≤ 5 channels, ≤ 3 messages before "Search all messages").
- How the panel is implemented (new popup vs. evolving `QuickSwitcher`), as long as
  existing switcher behaviour (workspace tabs) survives.
- Whether the header row keeps msga's centred-title look anywhere (e.g. no
  conversation open: show the workspace name in the bar's placeholder only).
- Tests: at minimum, grouping/order of results for a query, empty-query recent
  places, stale async results ignored, ⌘F/⌘K both open the panel, Enter on a person
  without a DM calls `messageUser`, header row present on macOS.

## Scope of the build (owner's call)

- **Personal copy only, not for upstream.** The centred macOS unified header is
  replaced outright: no setting to keep the old look. On macOS `ConvHeader` uses the
  same header-row path as Linux/Windows, and the code that exists only for the
  centred title (the `__APPLE__` header layout, the workspace-name fallback label,
  and moving the header for full screen in `syncFullScreenChrome`) is deleted rather
  than kept behind a flag. One branch, `feat/unified-search`, built locally.
