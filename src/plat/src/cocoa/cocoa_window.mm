// Cocoa windows: an NSWindow whose content view (PlatView) is a plain layer we
// hand CPU frames to, plus the NSTextInputClient half of IME support, touchpad
// phases and gestures, and both ends of drag and drop.
#include "cocoa/cocoa_internal.h"
#include "core/drop.h"

#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using plat::Button;
using plat::Event;
using plat::EventType;
using plat::HitArea;
using plat::Key;
using plat::cocoa::CocoaWindow;

namespace {

static bool isSurrogateLow(unichar c) {
    return c >= 0xDC00 && c <= 0xDFFF;
}
static bool isSurrogateHigh(unichar c) {
    return c >= 0xD800 && c <= 0xDBFF;
}

static NSUInteger utf16LengthOfUtf8(std::string_view u8) {
    if (u8.empty())
        return 0;
    NSString *s = [[NSString alloc] initWithBytes:u8.data()
                                           length:u8.size()
                                         encoding:NSUTF8StringEncoding];
    return s ? s.length : 0;
}

static NSInteger utf8OffsetFromUtf16(NSString *s, NSUInteger u16) {
    if (!s || u16 == 0)
        return 0;
    if (u16 >= s.length)
        return (NSInteger)[s lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    if (isSurrogateLow([s characterAtIndex:u16]) && isSurrogateHigh([s characterAtIndex:u16 - 1]))
        return -1; // splits surrogate pair
    NSString *prefix = [s substringToIndex:u16];
    return (NSInteger)[prefix lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
}

static NSUInteger utf16OffsetFromUtf8(std::string_view u8, uint32_t byteOffset) {
    if (byteOffset == 0)
        return 0;
    if (byteOffset >= u8.size())
        return utf16LengthOfUtf8(u8);
    if ((uint8_t(u8[byteOffset]) & 0xC0) == 0x80)
        return NSNotFound; // splits UTF-8
    NSString *prefix = [[NSString alloc] initWithBytes:u8.data()
                                                length:byteOffset
                                              encoding:NSUTF8StringEncoding];
    return prefix ? prefix.length : NSNotFound;
}

// UTF-8 byte length of the first `utf16` code units of s — preedit cursor
// offsets are bytes in plat but NSRange counts UTF-16 units.
int utf8Offset(NSString *s, NSUInteger utf16) {
    utf16 = std::min<NSUInteger>(utf16, s.length);
    return int([[s substringToIndex:utf16] lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
}

NSString *plainString(id s) {
    return [s isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)s string]
                                                        : (NSString *)s;
}

// AppKit hands Cmd/Ctrl chords, arrows and function keys to insertText: as
// control characters or private-use code points (NSUpArrowFunctionKey…);
// none of that is text.
std::string filteredText(NSString *s) {
    NSMutableString *out = [NSMutableString string];
    [s enumerateSubstringsInRange:NSMakeRange(0, s.length)
                          options:NSStringEnumerationByComposedCharacterSequences
                       usingBlock:^(NSString *sub, NSRange, NSRange, BOOL *) {
                         const unichar c = [sub characterAtIndex:0];
                         if (c < 0x20 || c == 0x7f || (c >= 0xF700 && c <= 0xF8FF))
                             return;
                         [out appendString:sub];
                       }];
    return out.length ? std::string(out.UTF8String) : std::string();
}

// Device-dependent modifier bits (IOKit's NX_DEVICE*KEYMASK) in
// NSEvent.modifierFlags: the only way to tell which of two Shifts changed.
constexpr NSEventModifierFlags kDevLCtrl = 0x0001, kDevLShift = 0x0002, kDevRShift = 0x0004,
                               kDevLCmd = 0x0008, kDevRCmd = 0x0010, kDevLAlt = 0x0020,
                               kDevRAlt = 0x0040, kDevRCtrl = 0x2000;

} // namespace

// ── Frame clock target (CADisplayLink retains its target) ──────────────────
@interface                                PlatFrameTarget : NSObject
@property(nonatomic, assign) CocoaWindow *owner;
@end
@implementation PlatFrameTarget
- (void)tick:(id)link {
    if (_owner)
        _owner->deliverFrameIfDue();
}
@end

// ── Content view ────────────────────────────────────────────────────────────

struct PendingTextEvent {
    plat::Event                       event;
    std::optional<plat::TextDocument> document;
};

@implementation PlatView {
    NSTrackingArea          *_tracking;
    bool                     _hasDoc;
    std::string              _docText;
    uint32_t                 _docAnchor;
    uint32_t                 _docCaret;
    bool                     _hasMarked;
    std::string              _markedText;
    plat::TextRange          _markedRepl;
    plat::TextRange          _markedSel;
    // State of the key event being run through interpretKeyEvents:
    bool                     _inKeyDown;
    bool                     _imeTouched;
    bool                     _commandCalled;
    SEL                      _commandSelector;
    std::vector<PendingTextEvent>     _pendingEvents;
    uint64_t                          _documentGeneration;
    bool                              _checkingDocument;
    std::optional<plat::TextDocument> _expectedDocument;
    bool                     _keyDownSent[128];
    // The offered MIME types of the drag over us, kept for the moves that
    // follow: valid while the drag (sequence number) and the pasteboard
    // contents (change count) are the same.
    std::vector<std::string> _dropMimes;
    NSInteger                _dropSeq;
    NSInteger                _dropChange;
    bool                     _dropKnown;
}

- (void)sendTextEvent:(plat::Event)e {
    if (_inKeyDown) {
        std::optional<plat::TextDocument> document;
        if (_hasDoc) {
            document = plat::TextDocument{_docText, _docAnchor, _docCaret, std::nullopt};
            if (_hasMarked)
                document->composition = plat::TextComposition{_markedRepl, _markedText, _markedSel};
        }
        _pendingEvents.push_back({std::move(e), std::move(document)});
    } else if (_owner) {
        _owner->emit(e);
    }
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer                = YES;
        // We own layer.contents; AppKit must never redraw or clear it.
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        // A live-resize frame shows the old image pinned to the top-left
        // until the synchronous repaint lands, instead of stretching it.
        self.layerContentsPlacement    = NSViewLayerContentsPlacementTopLeft;
        [self registerForDraggedTypes:[PlatView dropTypes]];
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
} // top-left origin, like plat
- (BOOL)isOpaque {
    return YES;
}
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (void)updateLayer {
    // AppKit wants the view redrawn (exposure after deminiaturize, a screen
    // change, …): that is a Frame in plat's terms.
    if (_owner)
        _owner->requestFrame();
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent *)ev {
    return YES;
}
- (BOOL)mouseDownCanMoveWindow {
    return NO;
} // the hit-test callback decides

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (_owner)
        _owner->viewResized();
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (_owner)
        _owner->backingChanged();
}

- (void)updateTrackingAreas {
    if (_tracking)
        [self removeTrackingArea:_tracking];
    // ActiveAlways: hover feedback in inactive windows too, as on the other
    // platforms. InVisibleRect keeps it sized to the view for free.
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                     NSTrackingCursorUpdate | NSTrackingActiveAlways | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

// ── pointer ─────────────────────────────────────────────────────────────────

- (void)cursorUpdate:(NSEvent *)ev {
    if (_owner)
        _owner->applyCursor();
}

- (void)mouseEntered:(NSEvent *)ev {
    if (!_owner || _owner->pointerInside)
        return;
    _owner->pointerInside = true;
    _owner->applyCursor();
    _owner->emit(
        {.type = EventType::PointerEnter,
         .pos  = _owner->viewPoint(ev),
         .mods = plat::cocoa::modsFromFlags(ev.modifierFlags)}
    );
}

- (void)mouseExited:(NSEvent *)ev {
    if (!_owner || !_owner->pointerInside)
        return;
    _owner->pointerLeft();
}

- (void)movedWith:(NSEvent *)ev {
    if (!_owner)
        return;
    // Motion can precede mouseEntered: (window just mapped under the
    // pointer); the contract wants Enter first.
    if (!_owner->pointerInside)
        [self mouseEntered:ev];
    if (ev.type != NSEventTypeMouseMoved)
        _owner->lastMouseEvent = ev; // what a startDrag() from this Move starts from
    _owner->emit(
        {.type = EventType::PointerMove,
         .pos  = _owner->viewPoint(ev),
         .mods = plat::cocoa::modsFromFlags(ev.modifierFlags)}
    );
}
- (void)mouseMoved:(NSEvent *)ev {
    [self movedWith:ev];
}
- (void)mouseDragged:(NSEvent *)ev {
    [self movedWith:ev];
}
- (void)rightMouseDragged:(NSEvent *)ev {
    [self movedWith:ev];
}
- (void)otherMouseDragged:(NSEvent *)ev {
    [self movedWith:ev];
}

- (void)button:(Button)b down:(bool)down event:(NSEvent *)ev {
    if (!_owner)
        return;
    const plat::Point p = _owner->viewPoint(ev);
    const auto        i = size_t(b);
    if (down) {
        _owner->swallowUp[i] = false;
        if (b == Button::Left && _owner->hitTest) {
            const HitArea a = _owner->hitTest(p);
            if (plat::isNonClient(a)) {
                _owner->swallowUp[i] = true;
                if (a == HitArea::Caption)
                    [self captionPress:ev];
                // Resize*: macOS resizes from its own edge zone, which sits on
                // and just outside the frame and already took the press if it
                // was there; a press further in is swallowed like on the
                // other backends but starts nothing (AppKit has no public
                // "begin interactive resize").
                return;
            }
        }
        _owner->lastMouseEvent = ev; // what a startDrag() from this Down starts from
        _owner->emit(
            {.type   = EventType::PointerDown,
             .pos    = p,
             .button = b,
             .clicks = int(std::max<NSInteger>(1, ev.clickCount)),
             .mods   = plat::cocoa::modsFromFlags(ev.modifierFlags)}
        );
    } else {
        _owner->lastMouseEvent = nil;
        if (_owner->swallowUp[i]) {
            _owner->swallowUp[i] = false;
            return;
        }
        _owner->emit(
            {.type   = EventType::PointerUp,
             .pos    = p,
             .button = b,
             .mods   = plat::cocoa::modsFromFlags(ev.modifierFlags)}
        );
    }
}

- (void)captionPress:(NSEvent *)ev {
    NSWindow *w = self.window;
    if (ev.clickCount == 2) {
        // The title-bar double-click action is a user setting (System
        // Settings → Desktop & Dock); honour it like a native title bar.
        NSString *action =
            [NSUserDefaults.standardUserDefaults stringForKey:@"AppleActionOnDoubleClick"];
        if ([action isEqualToString:@"None"])
            return;
        if ([action isEqualToString:@"Minimize"] ||
            (!action &&
             [NSUserDefaults.standardUserDefaults boolForKey:@"AppleMiniaturizeOnDoubleClick"]))
            [w performMiniaturize:nil];
        else
            [w performZoom:nil]; // "Maximize", "Fill" (no public API) and the default
        return;
    }
    // Hands the drag to the window server: Spaces, snapping to screen
    // edges and tiling all keep working.
    [w performWindowDragWithEvent:ev];
}

- (Button)otherButton:(NSEvent *)ev ok:(bool *)ok {
    *ok = true;
    switch (ev.buttonNumber) {
    case 2:
        return Button::Middle;
    case 3:
        return Button::Back;
    case 4:
        return Button::Forward;
    default:
        *ok = false;
        return Button::Middle;
    }
}

- (void)mouseDown:(NSEvent *)ev {
    [self button:Button::Left down:true event:ev];
}
- (void)mouseUp:(NSEvent *)ev {
    [self button:Button::Left down:false event:ev];
}
- (void)rightMouseDown:(NSEvent *)ev {
    [self button:Button::Right down:true event:ev];
}
- (void)rightMouseUp:(NSEvent *)ev {
    [self button:Button::Right down:false event:ev];
}
- (void)otherMouseDown:(NSEvent *)ev {
    bool         ok;
    const Button b = [self otherButton:ev ok:&ok];
    if (ok)
        [self button:b down:true event:ev];
}
- (void)otherMouseUp:(NSEvent *)ev {
    bool         ok;
    const Button b = [self otherButton:ev ok:&ok];
    if (ok)
        [self button:b down:false event:ev];
}

- (void)scrollWheel:(NSEvent *)ev {
    if (!_owner)
        return;
    Event e{.type = EventType::Scroll, .pos = _owner->viewPoint(ev)};
    // AppKit's deltas are "content moves by" (+y = towards the top, i.e.
    // scrolling up); plat's +dy is "scroll down", so both axes flip. The
    // user's natural-scrolling choice is already applied to the deltas.
    if (ev.hasPreciseScrollingDeltas) {
        e.precise = true;
        e.dx      = 0.0 - ev.scrollingDeltaX; // not -x: no -0 on Begin/End
        e.dy      = 0.0 - ev.scrollingDeltaY;
    } else {
        e.dx = -ev.deltaX; // notches (AppKit may accelerate them past ±1)
        e.dy = -ev.deltaY;
    }
    // Touchpad phases. A two-finger scroll is Begin, Update…, End when the
    // fingers lift; if the flick coasts, AppKit follows with a momentum
    // sequence, which plat reports as Momentum… closed by a second End. So
    // End always means "no more motion for now", and a Momentum after an End
    // means coasting resumed it. MayBegin (fingers resting, not moving yet)
    // and Stationary carry nothing and are dropped.
    const NSEventPhase p = ev.phase, m = ev.momentumPhase;
    if (m != NSEventPhaseNone) {
        e.phase = (m & (NSEventPhaseEnded | NSEventPhaseCancelled)) ? plat::ScrollPhase::End
                                                                    : plat::ScrollPhase::Momentum;
    } else if (p & NSEventPhaseBegan) {
        e.phase = plat::ScrollPhase::Begin;
    } else if (p & NSEventPhaseChanged) {
        e.phase = plat::ScrollPhase::Update;
    } else if (p & (NSEventPhaseEnded | NSEventPhaseCancelled)) {
        e.phase = plat::ScrollPhase::End;
    } else if (p != NSEventPhaseNone) {
        return; // MayBegin / Stationary
    }
    const bool bracket = e.phase == plat::ScrollPhase::Begin || e.phase == plat::ScrollPhase::End;
    if (e.dx == 0 && e.dy == 0 && !bracket)
        return; // phase bookkeeping without motion
    e.mods = plat::cocoa::modsFromFlags(ev.modifierFlags);
    _owner->emit(e);
}

// ── touchpad gestures ───────────────────────────────────────────────────────

// The one-shot navigation swipe (three fingers, or two with "Swipe between
// pages"). AppKit's deltaX is +1 when the fingers moved right, which is
// "back" — the SwipeGesture contract, so the signs pass straight through.
- (void)swipeWithEvent:(NSEvent *)ev {
    if (!_owner)
        return;
    const auto sign = [](CGFloat v) { return v > 0 ? 1.0 : v < 0 ? -1.0 : 0.0; };
    _owner->emit(
        {.type    = EventType::SwipeGesture,
         .pos     = _owner->viewPoint(ev),
         .dx      = sign(ev.deltaX),
         .dy      = sign(ev.deltaY),
         .mods    = plat::cocoa::modsFromFlags(ev.modifierFlags),
         .gesture = plat::Gesture::Swipe,
         .fingers = 3}
    );
}

// ── keys ────────────────────────────────────────────────────────────────────

- (Event)keyEvent:(NSEvent *)ev type:(EventType)t {
    return {
        .type     = t,
        .key      = plat::cocoa::keyFromKeyCode(ev.keyCode),
        .scancode = ev.keyCode,
        .mods     = plat::cocoa::modsFromFlags(ev.modifierFlags),
        // -isARepeat throws for FlagsChanged events.
        .repeat   = ev.type == NSEventTypeKeyDown && ev.isARepeat
    };
}

- (void)keyDown:(NSEvent *)ev {
    if (!_owner)
        return;
    const uint16_t vk = ev.keyCode;
    // Command chords never produce text (Cmd+C is a shortcut, not 'c').
    const bool     wantText =
        _owner->textInput.enabled && !(ev.modifierFlags & NSEventModifierFlagCommand);
    if (!wantText) {
        if (vk < 128)
            _keyDownSent[vk] = true;
        _owner->emit([self keyEvent:ev type:EventType::KeyDown]);
        return;
    }
    // Run the key through the input method first, buffering what it commits,
    // so the order stays KeyDown → TextInput. A key the IME is composing with
    // (dead key, Kana, Pinyin, the Enter that commits a candidate) is the
    // IME's, not the app's: it gets no KeyDown, or Enter would also send.
    const bool hadMarked = _hasMarked;
    const uint64_t generation       = _documentGeneration;
    const auto     originalDocument = _owner->textInput.document;
    _inKeyDown       = true;
    _imeTouched      = false;
    _commandCalled   = false;
    _commandSelector = nil;
    _pendingEvents.clear();
    [self interpretKeyEvents:@[ ev ]];
    _inKeyDown = false;
    if (_commandCalled && _commandSelector == @selector(cancelOperation:)) {
        if (_hasMarked) {
            Event cancelEvent{.type = EventType::TextPreedit, .compositionEnd = plat::CompositionEnd::Cancel};
            _hasMarked = false;
            _markedText.clear();
            [self.inputContext discardMarkedText];
            if (_owner)
                _owner->emit(cancelEvent);
        }
    } else if (!_imeTouched && (!hadMarked || _pendingEvents.empty())) {
        if (vk < 128)
            _keyDownSent[vk] = true;
        _expectedDocument = originalDocument;
        _checkingDocument = true;
        _owner->emit([self keyEvent:ev type:EventType::KeyDown]);
        _checkingDocument = false;
    }
    auto pending = std::move(_pendingEvents);
    _pendingEvents.clear();
    for (auto &pe : pending) {
        if (!_owner || generation != _documentGeneration)
            break;
        _expectedDocument = std::move(pe.document);
        _checkingDocument = true;
        _owner->emit(pe.event);
        _checkingDocument = false;
    }
    _expectedDocument.reset();
}

- (void)keyUp:(NSEvent *)ev {
    if (!_owner)
        return;
    const uint16_t vk = ev.keyCode;
    // Only release what we pressed: an IME-consumed key had no KeyDown.
    if (vk < 128) {
        if (!_keyDownSent[vk])
            return;
        _keyDownSent[vk] = false;
    }
    _owner->emit([self keyEvent:ev type:EventType::KeyUp]);
}

- (void)flagsChanged:(NSEvent *)ev {
    if (!_owner)
        return;
    const Key k = plat::cocoa::keyFromKeyCode(ev.keyCode);
    if (k == Key::Unknown) // Fn
        return;
    const NSEventModifierFlags f = ev.modifierFlags;
    if (k == Key::CapsLock) {
        // The event only reports the new lock state; there is no release of
        // the physical key, so give each toggle a press and a release.
        _owner->emit([self keyEvent:ev type:EventType::KeyDown]);
        _owner->emit([self keyEvent:ev type:EventType::KeyUp]);
        return;
    }
    NSEventModifierFlags dev = 0, any = 0;
    switch (k) {
    case Key::ShiftLeft:
        dev = kDevLShift, any = NSEventModifierFlagShift;
        break;
    case Key::ShiftRight:
        dev = kDevRShift, any = NSEventModifierFlagShift;
        break;
    case Key::ControlLeft:
        dev = kDevLCtrl, any = NSEventModifierFlagControl;
        break;
    case Key::ControlRight:
        dev = kDevRCtrl, any = NSEventModifierFlagControl;
        break;
    case Key::AltLeft:
        dev = kDevLAlt, any = NSEventModifierFlagOption;
        break;
    case Key::AltRight:
        dev = kDevRAlt, any = NSEventModifierFlagOption;
        break;
    case Key::SuperLeft:
        dev = kDevLCmd, any = NSEventModifierFlagCommand;
        break;
    case Key::SuperRight:
        dev = kDevRCmd, any = NSEventModifierFlagCommand;
        break;
    default:
        return;
    }
    // Some event sources leave the device bits clear; fall back to the
    // side-less flag then.
    const bool     devKnown = (f & (kDevLShift | kDevRShift | kDevLCtrl | kDevRCtrl | kDevLAlt |
                                    kDevRAlt | kDevLCmd | kDevRCmd)) != 0 ||
                              !(f & (NSEventModifierFlagShift | NSEventModifierFlagControl |
                                     NSEventModifierFlagOption | NSEventModifierFlagCommand));
    const bool     down     = devKnown ? (f & dev) != 0 : (f & any) != 0;
    const uint16_t vk       = ev.keyCode;
    if (vk < 128)
        _keyDownSent[vk] = down;
    _owner->emit([self keyEvent:ev type:down ? EventType::KeyDown : EventType::KeyUp]);
}

// Key equivalents that no menu item claimed come here before keyDown:; let
// them through to keyDown: so the app sees Cmd+C etc. as KeyDown.
- (BOOL)performKeyEquivalent:(NSEvent *)ev {
    return NO;
}

// ── NSTextInputClient ───────────────────────────────────────────────────────

- (void)updateDocumentState:(const plat::TextInputState &)s {
    if (_checkingDocument && s.document != _expectedDocument)
        ++_documentGeneration; // an authoritative edit invalidates the rest of this key's batch
    if (!s.enabled || !s.document) {
        _hasDoc    = false;
        _hasMarked = false;
        _docText.clear();
        _markedText.clear();
        return;
    }
    _hasDoc    = true;
    _docText   = s.document->text;
    _docAnchor = s.document->anchor;
    _docCaret  = s.document->caret;
    if (s.document->composition) {
        _hasMarked  = true;
        _markedRepl = s.document->composition->replacement;
        _markedText = s.document->composition->text;
        _markedSel  = s.document->composition->selection;
    } else {
        _hasMarked  = false;
        _markedText.clear();
        _markedRepl = {_docCaret, _docCaret};
        _markedSel  = {0, 0};
    }
}

struct EffectiveState {
    NSString   *effStr;
    NSRange     markedRange;
    NSRange     selectedRange;
    NSUInteger  prefixU16Len;
    NSUInteger  markedU16Len;
    uint32_t    cReplBegin;
    uint32_t    cReplEnd;
};

struct NativeReplacement {
    plat::TextRange range;
    std::string     text;
    uint32_t        prefixBytes;
};

// Include the marked span in the transaction, retaining any committed gap to the edit.
static std::optional<NativeReplacement>
replaceEffectiveText(const EffectiveState &st, bool marked, NSRange range, std::string_view text) {
    if (range.location > st.effStr.length || range.length > st.effStr.length - range.location)
        return std::nullopt;
    const NSUInteger end = NSMaxRange(range);
    const NSUInteger begin =
        marked ? std::min(range.location, st.markedRange.location) : range.location;
    const NSUInteger finish = marked ? std::max(end, NSMaxRange(st.markedRange)) : end;
    const NSInteger  a      = utf8OffsetFromUtf16(st.effStr, begin);
    const NSInteger  b      = utf8OffsetFromUtf16(st.effStr, finish);
    const NSInteger  r0     = utf8OffsetFromUtf16(st.effStr, range.location);
    const NSInteger  r1     = utf8OffsetFromUtf16(st.effStr, end);
    if (a < 0 || b < 0 || r0 < 0 || r1 < 0)
        return std::nullopt;
    const std::string effective(st.effStr.UTF8String);
    const uint32_t    c0 = marked && begin == st.markedRange.location ? st.cReplBegin : uint32_t(a);
    const NSInteger   markedEnd =
        marked ? utf8OffsetFromUtf16(st.effStr, NSMaxRange(st.markedRange)) : 0;
    const uint32_t c1 = marked ? st.cReplEnd + uint32_t(b - markedEnd) : uint32_t(b);
    return NativeReplacement{
        {c0, c1},
        effective.substr(a, r0 - a) + std::string(text) + effective.substr(r1, b - r1),
        uint32_t(r0 - a)
    };
}

- (EffectiveState)currentEffectiveState {
    EffectiveState st{};
    st.markedRange   = NSMakeRange(NSNotFound, 0);
    st.selectedRange = NSMakeRange(0, 0);
    if (!_hasDoc) {
        if (_hasMarked && !_markedText.empty()) {
            st.effStr       = [NSString stringWithUTF8String:_markedText.c_str()];
            st.markedRange  = NSMakeRange(0, st.effStr ? st.effStr.length : 0);
            st.markedU16Len = st.markedRange.length;
        }
        return st;
    }
    if (_hasMarked) {
        st.cReplBegin = _markedRepl.begin;
        st.cReplEnd   = _markedRepl.end;
        if (st.cReplBegin > _docText.size())
            st.cReplBegin = uint32_t(_docText.size());
        if (st.cReplEnd < st.cReplBegin || st.cReplEnd > _docText.size())
            st.cReplEnd = uint32_t(_docText.size());
        std::string prefix = _docText.substr(0, st.cReplBegin);
        std::string suffix = _docText.substr(st.cReplEnd);
        std::string total  = prefix + _markedText + suffix;
        st.effStr          = [NSString stringWithUTF8String:total.c_str()];
        if (!st.effStr)
            st.effStr = @"";
        st.prefixU16Len = utf16LengthOfUtf8(prefix);
        st.markedU16Len = utf16LengthOfUtf8(_markedText);
        st.markedRange  = NSMakeRange(st.prefixU16Len, st.markedU16Len);
        NSUInteger selStart = utf16OffsetFromUtf8(_markedText, _markedSel.begin);
        if (selStart == NSNotFound)
            selStart = st.markedU16Len;
        NSUInteger selEnd = utf16OffsetFromUtf8(_markedText, _markedSel.end);
        if (selEnd == NSNotFound || selEnd < selStart)
            selEnd = selStart;
        st.selectedRange = NSMakeRange(st.prefixU16Len + selStart, selEnd - selStart);
    } else {
        st.cReplBegin = _docCaret;
        st.cReplEnd   = _docCaret;
        st.effStr     = [NSString stringWithUTF8String:_docText.c_str()];
        if (!st.effStr)
            st.effStr = @"";
        uint32_t a = std::min(_docAnchor, _docCaret);
        uint32_t b = std::max(_docAnchor, _docCaret);
        NSUInteger u16A = utf16OffsetFromUtf8(_docText, a);
        NSUInteger u16B = utf16OffsetFromUtf8(_docText, b);
        if (u16A != NSNotFound && u16B != NSNotFound)
            st.selectedRange = NSMakeRange(u16A, u16B - u16A);
        else
            st.selectedRange = NSMakeRange(0, 0);
    }
    return st;
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    NSString *rawStr = plainString(string);
    std::string text = filteredText(rawStr);

    if (!_hasDoc) {
        _hasMarked = false;
        _markedText.clear();
        if (text.empty() && replacementRange.location == NSNotFound)
            return;
        Event e{.type = EventType::TextInput, .text = text};
        [self sendTextEvent:std::move(e)];
        return;
    }

    EffectiveState st = [self currentEffectiveState];
    NSRange targetRange = replacementRange;
    if (targetRange.location == NSNotFound)
        targetRange = _hasMarked ? st.markedRange : st.selectedRange;

    auto edit = replaceEffectiveText(st, _hasMarked, targetRange, text);
    if (!edit)
        return;
    const uint32_t cursor = edit->prefixBytes + uint32_t(text.size());
    _docText.replace(edit->range.begin, edit->range.end - edit->range.begin, edit->text);
    _docCaret = _docAnchor = edit->range.begin + cursor;
    _hasMarked = false;
    _markedText.clear();

    Event e{.type = EventType::TextInput, .text = std::move(edit->text)};
    e.textReplacement = edit->range;
    e.textSelection   = plat::TextRange{cursor, cursor};
    [self sendTextEvent:std::move(e)];
}

- (void)setMarkedText:(id)string
        selectedRange:(NSRange)selectedRange
     replacementRange:(NSRange)replacementRange {
    _imeTouched = true;
    NSString *rawStr = plainString(string);
    std::string newText = rawStr.length ? std::string(rawStr.UTF8String) : std::string();

    if (!_hasDoc) {
        _hasMarked  = true;
        _markedText = newText;
        Event e{.type = EventType::TextPreedit, .text = newText};
        if (selectedRange.location != NSNotFound) {
            e.preeditCursorBegin = utf8Offset(rawStr, selectedRange.location);
            e.preeditCursorEnd   = utf8Offset(rawStr, NSMaxRange(selectedRange));
        }
        [self sendTextEvent:std::move(e)];
        return;
    }

    EffectiveState st = [self currentEffectiveState];
    NSRange targetRange = replacementRange;
    if (targetRange.location == NSNotFound)
        targetRange = _hasMarked ? st.markedRange : st.selectedRange;

    auto edit = replaceEffectiveText(st, _hasMarked, targetRange, newText);
    if (!edit)
        return;
    uint32_t curBegin = edit->prefixBytes + uint32_t(newText.size()), curEnd = curBegin;
    if (selectedRange.location != NSNotFound) {
        if (selectedRange.location > rawStr.length ||
            selectedRange.length > rawStr.length - selectedRange.location)
            return;
        NSInteger s0 = utf8OffsetFromUtf16(rawStr, selectedRange.location);
        NSInteger s1 = utf8OffsetFromUtf16(rawStr, NSMaxRange(selectedRange));
        if (s0 < 0 || s1 < 0)
            return;
        curBegin = edit->prefixBytes + uint32_t(s0);
        curEnd   = edit->prefixBytes + uint32_t(s1);
    }
    _hasMarked  = true;
    _markedText = std::move(edit->text);
    _markedRepl = edit->range;
    _markedSel = {curBegin, curEnd};

    Event e{.type = EventType::TextPreedit, .text = _markedText};
    e.textReplacement    = _markedRepl;
    e.preeditCursorBegin = int(curBegin);
    e.preeditCursorEnd   = int(curEnd);
    [self sendTextEvent:std::move(e)];
}

- (void)unmarkText {
    if (!_hasMarked)
        return;
    _imeTouched = true;
    if (_hasDoc) {
        _docText.replace(_markedRepl.begin, _markedRepl.end - _markedRepl.begin, _markedText);
        _docAnchor = _markedRepl.begin + _markedSel.begin;
        _docCaret  = _markedRepl.begin + _markedSel.end;
    }
    _hasMarked  = false;
    Event e{.type = EventType::TextPreedit, .compositionEnd = plat::CompositionEnd::Commit};
    _markedText.clear();
    [self sendTextEvent:std::move(e)];
}

- (void)discardComposition {
    ++_documentGeneration;
    _hasDoc = false;
    _docText.clear();
    _pendingEvents.clear();
    if (!_hasMarked)
        return;
    _hasMarked = false;
    _markedText.clear();
    [self.inputContext discardMarkedText];
    Event e{.type = EventType::TextPreedit, .compositionEnd = plat::CompositionEnd::Cancel};
    if (_owner)
        _owner->emit(e);
}

- (BOOL)hasMarkedText {
    return _hasMarked && !_markedText.empty();
}

- (NSRange)markedRange {
    EffectiveState st = [self currentEffectiveState];
    return st.markedRange;
}

- (NSRange)selectedRange {
    EffectiveState st = [self currentEffectiveState];
    return st.selectedRange;
}

- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText {
    return @[];
}

- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range
                                                actualRange:(NSRangePointer)actualRange {
    if (actualRange)
        *actualRange = NSMakeRange(NSNotFound, 0);
    EffectiveState st = [self currentEffectiveState];
    if (!st.effStr || st.effStr.length == 0)
        return nil;
    if (range.location >= st.effStr.length)
        return nil;
    NSRange actual = NSIntersectionRange(range, NSMakeRange(0, st.effStr.length));
    if (actual.length == 0)
        return nil;
    if (actual.location > 0 && actual.location < st.effStr.length &&
        isSurrogateLow([st.effStr characterAtIndex:actual.location]))
        return nil;
    if (NSMaxRange(actual) > 0 && NSMaxRange(actual) < st.effStr.length &&
        isSurrogateLow([st.effStr characterAtIndex:NSMaxRange(actual)]))
        return nil;
    if (actualRange)
        *actualRange = actual;
    return [[NSAttributedString alloc] initWithString:[st.effStr substringWithRange:actual]];
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    return NSNotFound;
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if (actualRange)
        *actualRange = NSMakeRange(NSNotFound, 0);
    if (!_owner || !self.window)
        return NSZeroRect;
    if (actualRange)
        *actualRange = [self selectedRange];
    const plat::Rect c        = _owner->textInput.caret;
    // The caret is logical px relative to the window = points in our flipped
    // view; the IME wants screen coordinates (bottom-left origin).
    const NSRect     inView   = NSMakeRect(c.x, c.y, std::max(c.w, 1), std::max(c.h, 1));
    const NSRect     inWindow = [self convertRect:inView toView:nil];
    return [self.window convertRectToScreen:inWindow];
}

- (void)doCommandBySelector:(SEL)selector {
    _commandCalled   = true;
    _commandSelector = selector;
}

// ── drag and drop: target ───────────────────────────────────────────────────
// Registered types decide which drags AppKit shows us at all (it matches
// exact types, not conformance): the standard plat types, the common
// native ones they come from, and generic data so plat-to-plat drags of
// custom (dynamic-UTI) types still get DropEnter.

+ (NSArray<NSPasteboardType> *)dropTypes {
    return @[
        NSPasteboardTypeFileURL,
        NSPasteboardTypeURL,
        NSPasteboardTypeString,
        NSPasteboardTypeHTML,
        NSPasteboardTypePNG,
        NSPasteboardTypeTIFF,
        NSPasteboardTypeRTF,
        @"public.data",
        @"public.item",
    ];
}

- (Event)dropEvent:(id<NSDraggingInfo>)info type:(EventType)t withData:(bool)withData {
    const NSPoint p = [self convertPoint:info.draggingLocation fromView:nil];
    Event         e{.type = t, .pos = {p.x, p.y}};
    e.allowedActions    = plat::cocoa::dropActionsFromOperation(info.draggingSourceOperationMask);
    e.dropAction        = plat::core::preferredAction(e.allowedActions);
    NSPasteboard   *pb  = info.draggingPasteboard;
    const NSInteger seq = info.draggingSequenceNumber;
    const NSInteger change = pb.changeCount;
    if (!_dropKnown || seq != _dropSeq || change != _dropChange) {
        _dropMimes  = plat::cocoa::pasteboardMimes(pb);
        _dropSeq    = seq;
        _dropChange = change;
        _dropKnown  = true;
    }
    // Read once on Drop: the uri-list item and e.uris, the text item and
    // e.text are the same reads.
    if (withData)
        e.uris = plat::cocoa::pasteboardUris(pb);
    std::optional<std::string> text;
    bool                       textRead = false;
    for (const auto &m : _dropMimes) {
        plat::DataItem item{m, {}};
        // Data only on Drop, and only for types that are cheap and meant
        // for us (not file promises or the TIFF a PNG drag also offers, say).
        if (withData && plat::core::readOnDrop(m)) {
            if (m == "text/uri-list") {
                for (const auto &u : e.uris)
                    item.data += u + "\r\n";
            } else {
                std::optional<std::string> d = plat::cocoa::readPasteboard(pb, m);
                if (m == plat::core::kTextMime) {
                    text     = d;
                    textRead = true;
                }
                item.data = std::move(d).value_or(std::string());
            }
        }
        e.items.push_back(std::move(item));
    }
    if (withData) {
        if (!textRead)
            if (NSString *s = [pb stringForType:NSPasteboardTypeString])
                text = std::string(s.UTF8String);
        e.text = std::move(text).value_or(std::string());
    }
    return e;
}

// The app's reply as the operation AppKit shows (cursor badge) and hands
// the source; None — or an action the source does not allow — rejects, and
// then performDragOperation: (our Drop) is never called.
- (NSDragOperation)dropReply:(id<NSDraggingInfo>)info {
    if (!_owner)
        return NSDragOperationNone;
    const NSDragOperation op = plat::cocoa::operationFromAction(_owner->dropReply);
    return (op & info.draggingSourceOperationMask) ? op : NSDragOperationNone;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
    if (!_owner)
        return NSDragOperationNone;
    Event e           = [self dropEvent:info type:EventType::DropEnter withData:false];
    // Default answer until the app says otherwise: what the source prefers.
    _owner->dropReply = e.dropAction;
    _owner->emit(std::move(e));
    return [self dropReply:info];
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
    if (!_owner)
        return NSDragOperationNone;
    _owner->emit([self dropEvent:info type:EventType::DropMove withData:false]);
    return [self dropReply:info];
}
- (void)draggingExited:(id<NSDraggingInfo>)info {
    if (_owner)
        _owner->emit({.type = EventType::DropLeave});
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    if (!_owner)
        return NO;
    Event e      = [self dropEvent:info type:EventType::Drop withData:true];
    e.dropAction = _owner->dropReply; // what the target agreed to
    _owner->emit(std::move(e));
    return YES;
}

// ── drag and drop: source ───────────────────────────────────────────────────

- (NSDragOperation)draggingSession:(NSDraggingSession *)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    return _owner ? plat::cocoa::operationFromActions(_owner->dragActions) : NSDragOperationNone;
}

- (void)draggingSession:(NSDraggingSession *)session
           endedAtPoint:(NSPoint)screenPoint
              operation:(NSDragOperation)operation {
    if (!_owner || !_owner->dragging)
        return;
    _owner->dragging       = false;
    // AppKit consumed the release; nothing of this press is still held.
    _owner->lastMouseEvent = nil;
    _owner->emit(
        {.type       = EventType::DragFinished,
         .dropAction = plat::cocoa::dropActionFromOperation(operation)}
    );
    _owner->app->noteWork();
}

@end

// ── NSWindow subclass: the title-bar zone of Custom windows ─────────────────
// With NSWindowStyleMaskFullSizeContentView our view extends under the
// (transparent) title bar, but AppKit's titlebar view still sits on top and
// takes every press in the top ~28 pt for its own drag/zoom handling — the
// app's hit-test callback and its own buttons there would never see them.
// Route those events to PlatView instead (it applies the hit test and does
// the native drag itself); only the traffic lights keep theirs.
@interface                                PlatNSWindow : NSWindow
@property(nonatomic, assign) CocoaWindow *owner;
@end

@implementation PlatNSWindow {
    NSUInteger _captured; // buttons whose press we took from the title bar
}

- (BOOL)titlebarWouldSteal:(NSEvent *)ev {
    NSView *content = self.contentView;
    NSView *frame   = content.superview; // theme frame: hitTest: takes window coords
    NSView *hit     = [frame hitTest:ev.locationInWindow];
    if (!hit || hit == content || [hit isDescendantOf:content])
        return NO;
    for (NSView *v = hit; v; v = v.superview)
        if ([v isKindOfClass:[NSControl class]]) // close / minimise / zoom
            return NO;
    const NSPoint p = [content convertPoint:ev.locationInWindow fromView:nil];
    return [content mouse:p inRect:content.bounds];
}

- (void)routeToView:(NSEvent *)ev {
    NSView *v = self.contentView;
    switch (ev.type) {
    case NSEventTypeLeftMouseDown:
        [v mouseDown:ev];
        break;
    case NSEventTypeLeftMouseUp:
        [v mouseUp:ev];
        break;
    case NSEventTypeLeftMouseDragged:
        [v mouseDragged:ev];
        break;
    case NSEventTypeRightMouseDown:
        [v rightMouseDown:ev];
        break;
    case NSEventTypeRightMouseUp:
        [v rightMouseUp:ev];
        break;
    case NSEventTypeRightMouseDragged:
        [v rightMouseDragged:ev];
        break;
    case NSEventTypeOtherMouseDown:
        [v otherMouseDown:ev];
        break;
    case NSEventTypeOtherMouseUp:
        [v otherMouseUp:ev];
        break;
    case NSEventTypeOtherMouseDragged:
        [v otherMouseDragged:ev];
        break;
    case NSEventTypeScrollWheel:
        [v scrollWheel:ev];
        break;
    default:
        break;
    }
}

- (void)sendEvent:(NSEvent *)ev {
    if (!_owner || _owner->decorations != plat::Decorations::Custom) {
        [super sendEvent:ev];
        return;
    }
    const NSUInteger bit = NSUInteger(1) << std::min<NSInteger>(ev.buttonNumber, 31);
    switch (ev.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeRightMouseDown:
    case NSEventTypeOtherMouseDown:
        if ([self titlebarWouldSteal:ev]) {
            // Bypassing super also bypasses its click-to-focus.
            if (!self.isKeyWindow && self.canBecomeKeyWindow)
                [self makeKeyAndOrderFront:nil];
            _captured |= bit;
            [self routeToView:ev];
            return;
        }
        break;
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
        if (_captured & bit) { // AppKit would send these to the titlebar view
            [self routeToView:ev];
            return;
        }
        break;
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseUp:
        if (_captured & bit) {
            _captured &= ~bit;
            [self routeToView:ev];
            return;
        }
        break;
    case NSEventTypeScrollWheel:
        if ([self titlebarWouldSteal:ev]) {
            [self routeToView:ev];
            return;
        }
        break;
    default:
        break;
    }
    [super sendEvent:ev];
}
@end

// ── Window delegate ─────────────────────────────────────────────────────────
@interface                                PlatWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) CocoaWindow *owner;
@property(nonatomic, strong) NSToolbar   *parkedToolbar; // off while in full screen
@end

@implementation PlatWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    // The app decides; the window only goes away when it is destroyed.
    if (_owner)
        _owner->emit({.type = EventType::CloseRequested});
    return NO;
}
- (void)windowDidBecomeKey:(NSNotification *)n {
    if (!_owner)
        return;
    _owner->emit({.type = EventType::FocusIn});
    _owner->emit({.type = EventType::StateChanged});
}
- (void)windowDidResignKey:(NSNotification *)n {
    if (!_owner)
        return;
    _owner->emit({.type = EventType::FocusOut});
    _owner->emit({.type = EventType::StateChanged});
}
- (void)windowDidMiniaturize:(NSNotification *)n {
    if (_owner)
        _owner->stateMaybeChanged();
}
- (void)windowDidDeminiaturize:(NSNotification *)n {
    if (!_owner)
        return;
    _owner->stateMaybeChanged();
    _owner->requestFrame();
}
// A window with a toolbar keeps it (and the title bar) on screen in full
// screen, an opaque band over the content; asking for an auto-hidden toolbar
// instead leaves it stuck off screen on macOS 15. The toolbar is empty (it
// only places the traffic lights), so full screen goes without it: the bare
// title bar slides in with the menu bar like any other app's. Every way out
// of a transition puts it back on a windowed window: a failed entry (AppKit
// refuses one, e.g. while the screen is locked) would otherwise leave the
// traffic lights in a bare title bar for good, and a failed exit parks it
// again.
- (void)parkToolbar:(NSWindow *)w {
    if (w.toolbar) {
        _parkedToolbar = w.toolbar;
        w.toolbar      = nil;
    }
}
- (void)restoreToolbar:(NSWindow *)w {
    if (_parkedToolbar) {
        w.toolbar      = _parkedToolbar;
        _parkedToolbar = nil;
    }
}
- (void)windowWillEnterFullScreen:(NSNotification *)n {
    [self parkToolbar:n.object];
}
- (void)windowDidFailToEnterFullScreen:(NSWindow *)w {
    [self restoreToolbar:w];
    if (_owner)
        _owner->stateMaybeChanged();
}
- (void)windowWillExitFullScreen:(NSNotification *)n {
    [self restoreToolbar:n.object];
}
- (void)windowDidFailToExitFullScreen:(NSWindow *)w {
    [self parkToolbar:w];
    if (_owner)
        _owner->stateMaybeChanged();
}
- (void)windowDidEnterFullScreen:(NSNotification *)n {
    [self parkToolbar:n.object]; // a no-op after the will-; a net if none came
    if (_owner)
        _owner->stateMaybeChanged();
}
- (void)windowDidExitFullScreen:(NSNotification *)n {
    [self restoreToolbar:n.object]; // likewise
    if (_owner)
        _owner->stateMaybeChanged();
}
- (void)windowDidMove:(NSNotification *)n {
    if (_owner)
        _owner->moved();
}
- (void)windowDidResize:(NSNotification *)n {
    if (_owner)
        _owner->stateMaybeChanged(); // zoom has no notification of its own
}
- (void)windowDidChangeScreen:(NSNotification *)n {
    if (_owner) {
        _owner->recreateDisplayLink();
        _owner->requestFrame();
    }
}
- (void)windowDidChangeScreenProfile:(NSNotification *)n {
    if (_owner) {
        _owner->recreateDisplayLink();
        _owner->requestFrame();
    }
}
- (void)windowDidChangeOcclusionState:(NSNotification *)n {
    // Display links stop while occluded; a Frame asked for meanwhile is
    // owed now.
    if (_owner && (_owner->window.occlusionState & NSWindowOcclusionStateVisible)) {
        _owner->recreateDisplayLink();
        if (_owner->framePending)
            _owner->requestFrame();
    }
}
@end

