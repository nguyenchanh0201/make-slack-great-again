# macOS Telex Input Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Apple's Vietnamese Simple Telex correctly replace existing text after caret navigation, without corrupting surrounding text or composition.

**Architecture:** Extend the current platform input contract to carry document state and replacement ranges. Keep UTF-8 in TextEdit; convert Cocoa's UTF-16 ranges at the native boundary against an effective document that includes marked text. Use a transactional composition overlay and the existing undo/formatting path rather than a native-control rewrite or Telex-specific workaround.

**Tech Stack:** C++20, Objective-C++/AppKit NSTextInputClient, existing plat and UI test harnesses, CMake/CTest; no new dependencies.

**Spec:** `docs/superpowers/specs/2026-11-09-macos-telex-input-design.md` (proposed design; review before implementation).

## Global Constraints

- No new dependencies.
- Preserve the existing UTF-8 editor model and rich-text rendering.
- No application feature work or settings switch.
- No automatic keyboard-source changes or typing into real conversations during testing.
- Do not change unrelated pre-existing working-tree edits.
- Fix the shared TextEdit path, covering composer, search, and other TextEdit users.
- Planning only until the user approves implementation.

## Review Focus

- UTF-16 surrogate pairs and decomposed Vietnamese: replacement must preserve Unicode boundaries and exact normalization (Tasks 1–3).
- Several native callbacks during one key: replacements must refer to the updated effective document, not stale state (Task 3).
- A selected formatted span becomes preedit: cancellation/undo must restore both text and formatting (Task 2).
- Focus changes during buffered input: no old-document content or edit may reach the next field; masked contents remain private (Tasks 2–3).
- Enter/arrow/shortcut ownership: composition confirmation must not submit, and navigation must not be lost (Tasks 3–4).

---

## File map

| File | Responsibility |
| --- | --- |
| `src/plat/include/plat/plat.h` | Optional document snapshot, replacement range, explicit composition ending semantics |
| `src/ui/view.h`, `src/ui/window.cpp` | Forward document state, bypass geometry-only deduplication when state changes |
| `src/ui/textedit.h`, `src/ui/textedit.cpp` | Publish focused state; replace ranges; transactional preedit overlay, undo and lifecycle |
| `src/plat/src/cocoa/cocoa_window.mm` | Native document queries, UTF-16 mapping, pending input metadata and key ownership |
| `src/plat/src/cocoa/cocoa_internal.h` | Native helper declarations only if needed |
| `tests/ui/test_textedit.cpp` | Deterministic model, selection, formatting, lifecycle and undo assertions |
| `tests/plat/cocoa_tests.mm` | Direct NSTextInputClient callback/query and callback-order tests |
| `src/plat/src/{headless,x11,wayland,win32}/…` | Audit added fields for source compatibility; change only if required |

Keep conversion helpers private to `cocoa_window.mm` initially. Do not add generic abstractions or a Vietnamese engine.

## Proposed shared interfaces

Add these types in namespace `plat` in `plat.h` (names are plan decisions):

```cpp
struct TextRange { uint32_t begin = 0, end = 0; }; // half-open committed UTF-8 bytes
struct TextComposition {
    TextRange replacement;
    std::string text;
    TextRange selection; // UTF-8 bytes relative to composition text
};
struct TextDocument {
    std::string text; // committed text only
    uint32_t anchor = 0, caret = 0;
    std::optional<TextComposition> composition;
};
enum class CompositionEnd : uint8_t { None, Commit, Cancel };
```

Extend `TextInputState` with `std::optional<TextDocument> document`. Absent means no document-query support, not an empty editor. Extend `Event` with `std::optional<TextRange> textReplacement` and `CompositionEnd compositionEnd = CompositionEnd::None`. `TextInput` commits a replacement; empty text may delete an explicit range. `TextPreedit` updates composition when `compositionEnd == None`, explicitly commits current preedit with `Commit`, or cancels with `Cancel`. For existing backend events lacking metadata, retain legacy empty-preedit behavior (end display, then a separate TextInput may follow); do not silently redefine other platforms' events as cancellation.

Change the UI forwarding signature to:

```cpp
void Window::setTextInput(bool enabled, RectF caret,
                         std::optional<plat::TextDocument> document = std::nullopt);
```

