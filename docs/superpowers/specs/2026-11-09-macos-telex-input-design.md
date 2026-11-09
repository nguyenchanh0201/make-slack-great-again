# macOS Telex input design

## Intent

Make MSGA editing behave like a normal macOS text editor with Apple's Vietnamese Simple Telex, especially after arrow-key movement back into an existing word to correct a typo. Preserve surrounding text, caret position, formatting, and undo. The user requested a plan, not implementation.

## Evidence and limits

- `src/plat/src/cocoa/cocoa_window.mm` ignores both native `replacementRange` parameters, supplies a fake selection, and returns no surrounding text.
- `plat::TextInputState` contains only enabled/caret geometry. `plat::Event` has no replacement range.
- `ui::TextEdit` inserts commits at its own selection and displays preedit as an insertion, not an overlay replacing an existing range.
- Existing UI tests exercise basic preedit/commit, not native reconversion or replacement after navigation.
- Installed Slack uses Electron; upstream Chromium maintains surrounding text, selection, and composition information. This is an architectural reference, not proof about the precise Chromium revision bundled with Slack.
- The user's input method and navigation/correction scenario are confirmed. No exact failing keystroke trace has been captured yet. Capture one before implementation; do not claim this investigation already reproduces it.

## Chosen approach (engineering recommendation)

Extend the existing C++/Objective-C++ bridge to implement the macOS NSTextInputClient text, selection, composition, and replacement contract. This is not a claim of complete text-service parity: arbitrary range geometry and screen-point hit testing are explicitly limited below. Keep TextEdit's UTF-8 model and its existing rendering/undo pipeline. Do not embed Chromium, switch to NSTextView, or add a Vietnamese parser or injected-backspace workaround.

A native-control replacement would affect rich text, layout, styling, and other platforms. A Telex-only patch would leave the same document-contract bugs for other input methods. Fix the shared contract once.

## Required contract

