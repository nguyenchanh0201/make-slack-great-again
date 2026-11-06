# Unified Search Bar Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace msga's hidden ⌘F overlay and ⌘K switcher with one Slack-style "Search <workspace>" bar in the title bar that opens a grouped search panel (recent places, people & DMs, channels, messages), and move the conversation name into a header row.

**Architecture:** A pure, store-only model (`unified_search_model`) ranks people and channels per keystroke by reusing `quickSwitchOrder`/`fuzzyScore`. A new `UnifiedSearch` popup (replacing `QuickSwitcher` as the shell's panel, keeping its workspace tabs) renders the groups, runs `Backend::search` debounced for messages, and expands into the full result list. `TitleStrip` hosts a `SearchBar` on every platform; on macOS `ConvHeader` switches to the header-row layout the other platforms already use.

**Tech Stack:** C++20, msga's `src/ui` toolkit, CMake/Ninja, msga's test framework (`TEST`/`CHECK`/`REQUIRE`, `msga_app_tests`).

**Spec:** `docs/superpowers/specs/2026-11-06-unified-search-design.md`

## Global Constraints

- Personal build only: delete the centred macOS header code; no setting to keep it.
- No new dependencies; C++20; repo clang-format (`clang-format --dry-run --Werror` clean).
- Every user-visible string through `tr()`/`N_()`; run `python3 -I src/tools/i18n.py update` and fill `src/app/i18n/ja.po` (the `i18n_up_to_date` test enforces it).
- Typing never waits on the network: People/Channels recompute synchronously per keystroke; only Messages is async.
- No `search.messages` per keystroke: debounce **300 ms** after the last keystroke; a response for an older query is dropped.
- Row caps in the grouped view: **5** people, **5** channels, **3** messages, then the "Search all messages" row.
- ⌘F (`Id::SearchMessages`) and ⌘K (`Id::QuickSwitch`) both stay in the shortcuts table and both open the panel.
- Search scope = the selected workspace tab (its Store and Backend); with one workspace no tab strip shows.
- macOS: empty title-bar space still drags the window; the bar is a control; traffic lights stay clear.
- Commits as `Chánh Nguyễn <chanhnguyen34@gmail.com>`, no AI co-author trailer.
- Pre-existing failures to ignore: `core_slack_write`, `messages_actions` (fail on `master`), `core_slack_features` channel-sections case (flaky).

## Review Focus

1. A query that is only Slack modifiers (`from:@henry`, `in:#talk-ai`) — People/Channels show nothing odd, Messages still searches with the raw query. → Task 2 test `unified search: modifier-only query sends raw text`.
2. Fast typing: results for "he" arriving after "henry" was typed must not replace "henry"'s. → Task 2 test `unified search: a stale message response is dropped`.
3. Workspace switch while the panel is open (or a workspace stops) — the panel must not touch a gone Store/Backend. → Task 2 test `unified search: closing on workspace switch`.
4. Bots, deleted and stranger users, and yourself must not appear under People. → Task 1 test `unified model: people skip bots, deleted, strangers and me`.
5. Full screen on macOS: no title bar, but the search must still open (⌘F/⌘K) and the header row must show. → Task 4 test `header row: full screen keeps the header and ⌘F opens search`.

---

### Task 1: Search model (people, channels, recent places)

**Files:**
- Create: `src/app/screens/shell/unified_search_model.h`, `src/app/screens/shell/unified_search_model.cpp`
- Modify: `src/app/screens/shell/CMakeLists.txt` (add the .cpp beside `quick_switcher.cpp`)
- Test: `tests/app/screens/shell/test_unified_search.cpp`; register `unified_search` in the `foreach(s …)` list and add `msga_add_test(unified_search_tests COMMAND msga_app_tests test_unified_search TZ HOME ${_home} LABELS shell)` in `tests/app/screens/shell/CMakeLists.txt`

**Interfaces:**
- Consumes: `quickSwitchOrder(const Store&, visited)`, `quickSwitchNames`, `quickSwitchFilter` (quick_switcher.h); `fuzzyScore` (fuzzy_match.h); `Store::user(UserRef)`, `Store::userCount()`; `User{bot, deleted, stranger}`; `Sidebar::Nav`.
- Produces (namespace `shell`):
  ```cpp
  struct PersonHit { model::UserRef user; model::ConvRef dm = model::kNoConv; }; // dm: an existing DM, else kNoConv
  struct RecentPlace { model::ConvRef conv = model::kNoConv; Sidebar::Nav page = Sidebar::Nav::None; };
  std::vector<RecentPlace> recentPlaces(const model::Store &, const std::vector<model::ConvRef> &order, size_t max = 8);
  std::vector<PersonHit>   searchPeople(const model::Store &, std::string_view query, size_t max = 5);
  std::vector<model::ConvRef> searchChannels(const model::Store &, const std::vector<model::ConvRef> &order,
                                             const std::vector<QuickSwitchName> &names, std::string_view query, size_t max = 5);
  ```

- [ ] **Step 1: Write the failing tests** in `test_unified_search.cpp` on a `Store` filled the way `test_shell.cpp`'s quick-switcher tests fill it:
  - `unified model: people match display name, real name and handle, best first` — users "Henry Nguyen" (@henry), "Hanna Ry" (@hanna); `searchPeople(s, "henry")` → first is Henry; `searchPeople(s, "hen")` contains both, Henry first.
  - `unified model: people skip bots, deleted, strangers and me` — a bot "Henry Bot", a deleted "Henry Old", a stranger, and the signed-in user named "Henry Me" → `searchPeople(s, "henry")` returns only the active human non-self Henry.
  - `unified model: a person with a DM carries it` — Henry has DM `D1` → `hit.dm == D1`; Hanna has none → `kNoConv`.
  - `unified model: channels exclude DMs and keep fuzzy order` — `#talk-ai`, `#talk-backend`, DM with Henry → `searchChannels(…, "talk")` == {talk-ai, talk-backend} in `quickSwitchFilter` order, no DM.
  - `unified model: recent places lead with Threads, Saved, Activity, Scheduled then conversations` — empty visit stamps → first four entries are pages in that order, then `order`'s first 4 conversations (`max = 8`).
  - `unified model: empty or whitespace query finds nothing` — `searchPeople(s, "  ").empty()`, `searchChannels(…, "").empty()`.
- [ ] **Step 2: Run** `./scripts/build.sh --test` then `ctest --test-dir build -R unified_search_tests --output-on-failure` → FAIL (undefined symbols).
- [ ] **Step 3: Implement** the three functions in `unified_search_model.cpp`. People: score each eligible user with `fuzzyScore` over displayName, the real name and handle (max of the three, as quick_switcher.cpp does for DM alt names), sort best first (ties by display name), cap `max`; `dm` = the store's existing 1:1 DM with that user. Channels: `quickSwitchFilter` over `order`/`names`, drop DMs/group DMs, cap `max`. Recent places: the four `Sidebar::Nav` pages (Threads, Saved, Activity, Scheduled), then conversations from `order`, total ≤ `max`.
- [ ] **Step 4: Run** the same ctest → PASS.
- [ ] **Step 5: Commit** `git add src/app/screens/shell/unified_search_model.* src/app/screens/shell/CMakeLists.txt tests/app/screens/shell/test_unified_search.cpp tests/app/screens/shell/CMakeLists.txt && git commit -m "search: people, channels and recent places model"`

### Task 2: UnifiedSearch panel (groups, async messages, full results, tabs)

**Files:**
- Create: `src/app/screens/shell/unified_search.h`, `src/app/screens/shell/unified_search.cpp` (CMake as Task 1)
- Modify: `src/app/screens/shell/message_search.h/.cpp` — keep only `searchConvLabel` and `searchPreview`; delete class `MessageSearch`
- Test: `tests/app/screens/shell/test_unified_search.cpp`

**Interfaces:**
- Consumes: Task 1 functions; `QuickSwitchTab` (quick_switcher.h, reused as-is for tabs); `Backend::search`, `Backend::SearchHit`; `searchConvLabel`, `searchPreview`; `Avatars`.
- Produces:
  ```cpp
  class UnifiedSearch : public ui::Popup {
  public:
      enum class RowKind : uint8_t { Recent, Person, Channel, Message, AllMessages, FullResult, Status };
      struct Row { RowKind kind; std::string text; size_t index = 0; }; // index into the kind's own list
      UnifiedSearch(screens::Context &, Avatars &, std::vector<QuickSwitchTab> tabs, int openTab, std::function<model::Backend *(int tab)> backendFor);
      ui::TextEdit &field();
      const std::vector<Row> &rows() const;
      int  selected() const;            // 0 after every refresh (first row)
      void choose(int row);             // opens it, closes the panel (AllMessages expands instead)
      void showAllMessages();           // the full list for the current query
      bool showingAll() const;
      int  tab() const; void setTab(int); // as QuickSwitcher
      // Outlets (Shell wires them; `key` = workspace of the selected tab):
      std::function<void(const std::string &key, model::ConvRef)>              onOpenConv;
      std::function<void(Sidebar::Nav)>                                        onOpenPage;
      std::function<void(const std::string &key, model::UserRef)>              onMessageUser;
      std::function<void(const std::string &key, model::ConvRef, model::Ts)>  onOpenMessage;
      static constexpr double kDebounceMs = 300;
  };
  ```

- [ ] **Step 1: Write the failing tests** (FakeBackend's search hook returns canned hits, as `test_shortcuts.cpp:262` does for `MessageSearch`):
  - `unified search: empty field lists recent places` — rows are all `Recent`, first is Threads.
  - `unified search: typing groups people, channels, then messages` — type "talk"; before the debounce, rows have no `Message`; after pumping 300 ms and the fake reply, order of kinds is Person*, Channel*, Message (≤3), AllMessages last.
  - `unified search: modifier-only query sends raw text` — type `from:@henry`; the fake backend's recorded query is exactly `from:@henry`.
  - `unified search: a stale message response is dropped` — type "he", let its search start, type "henry"; deliver "he"'s reply after "henry"'s → rows show "henry"'s hits only.
  - `unified search: enter on a person without a DM messages them` — choose a Person row with `dm == kNoConv` → `onMessageUser` called with that user; with a DM → `onOpenConv(D1)`.
  - `unified search: all messages expands and Esc returns` — choose AllMessages → `showingAll()`, rows are `FullResult` (all 20 hits); an Escape key event → back to grouped rows, panel still open; second Escape closes.
  - `unified search: closing on workspace switch` — destroy the second tab's Store while it is selected → panel closes without touching it (no crash under ASan builds).
- [ ] **Step 2: Run** `ctest --test-dir build -R unified_search_tests --output-on-failure` → FAIL.
- [ ] **Step 3: Implement** `UnifiedSearch`: field + optional tab strip (move QuickSwitcher's strip code, incl. re-aiming the tab at the best name match while typing), a `ui::VirtualList` of `rows()` with group headers ("Recent", "People", "Channels", "Messages"), ↑/↓/Enter/Esc. Message search: on each keystroke bump a generation counter and restart a `kDebounceMs` timer (`tick`); when it fires, call `backendFor(tab())->search(query, …)` capturing the generation and an `alive` weak_ptr; ignore replies whose generation is old. Grouped view takes the first 3 hits; `showAllMessages()` shows all hits as `FullResult` rows (no second request). Delete `MessageSearch` and its `Card`; keep the two helpers.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `git commit -am "search: unified panel with grouped and full results" ` (after `git add` of the new files).

### Task 3: Search bar in the title strip and shell wiring

**Files:**
- Modify: `src/app/screens/shell/shell.h` (replace `MessageSearch *_search`, `messageSearch()`, `QuickSwitcher *_switcher` with `UnifiedSearch *_unified`; add `void openUnifiedSearch(); UnifiedSearch *unifiedSearch() const;`)
- Modify: `src/app/screens/shell/shell.cpp` — `buildTitleBar` (add the bar), `openSearch`/`showQuickSwitcher` (both → `openUnifiedSearch`), remove the `MessageSearch` setup at ~1669 and its `hideNow/reset` calls, `hitTest` (bar = Client)
- Modify: `src/app/screens/shell/header.cpp:350` (the header's search button calls `onSearch`, now the unified panel)
- Modify: `src/app/screens/shell/shortcuts.cpp` (labels: `Id::SearchMessages` → N_("Search"), `Id::QuickSwitch` stays "Jump to a conversation")
- Modify: `src/app/screens/shell/quick_switcher.h/.cpp` — delete class `QuickSwitcher` (its tab-strip code moved in Task 2); keep `quickSwitchOrder`, `quickSwitchFilter`, `quickSwitchNames`, `QuickSwitchName`, `QuickSwitchTab`. Port the `test_shell.cpp` "quick switcher: …" cases (443–530) that construct the popup to `UnifiedSearch`; leave the ones that call the free functions unchanged.
- Test: `tests/app/screens/shell/test_shell.cpp`, `tests/app/screens/shell/test_shortcuts.cpp` (rewrite the `messageSearch()` tests at 262/287 and `openSearch` tests at 3414–3497 against `unifiedSearch()`)

**Interfaces:**
- Consumes: `UnifiedSearch` (Task 2); `liveWorkspaces()`, `findRunning`, `onSwitchWorkspace`, `open(conv)`, `showPage(Page)`, `jumpToMessage(conv, ts, 0)`, `_ctx.messageUser`.
- Produces: `class SearchBar : public ui::Clickable` (in shell.cpp, anonymous namespace) showing "Search %1" with the workspace name (`tr("Search %1")`, "msga" when signed out); `Shell::openUnifiedSearch()` anchors the panel under the bar (`Popup::Place::Below`, the bar's window rect; centred 600 px wide when the title bar is hidden).

- [ ] **Step 1: Write the failing tests:**
  - `search bar: a click, ⌘F and ⌘K open the same panel` — each opens `unifiedSearch()` non-null with focus in its field; a second ⌘F while open keeps one panel.
  - `search bar: shows the workspace name` — bar text == "Search CODE LEAP" for a store named "CODE LEAP".
  - `search bar: a person row opens a DM in the right workspace` — two workspaces; choose a Person in tab 2 → `onSwitchWorkspace("w2")` then `messageUser` called on w2's context.
  - Updated `test_shortcuts.cpp` cases: ⌘F opens the unified panel (was `messageSearch()`), Esc closes it.
- [ ] **Step 2: Run** `ctest --test-dir build -R 'shell_tests|shortcut_tests' --output-on-failure` → FAIL.
- [ ] **Step 3: Implement** the bar (flex 1, height = title bar − 12, rounded, `C::TitleBarControl` text over a 55%-white fill like the mockup), place it in `buildTitleBar` on all platforms (macOS: after a 80 px spacer for the traffic lights; others: before the window-control buttons), wire `openUnifiedSearch` and the four outlets (switch workspace first when `key` ≠ open one, as `onChooseIn` does today). In `hitTest`, the bar is `H::Client`; empty strip stays `H::Caption`.
- [ ] **Step 4: Run** → PASS (only the known pre-existing failures elsewhere).
- [ ] **Step 5: Commit** `git commit -m "search: Slack-style bar in the title strip opens the unified panel"`

### Task 4: Conversation header row on macOS; delete the centred header

**Files:**
- Modify: `src/app/screens/shell/header.cpp` (drop the `#ifdef __APPLE__` constructor branch; the 48-px row layout is used everywhere), `src/app/screens/shell/shell.cpp` (`buildMain`: `_header` always in `_mainPane`; delete `_titleLabel`, its setText in workspace switching and the Settings show/hide added on `fix/image-viewer-titlebar`; `syncFullScreenChrome` only hides the title strip; delete `_header->setEnabled(false/true)` around Settings), `src/app/screens/shell/shell.h` (`_titleLabel` gone)
- Test: `tests/app/screens/shell/test_shell_ui.cpp`

**Interfaces:**
- Consumes: Task 3's title strip with the bar.
- Produces: nothing new for later tasks.

- [ ] **Step 1: Write the failing tests:**
  - `header row: the conversation header sits in the message pane` — after opening `#talk-ai`, `h.sh->header().parent()` is the main pane (not the title strip) on every platform, and its title text is "talk-ai".
  - `header row: full screen keeps the header and ⌘F opens search` — simulate full screen (`syncFullScreenChrome` with the fake window fullscreen); title strip hidden, header still visible, ⌘F opens `unifiedSearch()`.
- [ ] **Step 2: Run** `ctest --test-dir build -R shell_ui_tests --output-on-failure` → FAIL on macOS builds.
- [ ] **Step 3: Implement** the deletions listed under Files; keep the title-strip height from `_win.native().titleBarHeight()`.
- [ ] **Step 4: Run** `./scripts/build.sh --test` → all pass except the three known pre-existing failures.
- [ ] **Step 5: Commit** `git commit -m "header: conversation header as a row on macOS too; centred title removed"`

### Task 5: Strings, full check, local build

**Files:**
- Modify: `src/app/i18n/ja.po`, `src/app/i18n/languages_generated.cpp` (generated)

- [ ] **Step 1: Run** `python3 -I src/tools/i18n.py update` and translate every new empty `msgstr` in `ja.po` (e.g. "Search %1" → "%1 を検索", "Recent" → "最近", "People" → "メンバー", "Channels" → "チャンネル", "Messages" → "メッセージ", "Search all messages for “%1”" → "「%1」をすべてのメッセージから検索").
- [ ] **Step 2: Run** `python3 -I src/tools/i18n.py update` again → "N/N translated"; `./scripts/build.sh --test` → `i18n_up_to_date` passes; clang-format check clean on all changed files.
- [ ] **Step 3: Build the personal app** in `~/orca/workspaces/msga-local-build` with this branch merged with `fix/image-viewer-titlebar` and `feat/import-slack-reactions` plus the local Lato line, then launch it and check by hand: bar visible, ⌘F/⌘K/click open the panel, typing "henry" groups results, Enter opens.
- [ ] **Step 4: Commit** `git add src/app/i18n && git commit -m "i18n: unified search strings (ja)"`