Publish an owned snapshot; do not keep a string_view into TextEdit. `document.composition` lets Cocoa construct the effective document while retaining the correct committed-model replacement range. Native ranges inside preedit are resolved by the Cocoa adapter, not passed as if they indexed committed text. No revision counter is required if ordered synchronous delivery plus local pending-state tracking is proven by Task 3; if delivery is not synchronous, stop and revise this contract before proceeding.

### Task 1: Pin the native failure and Unicode mapping contract

**Files:** `tests/plat/cocoa_tests.mm`; later edits to `cocoa_window.mm` are owned by Task 3.

**Interfaces:** Consumes existing CocoaWindow/view access; produces native regression cases and the captured real-input trace. Tests can seed proposed TextDocument state once Task 2 supplies the fields.

- [ ] Capture the user's scenario in a safe local editor with Apple Vietnamese Simple Telex. Record initial text, every arrow/edit key, native queries/callbacks including NSRanges, resulting text, and caret. Include Escape, empty marked-text updates, partial marked replacements, and arrow movement. Use temporary local diagnostics only; do not log real conversation contents or ship diagnostics. Compare with a plain NSTextView reference. Freeze the exact trace as a test fixture/comment; do not invent which callbacks Apple sends. **Evidence gate:** this plan is provisional until that trace exists; do not begin Task 2 implementation before reconciling the proposed cancellation/replacement policies with it. If range geometry is required, revise scope first.
- [ ] Add and register `caseImeDocumentQueries`, `caseImeReplacementRanges`, and `caseImeUnicodeRanges` in the existing `runCase` harness. Use explicit native callback calls so these cases do not depend on the global input source. Seed committed `"abc"`, caret 3; expect selectedRange `{3,0}`. Invoke insertText `@"é"` with replacement `{1,1}`; expect TextInput replacement `{1,2}` and UTF-8 `"é"`, not plain append-only input. Empty text with the same range must emit a deletion.
- [ ] Add mapping assertions using `"A😀e\u0301Z"`: native `{3,2}` maps to committed UTF-8 bytes `{5,8}`; substring queries return `e` plus combining acute unchanged. A range splitting the emoji's surrogate pair must not become an edit splitting UTF-8. Query partially unavailable ranges and assert truthful actualRange; failure initializes it to NSNotFound.
- [ ] Configure/build and demonstrate failure on the current query/replacement behavior. Tests depending on the new API initially fail compilation until Task 2 lands; record that separately from a behavioral failure.

```sh
cmake -S src/plat -B build-plat-ime -DPLAT_BUILD_TESTS=ON -DPLAT_TEST_HOOKS=ON
cmake --build build-plat-ime --target plat_cocoa_tests
./build-plat-ime/tests/plat_cocoa_tests
```

Expected red evidence: fake selection, absent replacement metadata, or unavailable document substring. Native tests need a macOS login/window-server session; skips are not passes.

- [ ] Commit the regression tests once they compile against Task 2's contract; keep Task 3 implementation separate.

### Task 2: Add the document contract and transactional editor replacements

**Files:** `plat.h`, `src/ui/view.h`, `src/ui/window.cpp`, `src/ui/textedit.{h,cpp}`, `tests/ui/test_textedit.cpp`.

**Interfaces:** Produces the exact shared types and Window signature above. Existing `TextEdit::updateIme()`, `replace(...)`, `toDisplay(uint32_t)`, and `toModel(uint32_t)` remain the implementation entry points. Add private `void finishComposition(bool commit)` for explicit ending; it must be idempotent.

