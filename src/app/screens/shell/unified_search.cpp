#include "screens/shell/unified_search.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "screens/common/avatar_initial.h"
#include "screens/shell/message_search.h"
#include "screens/shell/shell_text.h"
#include "screens/shell/shortcuts.h"

#include <algorithm>

using namespace ui;
using gfx::Icon;
using i18n::tr;
using model::ConvRef;

namespace shell {

namespace {

constexpr float  kListH       = 360; // the list's height
constexpr size_t kGroupedHits = 3;   // messages in the grouped view

// Copied from quick_switcher.cpp (it goes with QuickSwitcher): a name's fuzzy
// score, group DMs half a match behind; none for an id we can't name yet.
std::optional<double> scoreOf(const QuickSwitchName &n, const std::vector<uint32_t> &q) {
    if (n.text.folded.empty())
        return std::nullopt;
    std::optional<double> s = fuzzyScore(q, n.text);
    if (!n.alt.folded.empty())
        if (const std::optional<double> a = fuzzyScore(q, n.alt); a && (!s || *a > *s))
            s = a;
    if (!s)
        return std::nullopt;
    return *s + (n.group ? -0.5 : 0.0);
}

// Copied from quick_switcher.cpp: a workspace tab's 40 px bubble in a slot
// with room for the selection ring; dimmed when it has nothing for the query.
class TabBubble final : public Clickable {
public:
    static constexpr float kBubble = 40, kRadius = 10, kRing = 2, kRingGap = 2;
    static constexpr float kSlot = kBubble + 2 * (kRing + kRingGap);

