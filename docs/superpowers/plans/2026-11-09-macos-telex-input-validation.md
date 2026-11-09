# Telex correction follow-up — implementation and validation

## Reported symptoms

With Apple's Vietnamese Simple Telex, repeatedly typing `c` in an empty composer left the caret at the beginning of underlined text. Arrow-based correction and replacement were also unreliable. Pending text left the Send button grey.

## Root causes and implemented corrections

- The paragraph-length calculation excluded insertion-only preedit at an empty paragraph or the end of a paragraph. Caret placement consequently clamped to the old paragraph length. Paragraph lengths now count the visible composition replacement.
- TextEdit swallowed app-owned keys even after Cocoa had offered them to the IME. Ranged/document composition now finalizes before unconsumed navigation. Enter confirms before any submit hook, including empty compositions. Legacy backend preedit retains its existing key ownership.
- Partial marked replacement lost the retained prefix when translating its selection. Both native replacement selectors now use one checked effective-document transaction. The transaction retains intervening committed text even when the explicit edit is outside the marked span.
- Finalizing composition discarded its internal caret/selection. The existing replace/undo path now accepts an optional selection within inserted text, also carried on native TextInput events. Undo/redo retains the resulting selection.
- Native unmark followed by insertion in one key used stale committed text. Unmark now updates the native local document before subsequent callbacks.
- A locally moved native pending batch survived focus or authoritative document changes. Buffered events carry their expected document acknowledgment; focus generation or an unexpected document update aborts the remaining old edits instead of rebasing them.
- Composer affordances consulted only committed text. A separate preedit notification updates Send from visible composed text, without pretending every preedit update is a committed edit or running spell checks on it.
- Explicit send/schedule finalizes composition. Draft stashing and entering message-edit mode finalize it before serializing or replacing the old draft. This avoids losing visible text when switching conversations.

No dependencies, Vietnamese parsing rules, application preferences, or installed applications were changed. The normal IME underline remains while text is being composed; it is not itself an error.

## Regression evidence

Tests were added before the corresponding fixes and observed failing:

- Repeated insertion-only composition, paragraph-end/multiline Unicode caret geometry.
- Unconsumed Left/Shift+Left and safe Enter confirmation.
- Legacy explicit composition commit.
- Marked internal selection, navigation and undo/redo selection.
- Empty composition confirmation before composer hooks.
- Native partial marked selection (including emoji/UTF-16 translation).
- Out-of-bounds and overflowing native edits.
- Native disjoint edits before/after marked text, including preservation of the committed gap.
- Native unmark→insert and pending batch invalidation after focus or authoritative edits.
- Composer send affordance, explicit sending, scheduling, cancellation, conversation switching and set-aside draft preservation when editing a sent message.

After the final source build, the focused native IME run passed **11 cases, zero failures, zero skips**. Both standalone platform CTests passed. The final application run passed **126/129 CTests**, including `ui_textedit` and `shell_ui_tests`. Failures: `core_slack_write`, `core_slack_features`, and `messages_actions`. Both Slack suites passed on a final serial rerun (2/2); the reaction failure remained reproducible in its earlier serial rerun. All identified input regressions pass, but the complete project suite is not green.

## Commands

```sh
cmake --build build-ime-tests -j 6
ctest --test-dir build-ime-tests --output-on-failure -j 4
cmake --build build-plat-ime -j 6
./build-plat-ime/tests/plat_cocoa_tests --ime-only
ctest --test-dir build-plat-ime --output-on-failure
git diff --check
```

The native executable's default invocation still runs its entire suite; `--ime-only` makes input regression checks available without unrelated fullscreen/presentation tests. Earlier default native runs exposed a fullscreen transition failure and intermittent presentation/drop timing failures. These were not changed or hidden by this patch.

The first full application run passed 127/129 CTests. `core_slack_write` (Grid token routing) and `messages_actions` (one-click skin-tone reactions) failed. Serial rerun passed `core_slack_write`, while `messages_actions` still failed. Neither test concerns the modified input paths; do not describe the whole suite as green.

## Native-input validation limit

A safe, separate local probe used the already-selected Apple Vietnamese Simple Telex source, without changing keyboard settings or sending messages. Twelve `c` keyDown events preserved all characters and native selection advanced from 1 to 12. A `tieengs ` sequence matched a native NSTextView reference, but both received literal `tieengs `: direct synthetic keyDown does **not** demonstrate real Apple accent conversion. These probe results are not a substitute for hardware typing in the built application. The actual marked-text screenshot path is covered by the failing-then-passing native/editor composition regressions.

Manual acceptance still needed: open the new local build, repeat the `c` sequence, type Vietnamese accents, navigate into a typo and replace it, then verify Enter, Send, undo and draft switching. Linux/Windows runtime IME behavior was not exercised; no non-Cocoa backend was changed.

## Decisions and remaining risks

- Reused the committed input-contract implementation in 96203b0 rather than replaying the original plan. Risk: existing connected gaps had to be discovered through regression tests and review; six were found and addressed.
- Kept the patch uncommitted and did not replace `/Applications/msga.app`. The user must launch the newly built app, not merely restart an older installed binary.
- Added optional selection metadata rather than moving the caret after edit callbacks. This keeps undo and callbacks consistent; events without the metadata retain the normal end-of-insertion behavior.
- Invalidated the remaining old native batch after unexpected application edits instead of silently rebasing it. A callback intentionally changing content may suppress the rest of that one key's callbacks; this is preferable to corrupting the replacement document.
- Arbitrary-range geometry and screen-point hit testing remain outside this patch. Caret-based candidate positioning is retained.

## Local build

`build-ime-tests/msga.app` (executable: `build-ime-tests/msga.app/Contents/MacOS/msga`). This is not the installed `/Applications/msga.app`.