- [ ] Add failing headless tests by dispatching a `plat::Event` through `Window::handle`, retaining raw replacement metadata. Case `textedit: IME replacement after navigation`: start `"abc"`, move caret to the end, send `TextInput("é", {1,2})`; expect `"aéc"`, caret 3 (UTF-8 bytes), then undo to `"abc"` and redo to `"aéc"`. Add `textedit: IME empty replacement deletes` expecting `"ac"` for empty text and `{1,2}`.
- [ ] Add `textedit: IME composition replacement is transactional`: start `"abc"`, preedit `"é"` over `{1,2}`; committed `text()` stays `"abc"`, effective display is `"aéc"`; update to `"ê"` without accumulating text; commit once to `"aêc"`. Cancel instead restores the original selection/text and creates no undo entry. Repeat with a bold/link formatted selected span, verifying formatting after cancel and undo.
- [ ] Add tests for replacement beside emoji and decomposed accents, reversed selections, multiline ranges, out-of-bounds/split-UTF-8 edit ranges (reject without mutation), and composition before/at the replaced range's endpoints. Rendering/hit-test mappings must skip the hidden committed span rather than merely insert preedit ahead of it.
- [ ] Add `textedit: IME document snapshot follows caret`: capture the state passed to the native/headless window after Left/Right, Shift selection, text edits, composition updates and undo. State changes must propagate even with identical caret geometry. Assert focused masked fields have `document == nullopt`; normal empty fields expose an empty document.
- [ ] Add `textedit: IME focus transfer commits once`: active preedit is committed to the old editor before detach, the next editor receives only its own snapshot, and subsequent end/cancel callbacks cannot duplicate the old commit. Include editor destruction and repeated finishing calls. Audit Window focus-switch/detach paths: current focus may already point elsewhere before FocusOut, and destruction can bypass FocusOut; finalize explicitly while the old editor is still valid.
- [ ] Add `textedit: external edits finish composition`: mouse/public selection changes, paste/cut and undo/redo finalize first; undo removes the finalized composition first. Programmatic setText cancels the overlay and installs authoritative text. Include async paste completing during composition and onChange replacing text or moving focus; abort remaining old pending operations rather than applying stale ranges. Trace all public mutators, not just keyboard handlers.
- [ ] Run red tests, then implement the shared fields, forwarding signature, and snapshot publication. Move document publication out of any geometry-only early return. Preserve no-document callers and audit every `setTextInput` caller/backend. Publish on changes, not paint/blink.
- [ ] Implement the composition overlay as a committed replacement interval plus preedit selection, retaining original selection until commit/cancel. Update model/display mapping, paragraph rendering, caret placement and hit testing accordingly. Apply final edits through existing `replace` so undo and formatting have one source of truth; do not pre-delete the selection. Accept empty-text ranged deletion before the current `t.empty()` early return.
- [ ] Implement explicit commit/cancel lifecycle through `finishComposition(bool commit)`, and preserve legacy no-metadata preedit/commit handling for other backends. Snapshot masked fields without real contents. Emit no temporary model changes/onChange callbacks for overlay updates.
- [ ] Verify all UI cases, not just new IME tests:

```sh
cmake -S . -B build-ime-tests -DMSGA_BUILD_TESTS=ON
cmake --build build-ime-tests --target ui_tests
ctest --test-dir build-ime-tests -R '^ui_textedit$' --output-on-failure
ctest --test-dir build-ime-tests -R '^ui_' --output-on-failure
```

Expected: nonzero discovered tests, all pass, no skipped new cases. These commands are planned, not already executed.

- [ ] Commit only the named contract/editor/test files: `fix(input): preserve document ranges and transactional composition`.

### Task 3: Implement the Cocoa document adapter and key lifecycle

**Files:** `src/plat/src/cocoa/cocoa_window.mm`, optional declarations in `cocoa_internal.h`, `tests/plat/cocoa_tests.mm`.

**Interfaces:** Consumes Task 2's document snapshot and event metadata. Implements existing NSTextInputClient selectors and uses Cocoa-local helpers for effective-document construction and checked UTF-16/UTF-8 mapping. Keeps pending operations as complete events, not only `std::string` values.