    TabBubble(std::string name, std::shared_ptr<const gfx::Bitmap> icon)
        : _name(std::move(name)), _icon(std::move(icon)),
          _letter(screens::avatarInitial(_name.empty() ? std::string_view("?") : _name)) {
        style().size(kSlot, kSlot).noShrink();
        setLook({C::None, C::None, C::None, C::None, 0});
        setHoverRepaint(true);
        setCursor(plat::Cursor::Hand);
        setTooltip(_name);
    }
    void set(bool current, bool dimmed) {
        if (current != _current || dimmed != _dimmed) {
            _current = current;
            _dimmed  = dimmed;
            update();
        }
    }
    bool showsBitmap(const gfx::Bitmap *b) const override { return _icon.get() == b; }
    void paint(gfx::Painter &p) override {
        const float in = kRing + kRingGap;
        const RectF r{in, in, kBubble, kBubble};
        p.save();
        if (_dimmed && !_current)
            p.setOpacity(0.35f);
        if (_icon && !_icon->empty()) {
            p.save();
            p.clipRoundRect(r, kRadius);
            p.drawBitmap(_icon->view(), r, gfx::Sampling::Smooth);
            p.restore();
        } else {
            p.fillRoundRect(r, kRadius, color(C::Accent));
            screens::paintInitial(p, *this, r, kRadius, 0, _letter, _layout, 17);
        }
        p.restore();
        if (_current || hovered()) {
            const float out = kRingGap + kRing / 2;
            p.strokeRoundRect(
                {r.x - out, r.y - out, r.w + 2 * out, r.h + 2 * out},
                kRadius + out,
                kRing,
                color(_current ? C::Accent : C::FormDividerStrong)
            );
        }
    }

private:
    std::string                        _name;
    std::shared_ptr<const gfx::Bitmap> _icon;
    std::string                        _letter;
    std::unique_ptr<text::Layout>      _layout;
    bool                               _current = false, _dimmed = false;
};

using Kind = UnifiedSearch::RowKind;

// The group header above a kind's first row.
const char *groupName(Kind k) {
    switch (k) {
    case Kind::Recent:
        return N_("Recent");
    case Kind::Person:
        return N_("People");
    case Kind::Channel:
        return N_("Channels");
    default:
        return N_("Messages");
    }
}

std::string pageName(Sidebar::Nav n) {
    switch (n) {
    case Sidebar::Nav::Threads:
        return tr("Threads");
    case Sidebar::Nav::Saved:
        return tr("Saved messages");
    case Sidebar::Nav::Activity:
        return tr("Activity");
    default:
        return tr("Scheduled messages");
    }
}

Icon pageIcon(Sidebar::Nav n) { // the sidebar's
    switch (n) {
    case Sidebar::Nav::Threads:
        return Icon::Split;
    case Sidebar::Nav::Saved:
        return Icon::Bookmark;
    case Sidebar::Nav::Activity:
        return Icon::Bell;
    default:
        return Icon::Clock;
    }
}

} // namespace

// The rows, virtual: a header above each group's first row, then the row
// (an avatar or icon, the text, and for a message its place and time).
class UnifiedSearch::Adapter final : public VirtualList::Adapter {
public:
    explicit Adapter(UnifiedSearch &s) : _s(s) {}
    class Item final : public View {
    public:
        Item() {
            style().column();
            header = add<Label>("", Font::Small, C::TextMuted);
            header->style().padding(10, 8, 10, 4);
            line = add<Clickable>();
            line->setLook({C::None, C::Hover, C::Pressed, C::Selection, 6});
            line->style().row().padding(10, 6).spacing(10).items(Align::Center);
            line->style().minH = 32;
            avatar             = line->add<Avatar>();
            avatar->style().size(20, 20);
            avatar->setRadius(5);
            icon = line->add<IconView>(Icon::Hash, 16, C::TextMuted);
            icon->style().size(20, 20);
            auto *texts = line->add<View>();
            texts->style().column().flex(1);
            sub = texts->add<Label>("", Font::Small, C::TextMuted);
            sub->setMaxLines(1);
            name = texts->add<Label>("", Font::Body);
            name->setMaxLines(1);
        }
        Label     *header = nullptr, *sub = nullptr, *name = nullptr;
        Clickable *line   = nullptr;
        Avatar    *avatar = nullptr;
        IconView  *icon   = nullptr;
        int        index  = -1;
    };
    int  count() const override { return int(_s._rows.size()); }
    int  kind(int i) const override { return headed(i) ? 1 : 0; }
    bool headed(int i) const {
        const auto &r = _s._rows;
        return i == 0 || groupName(r[size_t(i)].kind) != groupName(r[size_t(i) - 1].kind);
    }
    std::unique_ptr<View> create(int kind) override {
        auto  item = std::make_unique<Item>();
        Item *raw  = item.get();
        raw->header->setVisible(kind == 1);
        raw->line->onClick = [this, raw] { _s.choose(raw->index); };
        return item;
    }
    void bind(View &v, int i) override {
        Item               &it = static_cast<Item &>(v);
        const Row          &r  = _s._rows[size_t(i)];
        const model::Store &st = _s.store();
        it.index               = i;
        it.header->setText(tr(groupName(r.kind)));
        it.name->setText(r.text);
        it.name->setColor(r.kind == Kind::Status ? C::TextMuted : C::Text);
        std::string            sub;
        Icon                   icon = Icon::Search;
        Avatars::Picture       pic;
        std::optional<ConvRef> conv;
        switch (r.kind) {
        case Kind::Recent: {
            const RecentPlace &p = _s._recent[r.index];
            if (p.conv == model::kNoConv)
                icon = pageIcon(p.page);
            else
                conv = p.conv;
            break;
        }
        case Kind::Person:
            pic = _s._avatars.get(st.user(_s._people[r.index].user).avatar, 40);
            break;
        case Kind::Channel:
            conv = _s._channels[r.index];
            break;
        case Kind::Message:
        case Kind::FullResult: {
            const auto &h = _s._hits[r.index];
            sub           = str::concat(
                {searchConvLabel(st, h.conv), "  ", base::formatDateTime(model::tsSecs(h.ts))}
            );
            icon = Icon::MessagesSquare;
            break;
        }
        default:
            break;
        }
        bool person = r.kind == Kind::Person;
        if (conv) { // QuickSwitcher's look: a DM's photo, a group's initial, a channel's # or lock
            const model::Conversation &cv = st.conversation(*conv);
            person                        = cv.isDirect();
            if (cv.kind == model::ConvKind::Dm)
                pic = _s._avatars.get(st.user(cv.dmUser).avatar, 40);
            icon = convIcon(cv);
        }
        it.avatar->setVisible(person);
        it.icon->setVisible(!person && r.kind != Kind::Status);
        if (person) {
            it.avatar->setInitial(r.text);
            it.avatar->setBitmap(pic);
        } else {
            it.icon->setIcon(icon);
        }
        it.sub->setVisible(!sub.empty());
        it.sub->setText(sub);
        it.line->setChecked(i == _s._current);
    }
    float estimateHeight(int i) const override {
        const Kind k = _s._rows[size_t(i)].kind;
        return (headed(i) ? 24 : 0) + (k == Kind::Message || k == Kind::FullResult ? 46 : 32);
    }

private:
    UnifiedSearch &_s;
};

UnifiedSearch::UnifiedSearch(
    screens::Context                        &ctx,
    Avatars                                 &avatars,
    std::vector<QuickSwitchTab>              tabs,
    int                                      openTab,
    std::function<model::Backend *(int tab)> backendFor
)
    : _avatars(avatars), _tabs(std::move(tabs)), _backendFor(std::move(backendFor)), _tab(openTab) {
    if (_tabs.empty()) { // the open workspace alone
        _tabs = {QuickSwitchTab{{}, {}, {}, &ctx.store(), quickSwitchOrder(ctx.store(), {})}};
        _tab  = 0;
    }
    _tab             = std::clamp(_tab, 0, int(_tabs.size()) - 1);
    const bool multi = _tabs.size() > 1;
    style().width(560).padding(10).spacing(8);
    if (multi) { // QuickSwitcher's strip
        _strip = add<View>();
        _strip->style().row().spacing(6).items(Align::Center);
        for (size_t i = 0; i < _tabs.size(); ++i) {
            const QuickSwitchTab &t = _tabs[i];
            auto                 *b =
                _strip->add<TabBubble>(t.name, t.icon.empty() ? nullptr : avatars.get(t.icon, 80));
            b->onClick = [this, i] { setTab(int(i)); };
        }
        _tabName = _strip->add<Label>("", Font::Title);
        _tabName->setMaxLines(1);
        _tabName->style().flex(1).margins(4, 0, 0, 0);
    }
    _field = add<TextEdit>();
    _field->setMaxLines(1);
    _field->setBackground(C::InputBg, 6);
    _field->setBorder(C::InputBorderFocus);
    _field->style().padding(10, 8);
    _field->onChange = [this] { applyQuery(); };
    // Whatever the modifiers: ↑/↓ move (wrapping), Enter opens, Esc leaves
    // the full list then closes; with several workspaces ←/→ and Tab switch.
    _field->onKey    = [this, multi](const Event &e) {
        if (e.type != EventType::KeyDown)
            return false;
        switch (e.key) {
        case plat::Key::Down:
        case plat::Key::Up:
            if (const int n = int(_rows.size()))
                highlight((_current + (e.key == plat::Key::Down ? 1 : n - 1)) % n);
            return true;
        case plat::Key::Enter:
        case plat::Key::KpEnter:
            choose(_current);
            return true;
        case plat::Key::Escape:
            if (_all) {
                _all = false;
                rebuild(false);
            } else {
                close();
            }
            return true;
        case plat::Key::Left:
        case plat::Key::Right:
            if (!multi)
                return false;
            stepTab(e.key == plat::Key::Left ? -1 : 1);
            return true;
        case plat::Key::Tab:
            if (!multi)
                return false;
            stepTab(e.mods & plat::ModShift ? -1 : 1);
            return true;
        default:
            return false;
        }
    };
    _adapter = std::make_unique<Adapter>(*this);
    _list    = add<VirtualList>(_adapter.get());
    _list->setGap(1);
    _list->style().height(kListH);
    const char       *upDown = "\xE2\x86\x91\xE2\x86\x93", *leftRight = "\xE2\x86\x90\xE2\x86\x92";
    const std::string enter = shortcuts::nativeKeys(shortcuts::Id::SendMessage);
    auto             *hint  = add<Label>(
        multi ? i18n::arg(
                    tr("%1 to move \xC2\xB7 %2 to switch workspace \xC2\xB7 %3 to open"),
                    upDown,
                    leftRight,
                    enter
                )
              : i18n::arg(tr("%1 to move \xC2\xB7 %2 to open"), upDown, enter),
        Font::Small,
        C::TextMuted
    );
    hint->setAlign(text::LayoutOptions::Align::Center);
    refreshStrip();
    requery();
}

UnifiedSearch::~UnifiedSearch() = default;

const std::vector<QuickSwitchName> &UnifiedSearch::names(QuickSwitchTab &t) {
    if (t.namesRev != t.store->metaRevision()) {
        t.names    = quickSwitchNames(*t.store, t.order);
        t.namesRev = t.store->metaRevision();
    }
    return t.names;
}

void UnifiedSearch::applyQuery() {
    aimTab(std::string(str::trim(_field->text())));
    refreshStrip();
    requery();
}

// QuickSwitcher::applyFilter's re-aim: the tab follows the best name match;
// ties keep the showing tab, and one picked by hand holds while it has some.
void UnifiedSearch::aimTab(const std::string &query) {
    _dimmed.clear();
    if (query.empty()) {
        _manualTab = false; // a fresh query starts with a fresh mind
        return;
    }
    if (_tabs.size() < 2)
        return;
    const std::vector<uint32_t>        q = fuzzyQuery(query);
    std::vector<std::optional<double>> scores;
    int                                best = -1;
    for (size_t i = 0; i < _tabs.size(); ++i) {
        std::optional<double> s;
        for (const QuickSwitchName &n : names(_tabs[i]))
            if (const std::optional<double> x = scoreOf(n, q); x && (!s || *x > *s))
                s = x;
        scores.push_back(s);
        _dimmed.push_back(!s);
        if (s && (best < 0 || *s > *scores[size_t(best)]))
            best = int(i);
    }
    const std::optional<double> &cur = scores[size_t(_tab)];
    if (best >= 0 && cur && *cur == *scores[size_t(best)])
        best = _tab;
    if (best >= 0 && best != _tab && (!_manualTab || !cur))
        _tab = best;
}

void UnifiedSearch::setTab(int index) {
    if (index < 0 || index >= int(_tabs.size()) || index == _tab)
        return;
    _tab       = index;
    // Picked while a query is up: the user overruling the re-aim.
    _manualTab = !str::trim(_field->text()).empty();
    refreshStrip();
    requery();
}

void UnifiedSearch::stepTab(int delta) {
    const int n = int(_tabs.size());
    if (n > 1)
        setTab(((_tab + delta) % n + n) % n);
}

void UnifiedSearch::refreshStrip() {
    const QuickSwitchTab &t = _tabs[size_t(_tab)];
    _field->setPlaceholder(
        i18n::arg(tr("Search %1"), t.name.empty() ? t.store->workspaceName : t.name)
    );
    if (!_strip)
        return;
    for (size_t i = 0; i < _tabs.size(); ++i)
        static_cast<TabBubble *>(_strip->child(i))
            ->set(int(i) == _tab, i < _dimmed.size() && _dimmed[i]);
    _tabName->setText(t.name);
}

void UnifiedSearch::requery() {
    QuickSwitchTab &t = _tabs[size_t(_tab)];
    _query            = std::string(str::trim(_field->text()));
    _recent   = _query.empty() ? recentPlaces(*t.store, t.order) : std::vector<RecentPlace>();
    _people   = searchPeople(*t.store, _query);
    _channels = searchChannels(*t.store, t.order, names(t), _query);
    if (_query.empty())
        _all = false; // clearing the query returns to the groups
    // The old hits answer another query (or workspace): gone, and a reply
    // still on its way is ignored.
    _hits.clear();
    ++_generation;
    _searching     = !_query.empty();
    _debouncing    = _searching;
    _debounceStart = -1;
    if (_debouncing)
        startTicking();
    rebuild(false);
}

bool UnifiedSearch::tick(double nowMs) {
    if (!_debouncing)
        return false;
    if (_debounceStart < 0)
        _debounceStart = nowMs;
    if (nowMs - _debounceStart < kDebounceMs)
        return true;
    _debouncing       = false;
    model::Backend *b = _backendFor ? _backendFor(_tab) : nullptr;
    if (!b) {
        _searching = false;
        rebuild(true);
        return false;
    }
    // Slack's modifiers (from:, in:, quotes…) pass through untouched.
    const uint32_t      gen   = _generation;
    std::weak_ptr<char> alive = _alive;
    b->search(_query, [this, alive, gen](std::vector<model::Backend::SearchHit> hits) {
        if (alive.expired() || gen != _generation)
            return;
        _hits      = std::move(hits);
        _searching = false;
        rebuild(true);
    });
    return false;
}

void UnifiedSearch::rebuild(bool keepPick) {
    const model::Store &st = store();
    _rows.clear();
    auto add = [&](Kind k, std::string text, size_t i) {
        _rows.push_back({k, std::move(text), i});
    };
    if (_all) {
        for (size_t i = 0; i < _hits.size(); ++i)
            add(Kind::FullResult, searchPreview(st, _hits[i].text), i);
    } else {
        for (size_t i = 0; i < _recent.size(); ++i) {
            const RecentPlace &p = _recent[i];
            add(Kind::Recent,
                p.conv == model::kNoConv ? pageName(p.page) : st.displayName(p.conv),
                i);
        }
        for (size_t i = 0; i < _people.size(); ++i)
            add(Kind::Person, std::string(st.user(_people[i].user).label()), i);
        for (size_t i = 0; i < _channels.size(); ++i)
            add(Kind::Channel, st.displayName(_channels[i]), i);
        for (size_t i = 0; i < _hits.size() && i < kGroupedHits; ++i)
            add(Kind::Message, searchPreview(st, _hits[i].text), i);
        if (!_hits.empty())
            add(Kind::AllMessages,
                i18n::arg(tr("Search all messages for \xE2\x80\x9C%1\xE2\x80\x9D"), _query),
                0);
    }
    if (_hits.empty() && !_query.empty())
        add(Kind::Status,
            _searching ? tr("Searching\xE2\x80\xA6") : std::string(tr("No results found.")),
            0);
    // A late reply only adds rows below the instant groups: the pick stays.
    if (!keepPick || _current >= int(_rows.size()))
        _current = 0;
    _list->reset();
    if (!keepPick)
        _list->scrollTo(0);
}

void UnifiedSearch::highlight(int index) {
    const int prev = _current;
    _current       = index;
    if (prev >= 0 && prev < int(_rows.size()))
        _list->itemsChanged(prev, 1); // rebinds its checked state
    _list->itemsChanged(index, 1);
    _list->scrollToItem(index, VirtualList::ItemAlign::Nearest, false);
}

void UnifiedSearch::showAllMessages() {
    if (_all || _query.empty())
        return;
    _all = true;
    rebuild(false);
}

void UnifiedSearch::choose(int index) {
    if (index < 0 || size_t(index) >= _rows.size())
        return;
    const Row r = _rows[size_t(index)];
    if (r.kind == Kind::AllMessages)
        return showAllMessages();
    if (r.kind == Kind::Status)
        return;
    // Copies first: close() lets go of this panel.
    const std::string     key = _tabs[size_t(_tab)].key;
    std::function<void()> act;
    switch (r.kind) {
    case Kind::Recent: {
        const RecentPlace p = _recent[r.index];
        if (p.conv != model::kNoConv)
            act = [f = onOpenConv, key, c = p.conv] {
                if (f)
                    f(key, c);
            };
        else
            act = [f = onOpenPage, n = p.page] {
                if (f)
                    f(n);
            };
        break;
    }
    case Kind::Person: {
        const PersonHit p = _people[r.index];
        if (p.dm != model::kNoConv)
            act = [f = onOpenConv, key, c = p.dm] {
                if (f)
                    f(key, c);
            };
        else
            act = [f = onMessageUser, key, u = p.user] {
                if (f)
                    f(key, u);
            };
        break;
    }
    case Kind::Channel:
        act = [f = onOpenConv, key, c = _channels[r.index]] {
            if (f)
                f(key, c);
        };
        break;
    default: {
        const model::Backend::SearchHit &h = _hits[r.index];
        act                                = [f = onOpenMessage, key, c = h.conv, ts = h.ts] {
            if (f)
                f(key, c, ts);
        };
        break;
    }
    }
    close();
    act();
}

} // namespace shell