1. Publish the focused editor's committed UTF-8 text, actual caret/anchor, composition replacement range, preedit string, and preedit selection together with caret geometry. Publish on text, selection, composition, and focus changes, even when geometry is unchanged.
2. Platform-neutral document replacement ranges are half-open UTF-8 byte offsets in committed text. Composition selection and preeditCursorBegin/End are the explicit exception: UTF-8 offsets within preedit. Native Cocoa ranges are UTF-16 offsets in the effective document. The effective document replaces the committed composition range with preedit. Distinguish absent ranges from valid empty insertion ranges.
3. Cocoa maps native ranges through that effective document. For setMarkedText, a replacement wholly within marked text updates preedit and retains its underlying committed replacement range. A replacement extending outside marked text expands that range and preserves untouched prefix/suffix. For insertText, resolve the explicit effective-document edit and finalize the composition: marked `abc` plus insertText(`X`, range covering `b`) commits `aXc` over the underlying committed replacement span. If the edit extends beyond marked text, include the affected committed span in the single transaction. Never interpret preedit bytes as committed-model offsets.
4. Honor explicit replacement ranges for `insertText` and `setMarkedText`. With NSNotFound, replace active marked text, otherwise current selection. An empty insertion with an explicit nonempty replacement is deletion, not a no-op.
5. Return actual document-relative `selectedRange` and `markedRange`. Implement `attributedSubstringForProposedRange:actualRange:` against the effective text with valid intersection semantics and actualRange reporting. Initialize actualRange on failure. Never split a UTF-8 sequence or UTF-16 surrogate pair; reject malformed edit ranges without mutating text. Keep normalization unchanged (NFC/NFD both supported).
6. Composition is a transaction: hold the original committed replacement range and selection while displaying its replacement. Do not delete selected committed text merely to start preedit. Commit through the existing `replace`/undo machinery once; cancellation restores original text/selection. Preserve the affected original formatting for cancellation and undo; committed replacement follows the existing typing-format rule.
7. Differentiate finalizing marked text (`unmarkText`: retain visible text, remove marking) from explicit cancellation. `NSTextInputContext.discardMarkedText` is a client-to-context notification, NOT a cancellation callback; clear the client's marked state when sending it. A confirmed unconsumed cancelOperation: command cancels and restores original selection/text, then notifies the context. An empty setMarkedText is an empty replacement update, not by itself proof of cancellation; retain an internal transaction even if marked length becomes zero until finalization/cancellation. Capture Apple's actual Escape/empty-mark sequence before implementation and reconcile this policy with the observed native reference. No lost or duplicate commits if AppKit calls insert then unmark. For a focus switch or editor teardown, commit active preedit once before detaching the old editor. This focus policy is an engineering recommendation and needs live validation.
8. Let the input context process composition keys first. Only forward editing commands AppKit leaves to the application. Arrow navigation must work when not consumed; Enter that confirms composition must not also send a message. Command shortcuts remain functional.
9. Native callbacks and buffered key handling must use one coherent state. Preserve replacement metadata with pending text, update the local effective document between native callbacks, and reconcile with the editor after delivery. Do not regress KeyDown/TextInput order or allow a pending edit to land in a newly focused editor.
10. Focus loss clears document state and pending edits after finalizing the old editor. Masked fields never expose real surrounding text to the IME. Non-Cocoa backends may ignore added document metadata and keep existing behavior.
11. Before user-driven mouse/selection moves, paste/cut, or undo/redo outside the IME, finalize active composition once, then execute the action against the resulting document; Undo therefore undoes the finalized composition first. Public programmatic setText cancels the overlay before installing authoritative replacement text; public setSelection finalizes then moves. Reentrant onChange callbacks that replace content/focus invalidate the remainder of the old pending batch; never rebase it silently. Handle async paste against the active editor state when its data is applied, not an old composition range. These are engineering policy choices.
12. Geometry scope: retain caret-based candidate positioning. firstRectForCharacterRange must set actualRange to the actual supported caret/marked-selection position and must not pretend the returned caret rectangle represents an arbitrary requested span; initialize outputs on failure. characterIndexForPoint may remain NSNotFound (unsupported) in this patch. Accurate arbitrary-range rectangles and screen-point hit testing are deferred. If the captured Simple Telex trace requires those capabilities, stop and revise scope rather than shipping a misleading fallback.

## Scope and constraints

- No new dependencies.
- Preserve the existing UTF-8 editor model and rich-text rendering.
- No application feature work or settings switch.
- No automatic keyboard-source changes or typing into real conversations during testing.
- Do not change unrelated pre-existing working-tree edits.
- Fix the shared TextEdit path, covering composer, search, and other TextEdit users.
- Avoid per-frame full-document copies: publish on document/selection/composition changes, not caret blink or paint. A bounded text window is deferred until profiling justifies its offset/ownership complexity.

## Acceptance

- In a safe local test editor, capture an exact Simple Telex typo/correction trace and verify the same final Unicode text and caret in MSGA and a native NSTextView reference; compare Slack in an unsent draft if available.
- Arrow left/right and Option/Shift navigation followed by replacement change only the intended range, including in the middle of a line and before/after emoji.
- Multi-stage composition, selected text, empty replacement, explicit cancellation, focus transfer, undo/redo, paste, dead keys, and Enter-to-send retain their documented behavior.
- Test composed and decomposed Vietnamese, a supplementary-plane emoji, multiline text, invalid ranges, and masked input.
- Deterministic native callback tests and headless TextEdit tests both pass. Real Apple IME validation remains required; synthetic text injection alone is not acceptance.

## Remaining implementation discretion

Small helper placement and names private to Cocoa/TextEdit may follow surrounding conventions. Whether to cache UTF conversion indexes is a measured implementation choice; correctness comes first. Do not broaden into global editor rewrites or cross-platform IME feature parity.