- [ ] Add failing `caseImeMarkedReplacement`, `caseImeCallbackOrdering`, `caseImeEndComposition`, and `caseImeKeyOwnership` to the native harness. Drive setMarkedText over an existing committed span; verify document-relative markedRange/selectedRange and surrounding substrings, then commit and compare replacement metadata. Test explicit replacement inside marked text and across both marked and committed spans.
- [ ] In ordering tests, issue multiple native callbacks during a single key interpretation. Verify each sees the preceding pending edit, final commits target the original editor, and KeyDown precedes ordinary TextInput. Test focus changes in an event callback and ensure pending edits cannot target the new field. Inspect `CocoaApp::emitEvent` delivery before relying on synchrony; revise the API if it queues.
- [ ] In ending tests, verify unmark finalizes visible text exactly once, insertText followed by unmark does not duplicate, confirmed unconsumed cancelOperation: restores underlying text/selection, and disabling input clears cached document/pending data. `discardMarkedText` only notifies the input context; never treat it as an incoming cancellation signal. Empty setMarkedText remains a transaction update until explicitly ended, even though hasMarkedText may report no marked characters. Resolve any contrary Apple callback trace at the Task 1 evidence gate. Test a new composition after focus transfer and masked/no-document state.
- [ ] Add partial-commit assertions: preedit `abc` replacing a committed range, followed by insertText `X` over native `b`, emits one final commit `aXc` over that underlying committed span. setMarkedText over the same subrange instead leaves `aXc` marked. Include replacement spanning marked plus committed characters and verify preservation of untouched text.
- [ ] In key-ownership tests, verify an IME-consumed Enter is not emitted as app KeyDown, while an unconsumed arrow/edit command is forwarded once even if marked text existed before interpretation. Verify Cmd+C/V/Z and Option/Shift navigation do not duplicate text or vanish. Use selector behavior/captured trace rather than assuming all keys during composition are consumed.
- [ ] Run the native suite red. Implement selectedRange/markedRange/substring queries against effective text, returning native UTF-16 offsets. Return nil/NSNotFound safely when no document is available; maintain fallback IME support for no-document users without fabricated document contents.
- [ ] Implement both replacementRange selectors using the spec's fallback ordering and checked mapping. Replace `_pendingText` strings with full event records including replacement metadata; retain a local effective state across callbacks before the editor acknowledges delivery. Do not clear preedit first in a way that loses its committed replacement interval.
- [ ] Implement finalization versus explicit cancel semantics and native command ownership. Audit `doCommandBySelector`, `_imeTouched`, `hadMarked`, and matching key-up bookkeeping together; a blanket `hadMarked` suppression is insufficient evidence that an arrow was consumed. `discardMarkedText` is outbound context notification, not an incoming selector. Invalidate character coordinates when geometry changes, but refresh document state independently.
- [ ] Add a native geometry-output test and initialize firstRectForCharacterRange's actualRange on every path. Return a truthful supported caret/marked-selection position with the caret rectangle, not the arbitrary proposed span. Keep characterIndexForPoint unsupported (NSNotFound); document this deliberate limit. If Task 1 shows Simple Telex needs arbitrary-range geometry, stop and revise this plan rather than proceeding with the fallback.
- [ ] Rebuild/run the three native regression cases from Task 1 and the added Task 3 cases, then the full standalone platform tests:

```sh
cmake --build build-plat-ime
./build-plat-ime/tests/plat_cocoa_tests
ctest --test-dir build-plat-ime --output-on-failure
cmake --build build-ime-tests --target ui_tests
ctest --test-dir build-ime-tests -R '^ui_' --output-on-failure
```

Expected: all tests pass; native cases actually run in a login session. Run available Linux/Windows build CI to catch shared header/source compatibility; report unavailable platforms rather than claiming validation.

- [ ] Commit the adapter and native tests: `fix(macos): honor native IME document and replacement ranges`.

### Task 4: Validate real Simple Telex corrections and delivery scope

**Files:** Add a concise validation record to this plan or a PR description; no production code unless a reproduced failure requires returning to an earlier task.

**Interfaces:** Consumes the captured trace from Task 1 and the completed adapter/editor path.

- [ ] Build a local test/demo executable without replacing `/Applications/msga.app` or changing the user's input-source settings. Use the repository's demo/test fixture workflow, not a live sent message.
- [ ] With the user selecting Apple Vietnamese Simple Telex, replay the exact typo/arrow/correction trace from Task 1 and compare final text/caret with NSTextView and, if available, Slack in an unsent draft. Record exact key sequence, expected/resulting Unicode text, macOS/input-method version, and whether each comparison was actually performed.
- [ ] Repeat at start/middle/end of a word, on both sides of `😀`, with decomposed Vietnamese, selection replacement, Shift/Option arrows, multiline text, Backspace/Delete, undo/redo, paste and focus switching. Verify composition confirmation with Enter does not send, and a subsequent ordinary Enter still does. Smoke-test available dead-key, Japanese or Chinese input without making that a new feature project.
- [ ] Repeat the core trace in composer and search (shared TextEdit), check masked-field privacy, and confirm document publication does not copy the full text on every paint/blink.
- [ ] Run the UI/native verification commands again, inspect `git diff --check` and the final diff, and verify unrelated existing changes are untouched. Remove temporary diagnostic logging. Record any skipped platform or manual tests explicitly.
- [ ] Prepare only final targeted regression/validation additions. All commit steps in this plan are suggested checkpoints for an authorized implementation, not permission to commit now. Do not commit, install, push, or ship automatically without user authorization.

## Completion gate

The change is not complete until the exact real Simple Telex correction works and deterministic native/editor regressions pass. Headless tests alone do not prove Apple IME compatibility. If live macOS validation is unavailable, hand over a tested patch with that limitation rather than claiming the issue cannot recur.