// ── CocoaWindow ─────────────────────────────────────────────────────────────
namespace plat::cocoa {

CocoaWindow::CocoaWindow(CocoaApp *a, const WindowDesc &d) : app(a), decorations(d.decorations) {
    NSWindowStyleMask style =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (d.resizable)
        style |= NSWindowStyleMaskResizable;
    if (d.decorations == Decorations::Custom)
        style |= NSWindowStyleMaskFullSizeContentView;

    PlatNSWindow *pw =
        [[PlatNSWindow alloc] initWithContentRect:NSMakeRect(0, 0, d.size.w, d.size.h)
                                        styleMask:style
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
    pw.owner                  = this;
    window                    = pw;
    window.releasedWhenClosed = NO; // ARC owns it
    window.tabbingMode        = NSWindowTabbingModeDisallowed;
    window.restorable         = NO; // nothing to restore; see CocoaApp()
    // sRGB backing: plat colours are sRGB, and it keeps readback exact.
    window.colorSpace         = NSColorSpace.sRGBColorSpace;
    if (d.decorations == Decorations::Custom) {
        // The app draws the title bar; the traffic lights stay native (they
        // float over our content, top-left).
        window.titlebarAppearsTransparent = YES;
        window.titleVisibility            = NSWindowTitleHidden;
        // An empty toolbar in the compact unified style gives the traffic
        // lights the standard, centred placement of a unified macOS header
        // (Finder's) instead of a bare title bar's;
        // titleBarHeight() reports the band they sit in. AppKit owns the
        // buttons and their behaviour; PlatNSWindow routes the presses on the
        // rest of the band (the toolbar's own views included) to PlatView.
        NSToolbar *toolbar                = [[NSToolbar alloc] initWithIdentifier:@"plat.titlebar"];
        toolbar.allowsUserCustomization   = NO;
        window.toolbar                    = toolbar;
        window.toolbarStyle               = NSWindowToolbarStyleUnifiedCompact;
        window.titlebarSeparatorStyle     = NSTitlebarSeparatorStyleNone;
    }
    window.title = [NSString stringWithUTF8String:d.title.c_str()] ?: @"";
    if (d.minSize.w > 0 || d.minSize.h > 0)
        window.contentMinSize = NSMakeSize(d.minSize.w, d.minSize.h);

    delegate        = [PlatWindowDelegate new];
    delegate.owner  = this;
    window.delegate = delegate;

    view               = [[PlatView alloc] initWithFrame:NSMakeRect(0, 0, d.size.w, d.size.h)];
    window.contentView = view;
    view.owner         = this; // after contentView: its setFrameSize must not emit yet
    [window makeFirstResponder:view];
    [window center];
    if (d.position)
        setPosition(*d.position);

    _lastSize                = size();
    _lastScale               = window.backingScaleFactor;
    view.layer.contentsScale = _lastScale;

    _frameTarget       = [PlatFrameTarget new];
    _frameTarget.owner = this;
    if (@available(macOS 14.0, *)) {
        CADisplayLink *link = [view displayLinkWithTarget:_frameTarget selector:@selector(tick:)];
        // Common modes: keeps ticking inside live-resize and menu tracking.
        [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        link.paused  = YES;
        _displayLink = link;
    }

    if (d.visible)
        show();
}

CocoaWindow::~CocoaWindow() {
    if (_cursorHidden)
        [NSCursor unhide];
    if (@available(macOS 14.0, *))
        [(CADisplayLink *)_displayLink invalidate];
    _frameTarget.owner             = nullptr;
    view.owner                     = nullptr;
    delegate.owner                 = nullptr;
    ((PlatNSWindow *)window).owner = nullptr;
    window.delegate                = nil;
    [window orderOut:nil];
    [window close];
    if (_painting >= 0)
        IOSurfaceUnlock(_surfaces[_painting].surface, 0, nullptr);
    for (CocoaSurface &c : _surfaces)
        if (c.surface)
            CFRelease(c.surface);
    app->forget(this);
}

void CocoaWindow::emit(Event e) {
    e.window = this;
    app->emitEvent(e);
}

Point CocoaWindow::viewPoint(NSEvent *ev) const {
    NSPoint p = ev.locationInWindow;
    // Synthesised events without a window carry screen coordinates.
    if (ev.window != window)
        p = [window convertPointFromScreen:p];
    p = [view convertPoint:p fromView:nil];
    return {p.x, p.y};
}

void CocoaWindow::setTitle(std::string_view t) {
    window.title = plat::cocoa::nsString(t);
}

Size CocoaWindow::size() const {
    const NSSize s = view.bounds.size;
    return {int(std::lround(s.width)), int(std::lround(s.height))};
}

double CocoaWindow::scale() const {
    return window.backingScaleFactor;
}

void CocoaWindow::setSize(Size s) {
    [window setContentSize:NSMakeSize(s.w, s.h)];
}

void CocoaWindow::setMinSize(Size s) {
    window.contentMinSize = NSMakeSize(s.w, s.h);
}

void CocoaWindow::show() {
    [window makeKeyAndOrderFront:nil];
    requestFrame();
}

void CocoaWindow::hide() {
    [window orderOut:nil];
}

void CocoaWindow::minimize() {
    [window miniaturize:nil];
}

void CocoaWindow::setMaximized(bool on) {
    // macOS has no maximised state; "zoom" to the best size is the equivalent
    // (and what the green button's Option-click and a title double-click do).
    if (window.isZoomed != on)
        [window zoom:nil];
}

void CocoaWindow::setFullscreen(bool on) {
    if (isFullscreen() != on)
        [window toggleFullScreen:nil];
}

void CocoaWindow::setAlwaysOnTop(bool on) {
    window.level = on ? NSFloatingWindowLevel : NSNormalWindowLevel;
}

bool CocoaWindow::isAlwaysOnTop() const {
    return window.level >= NSFloatingWindowLevel;
}

void CocoaWindow::setDarkChrome(bool dark) {
    // Left to the system, a dark app on a light-mode Mac gets the Aqua
    // title bar: a bright rim along the top edge, light traffic lights.
    window.appearance =
        [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
}

double CocoaWindow::titleBarHeight() const {
    if (decorations != Decorations::Custom)
        return 0;
    // With the full-size content view the content spans the frame; the
    // layout rect is what the title bar (and toolbar) leave of it.
    return std::max(0.0, NSHeight(window.frame) - NSHeight(window.contentLayoutRect));
}

bool CocoaWindow::isMaximized() const {
    // In full screen -isZoomed is a synchronous window-server round trip
    // (~100 ms during the transition), and the answer is no anyway.
    return !isFullscreen() && window.isZoomed;
}
bool CocoaWindow::isFullscreen() const {
    return (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
}
bool CocoaWindow::isMinimized() const {
    return window.isMiniaturized;
}
bool CocoaWindow::isActive() const {
    return window.isKeyWindow;
}

void CocoaWindow::activate() {
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        [NSApp activateIgnoringOtherApps:YES];
    [window makeKeyAndOrderFront:nil];
}

// The content view's rect in the flipped virtual desktop: for Custom
// decorations (full-size content view) that includes the title-bar strip,
// the same area the app paints and hit-tests.
std::optional<Point> CocoaWindow::position() const {
    const NSRect r = [window convertRectToScreen:[view convertRect:view.bounds toView:nil]];
    const Rect   f = flipRect(r);
    return Point{double(f.x), double(f.y)};
}

bool CocoaWindow::setPosition(Point p) {
    // Move the frame by however far the content is from where it should be;
    // setFrameOrigin: does not constrain to the screen (only user drags and
    // -constrainFrameRect: do), so the window lands exactly there.
    const NSRect cur    = [window convertRectToScreen:[view convertRect:view.bounds toView:nil]];
    const NSRect target = unflipRect(
        {int(std::lround(p.x)),
         int(std::lround(p.y)),
         int(std::lround(cur.size.width)),
         int(std::lround(cur.size.height))}
    );
    NSPoint o = window.frame.origin;
    o.x += target.origin.x - cur.origin.x;
    o.y += target.origin.y - cur.origin.y;
    [window setFrameOrigin:o];
    return true;
}

uint64_t CocoaWindow::monitor() const {
    return screenId(window.screen); // nil (0) while entirely off-screen
}

void CocoaWindow::moved() {
    emit({.type = EventType::Moved});
}

void CocoaWindow::stateMaybeChanged() {
    const bool f = isFullscreen(), z = isMaximized(), m = window.isMiniaturized;
    if (z == _lastZoomed && m == _lastMini && f == _lastFull)
        return;
    _lastZoomed = z;
    _lastMini   = m;
    _lastFull   = f;
    emit({.type = EventType::StateChanged});
}

// ── cursors ─────────────────────────────────────────────────────────────────

namespace {
NSCursor *nsCursor(Cursor c) {
    if (@available(macOS 15.0, *)) {
        // The real window-edge resize cursors (public since macOS 15).
        auto frame = [](NSCursorFrameResizePosition p) {
            return [NSCursor frameResizeCursorFromPosition:p
                                              inDirections:NSCursorFrameResizeDirectionsAll];
        };
        switch (c) {
        case Cursor::ResizeH:
            return frame(NSCursorFrameResizePositionRight);
        case Cursor::ResizeV:
            return frame(NSCursorFrameResizePositionBottom);
        case Cursor::ResizeNWSE:
            return frame(NSCursorFrameResizePositionBottomRight);
        case Cursor::ResizeNESW:
            return frame(NSCursorFrameResizePositionBottomLeft);
        default:
            break;
        }
    }
    switch (c) {
    case Cursor::IBeam:
        return NSCursor.IBeamCursor;
    case Cursor::Hand:
        return NSCursor.pointingHandCursor;
    case Cursor::Crosshair:
        return NSCursor.crosshairCursor;
    case Cursor::NotAllowed:
        return NSCursor.operationNotAllowedCursor;
    case Cursor::Move:
    case Cursor::Grab:
        return NSCursor.openHandCursor;
    case Cursor::Grabbing:
        return NSCursor.closedHandCursor;
    case Cursor::ResizeH:
        return NSCursor.resizeLeftRightCursor;
    case Cursor::ResizeV:
        return NSCursor.resizeUpDownCursor;
    // No public diagonal-resize, busy or spinning cursor before macOS 15
    // (the beach ball is the system's, shown when the app stops pumping).
    default:
        return NSCursor.arrowCursor;
    }
}
} // namespace

void CocoaWindow::setCursor(Cursor c) {
    _cursor = c;
    if (pointerInside)
        applyCursor();
}

void CocoaWindow::applyCursor() {
    const bool hide = _cursor == Cursor::Hidden;
    // [NSCursor hide] nests, so keep exactly one outstanding hide per window.
    if (hide != _cursorHidden) {
        _cursorHidden = hide;
        if (hide)
            [NSCursor hide];
        else
            [NSCursor unhide];
    }
    if (!hide)
        [nsCursor(_cursor) set];
}

void CocoaWindow::pointerLeft() {
    pointerInside = false;
    // A hidden cursor is ours only while it is over us.
    if (_cursorHidden) {
        _cursorHidden = false;
        [NSCursor unhide];
    }
    emit({.type = EventType::PointerLeave});
}

// ── text input ──────────────────────────────────────────────────────────────

void CocoaWindow::setTextInput(const TextInputState &s) {
    const bool caretMoved = std::memcmp(&s.caret, &textInput.caret, sizeof s.caret) != 0;
    textInput             = s;
    if (!s.enabled)
        [view discardComposition];
    else {
        [view updateDocumentState:s];
        if (caretMoved)
            [view.inputContext invalidateCharacterCoordinates]; // move the candidate window
    }
}

// ── painting ────────────────────────────────────────────────────────────────

void CocoaWindow::viewResized() {
    const Size s = size();
    if (s.w == _lastSize.w && s.h == _lastSize.h)
        return;
    _lastSize = s;
    emit({.type = EventType::Resized});
    if (view.inLiveResize) {
        // AppKit shows the frame as soon as this returns; paint now so the
        // user drags a window with content, not a stale top-left image.
        framePending = false;
        emit({.type = EventType::Frame});
    } else {
        requestFrame();
    }
}

void CocoaWindow::backingChanged() {
    const double s = window.backingScaleFactor;
    if (s == _lastScale)
        return;
    _lastScale               = s;
    view.layer.contentsScale = s;
    emit({.type = EventType::Resized});
    requestFrame();
}

void CocoaWindow::requestFrame() {
    if (!framePending) {
        framePending     = true;
        frameRequestedAt = CFAbsoluteTimeGetCurrent();
    }
    startFrameClock();
}

void CocoaWindow::recreateDisplayLink() {
    if (@available(macOS 14.0, *)) {
        if (_displayLink) {
            [(CADisplayLink *)_displayLink invalidate];
            _displayLink = nil;
        }
        if (view && _frameTarget) {
            CADisplayLink *link = [view displayLinkWithTarget:_frameTarget selector:@selector(tick:)];
            if (link) {
                [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
                link.paused  = !framePending;
                _displayLink = link;
            }
        }
    }
}

void CocoaWindow::startFrameClock() {
    if (@available(macOS 14.0, *)) {
        if (_displayLink)
            ((CADisplayLink *)_displayLink).paused = NO;
        else
            recreateDisplayLink();
    }
    // The display link does not tick for a window that is not on a live
    // display (screen asleep, locked session) or before macOS 14; the
    // fallback timer covers those so a requested Frame always arrives.
    app->scheduleFrameFallback(frameRequestedAt + kFrameFallback);
}

void CocoaWindow::deliverFrameIfDue() {
    if (@available(macOS 14.0, *))
        ((CADisplayLink *)_displayLink).paused = YES; // until the next requestFrame
    if (!framePending || !window.isVisible)
        return;
    framePending = false;
    emit({.type = EventType::Frame});
    app->noteWork();
}

namespace {

constexpr uint32_t kOpaqueBlack = 0xff000000;

plat::Rect clipTo(plat::Rect r, int w, int h) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(w, r.x + r.w), y1 = std::min(h, r.y + r.h);
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

// BGRA, premultiplied: plat's ARGB32 in little-endian memory order.
IOSurfaceRef createSurface(int w, int h) {
    const size_t  bpr   = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, size_t(w) * 4);
    NSDictionary *props = @{
        (__bridge NSString *)kIOSurfaceWidth : @(w),
        (__bridge NSString *)kIOSurfaceHeight : @(h),
        (__bridge NSString *)kIOSurfaceBytesPerElement : @4,
        (__bridge NSString *)kIOSurfaceBytesPerRow : @(bpr),
        (__bridge NSString *)kIOSurfaceAllocSize : @(bpr * size_t(h)),
        (__bridge NSString *)kIOSurfacePixelFormat : @(uint32_t('BGRA')),
    };
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    // Untagged surfaces are shown as display-native colour; the canvas is sRGB.
    if (s)
        IOSurfaceSetValue(s, CFSTR("IOSurfaceColorSpace"), kCGColorSpaceSRGB);
    return s;
}

uint32_t *rowAt(IOSurfaceRef s, int y) {
    return reinterpret_cast<uint32_t *>(
        static_cast<uint8_t *>(IOSurfaceGetBaseAddress(s)) + size_t(y) * IOSurfaceGetBytesPerRow(s)
    );
}

void copyRect(IOSurfaceRef dst, IOSurfaceRef src, plat::Rect r) {
    for (int y = r.y; y < r.y + r.h; ++y)
        std::memcpy(rowAt(dst, y) + r.x, rowAt(src, y) + r.x, size_t(r.w) * 4);
}

} // namespace

// Painting goes straight into an IOSurface that the layer then shows, so a
// frame copies only what other frames changed since this surface was last
// presented, not the whole window. The surface on screen is never written:
// each frame paints into another one the window server no longer reads.
Canvas CocoaWindow::beginPaint() {
    const double s = scale();
    const Size   l = size();
    const int    w = std::max(1, int(std::lround(l.w * s))),
                 h = std::max(1, int(std::lround(l.h * s)));
    if (_painting < 0) {
        // Drop surfaces of another size (the layer keeps its own reference to
        // the one it shows) and idle ones beyond a double-buffered pair.
        int kept = 0;
        for (int i = 0; i < kMaxSurfaces; ++i) {
            CocoaSurface &c = _surfaces[i];
            if (!c.surface)
                continue;
            if (i != _front &&
                (c.width != w || c.height != h || (kept >= 2 && !IOSurfaceIsInUse(c.surface)))) {
                CFRelease(c.surface);
                c = {};
            } else {
                ++kept;
            }
        }
        // An idle surface of the right size, else a new one, else (every slot
        // taken and still read by the window server) any but the front.
        int pick = -1, empty = -1, busy = -1;
        for (int i = 0; i < kMaxSurfaces; ++i) {
            const CocoaSurface &c = _surfaces[i];
            if (i == _front)
                continue;
            if (!c.surface)
                empty = empty < 0 ? i : empty;
            else if (!IOSurfaceIsInUse(c.surface))
                pick = pick < 0 ? i : pick;
            else
                busy = busy < 0 ? i : busy;
        }
        if (pick < 0 && empty >= 0) {
            IOSurfaceRef n = createSurface(w, h);
            if (!n)
                return {};
            _surfaces[empty] = {n, w, h, true, {}};
            pick             = empty;
        }
        if (pick < 0)
            pick = busy;
        if (pick < 0)
            return {};

        // Bring it up to date with the newest frame, so the canvas always
        // holds what is on screen and a partial repaint is correct.
        CocoaSurface       &p   = _surfaces[pick];
        const CocoaSurface *src = _front >= 0 ? &_surfaces[_front] : nullptr;
        IOSurfaceLock(p.surface, 0, nullptr);
        if (src)
            IOSurfaceLock(src->surface, kIOSurfaceLockReadOnly, nullptr);
        if (p.staleAll) {
            // Keep what fits of the old frame: undamaged pixels must survive.
            const int cw = src ? std::min(w, src->width) : 0;
            const int ch = src ? std::min(h, src->height) : 0;
            for (int y = 0; y < h; ++y) {
                uint32_t *row  = rowAt(p.surface, y);
                const int keep = y < ch ? cw : 0;
                if (keep)
                    std::memcpy(row, rowAt(src->surface, y), size_t(keep) * 4);
                std::fill(row + keep, row + w, kOpaqueBlack);
            }
        } else if (src) {
            for (Rect r : p.stale) {
                r = clipTo(r, w, h);
                if (r.w > 0 && r.h > 0)
                    copyRect(p.surface, src->surface, r);
            }
        }
        if (src)
            IOSurfaceUnlock(src->surface, kIOSurfaceLockReadOnly, nullptr);
        p.stale.clear();
        p.staleAll = false;
        _painting  = pick;
    }
    const CocoaSurface &p = _surfaces[_painting];
    return {
        static_cast<uint32_t *>(IOSurfaceGetBaseAddress(p.surface)),
        p.width,
        p.height,
        int(IOSurfaceGetBytesPerRow(p.surface) / 4),
        s
    };
}

void CocoaWindow::endPaint(const std::vector<Rect> &damage) {
    if (_painting < 0)
        return;
    const int done = _painting;
    _painting      = -1;
    IOSurfaceUnlock(_surfaces[done].surface, 0, nullptr);

    // Other surfaces now lack whatever this frame painted.
    const bool whole = damage.empty();
    for (int i = 0; i < kMaxSurfaces; ++i) {
        CocoaSurface &o = _surfaces[i];
        if (i == done || !o.surface)
            continue;
        if (whole || o.stale.size() + damage.size() > 32) {
            o.staleAll = true;
            o.stale.clear();
        } else if (!o.staleAll) {
            o.stale.insert(o.stale.end(), damage.begin(), damage.end());
        }
    }
    _front = done;

    [CATransaction begin];
    [CATransaction setDisableActions:YES]; // no implicit cross-fade
    view.layer.contentsScale = scale();
    view.layer.contents      = (__bridge id)_surfaces[done].surface;
    [CATransaction commit];
}

// ── attention ───────────────────────────────────────────────────────────────

void CocoaWindow::requestAttention() {
    // Dock bouncing is per app, not per window, and does nothing for the
    // active app; AppKit cancels the request itself when the app activates.
    // Informational = one bounce, not the critical "bounce until noticed"
    // meant for alerts.
    if (window.isKeyWindow || NSApp.isActive)
        return;
    [NSApp requestUserAttention:NSInformationalRequest];
}

// ── drag source ─────────────────────────────────────────────────────────────

bool CocoaApp::startDrag(Window &source, const DragDesc &d) {
    auto    &w  = static_cast<CocoaWindow &>(source);
    NSEvent *ev = w.lastMouseEvent;
    if (!ev || w.dragging || d.items.empty())
        return false; // not inside a press, or a session already runs
    // AppKit's drag loop polls the hardware button state and ends the drag
    // (dropping wherever the pointer is) on its first event once no button
    // is physically down — measured: a session begun from a press the user
    // has already released, or a synthetic one, ends within ~15 ms. There is
    // no drag to run then, so say so instead of reporting a phantom drop.
    if (!(NSEvent.pressedMouseButtons & 1))
        return false;
    @autoreleasepool {
        NSArray<NSPasteboardItem *> *writers = pasteboardItems(d.items);
        // Image::scale physical pixels per point (a 2x drag image stays
        // crisp on Retina); the window's own scale does not enter into it.
        const double                 s       = d.image.scale > 0 ? d.image.scale : 1.0;
        NSImage                     *image   = nil;
        NSSize                       size    = NSMakeSize(32, 32);
        if (!d.image.empty()) {
            size  = NSMakeSize(d.image.width / s, d.image.height / s);
            image = nsImage(d.image, size);
        }
        if (!image) {
            // The OS default: a generic document icon, grabbed at its centre.
            image      = [NSWorkspace.sharedWorkspace iconForContentType:UTTypeItem];
            image.size = size;
        }
        const Point  hot   = d.image.empty() ? Point{size.width / 2, size.height / 2} : d.hotspot;
        const Point  at    = w.viewPoint(ev);
        // PlatView is flipped, so the frame's origin is its top-left corner.
        const NSRect frame = NSMakeRect(at.x - hot.x, at.y - hot.y, size.width, size.height);
        NSMutableArray<NSDraggingItem *> *items = [NSMutableArray array];
        for (NSPasteboardItem *pbi in writers) {
            NSDraggingItem *di = [[NSDraggingItem alloc] initWithPasteboardWriter:pbi];
            // Only the first item shows the app's image; further URIs ride along.
            [di setDraggingFrame:frame contents:items.count == 0 ? image : nil];
            [items addObject:di];
        }
        w.dragActions              = d.actions;
        w.dragging                 = true;
        NSDraggingSession *session = [w.view beginDraggingSessionWithItems:items
                                                                     event:ev
                                                                    source:w.view];
        if (!session) {
            w.dragging = false;
            return false;
        }
        session.animatesToStartingPositionsOnCancelOrFail = YES;
        session.draggingFormation                         = NSDraggingFormationNone;
    }
    return true;
}

} // namespace plat::cocoa
