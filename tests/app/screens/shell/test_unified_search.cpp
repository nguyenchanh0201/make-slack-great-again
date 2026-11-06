// The unified search model (people, channels and recent places) and its panel.
#include "app/fake/fake_backend.h"
#include "app/model/store.h"
#include "base/str.h"
#include "plat/testing.h"
#include "screens/shell/avatars.h"
#include "screens/shell/unified_search.h"
#include "screens/shell/unified_search_model.h"
#include "support/test.h"
#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/image_cache.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>

using namespace model;
using shell::Sidebar;

namespace {

struct Fixture {
    model::Store store;
    UserRef      henry, hanna;
    ConvRef      talkAi, talkBackend, dm;
    Fixture() {
        auto user = [&](const char *id, const char *handle, const char *display) {
            User u;
            u.id = id, u.name = handle, u.displayName = display;
            return store.addUser(std::move(u));
        };
        store.me  = user("U0ME", "me", "Me Myself");
        henry     = user("U0HENRY", "henry", "Henry Nguyen");
        hanna     = user("U0HANNA", "hanna", "Hanna Ren");
        auto conv = [&](const char *id, const char *name, ConvKind k, UserRef peer) {
            Conversation c;
            c.id = id, c.name = name, c.kind = k, c.dmUser = peer;
            return store.addConversation(std::move(c));
        };
        talkAi      = conv("C1", "talk-ai", ConvKind::Channel, kNoUser);
        talkBackend = conv("C2", "talk-backend", ConvKind::Channel, kNoUser);
        dm          = conv("D1", "", ConvKind::Dm, henry);
    }
    std::vector<ConvRef> order() const { return shell::quickSwitchOrder(store, {}); }
};

} // namespace

TEST("unified model: people match display name, real name and handle, best first") {
    Fixture f;
    f.store.user(f.hanna).realName = "Johanna Ry";
    auto r                         = shell::searchPeople(f.store, "henry");
    REQUIRE(!r.empty());
    CHECK(r[0].user == f.henry);
    r = shell::searchPeople(f.store, "hen");
    REQUIRE(r.size() == 2);
    CHECK(r[0].user == f.henry);
    r = shell::searchPeople(f.store, "johanna"); // the real name alone
    REQUIRE(!r.empty());
    CHECK(r[0].user == f.hanna);
}

TEST("unified model: people skip bots, deleted, strangers and me") {
    Fixture f;
    f.store.user(f.store.me).displayName = "Henry Me";
    auto add                             = [&](const char *id, const char *name, auto set) {
        User u;
        u.id = id, u.name = "x", u.displayName = name;
        set(u);
        f.store.addUser(std::move(u));
    };
    add("U0B", "Henry Bot", [](User &u) { u.bot = true; });
    add("U0D", "Henry Old", [](User &u) { u.deleted = true; });
    add("U0S", "Henry Far", [](User &u) { u.stranger = true; });
    const auto r = shell::searchPeople(f.store, "henry");
    REQUIRE(r.size() == 1);
    CHECK(r[0].user == f.henry);
}

TEST("unified model: a person with a DM carries it") {
    Fixture f;
    auto    r = shell::searchPeople(f.store, "henry");
    REQUIRE(r.size() == 1);
    CHECK(r[0].dm == f.dm);
    r = shell::searchPeople(f.store, "hanna");
    REQUIRE(r.size() == 1);
    CHECK(r[0].dm == kNoConv);
}

TEST("unified model: channels exclude DMs and keep fuzzy order") {
    Fixture    f;
    const auto order = f.order();
    const auto names = shell::quickSwitchNames(f.store, order);
    const auto r     = shell::searchChannels(f.store, order, names, "talk");
    const auto all   = shell::quickSwitchFilter("talk", order, names);
    REQUIRE(r.size() == 2);
    // The DM never matched "talk"; same order as the switcher, scores falling.
    CHECK(r[0].second == all[0]);
    CHECK(r[1].second == all[1]);
    CHECK(r[0].first >= r[1].first);
    // "henry" matches the DM by name: not a channel.
    CHECK(shell::searchChannels(f.store, order, names, "henry").empty());
}

TEST(
    "unified model: recent places lead with conversations, then Threads, Saved, Activity, "
    "Scheduled"
) {
    std::vector<ConvRef> order;
    for (ConvRef c = 0; c < 20; ++c)
        order.push_back(c);
    const Sidebar::Nav pages[] = {
        Sidebar::Nav::Threads, Sidebar::Nav::Saved, Sidebar::Nav::Activity, Sidebar::Nav::Scheduled
    };
    // Up to 12: eight conversations, the pages always in.
    auto r = shell::recentPlaces(order);
    REQUIRE(r.size() == 12);
    for (size_t i = 0; i < 8; ++i) {
        CHECK(r[i].conv == order[i]);
        CHECK(r[i].page == Sidebar::Nav::None);
    }
    for (size_t i = 0; i < 4; ++i) {
        CHECK(r[8 + i].conv == kNoConv);
        CHECK(r[8 + i].page == pages[i]);
    }
    // The open conversation is not a place to go: the next one moves up.
    r = shell::recentPlaces(order, 0);
    REQUIRE(r.size() == 12);
    CHECK(r[0].conv == 1);
    for (const auto &p : r)
        CHECK(p.conv != 0);
    // Few conversations: all of them, then the pages.
    r = shell::recentPlaces({3, 5}, 5);
    REQUIRE(r.size() == 5);
    CHECK(r[0].conv == 3);
    CHECK(r[1].page == Sidebar::Nav::Threads);
}

TEST("unified model: empty or whitespace query finds nothing") {
    Fixture    f;
    const auto order = f.order();
    const auto names = shell::quickSwitchNames(f.store, order);
    CHECK(shell::searchPeople(f.store, "  ").empty());
    CHECK(shell::searchChannels(f.store, order, names, "").empty());
}

// ── The panel ───────────────────────────────────────────────────────────────

namespace {

ui::App &app() {
    static std::unique_ptr<ui::App> a = [] {
        std::string err;
        auto        p = ui::App::create(&err);
        if (!p)
            std::fprintf(stderr, "ui::App::create: %s\n", err.c_str());
        return p;
    }();
    return *a;
}

void pump(int n = 10) {
    for (int i = 0; i < n; ++i)
        app().pump(2);
}

void press(ui::Window &w, plat::Key k) {
    app().platform().testHooks()->injectKey(w.native(), k, true);
    app().platform().testHooks()->injectKey(w.native(), k, false);
    pump(2);
}

// Pumps (frames run back to back) until `done`, for at most 3 s.
void waitFor(const std::function<bool()> &done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done() && std::chrono::steady_clock::now() < end)
        app().pump(5);
}

// Records each search; the test answers them, in any order.
class SearchFake final : public fake::FakeBackend {
public:
    using FakeBackend::FakeBackend;
    struct Pending {
        std::string                                 query;
        std::function<void(std::vector<SearchHit>)> done;
    };
    std::vector<Pending> pending;
    void search(std::string query, std::function<void(std::vector<SearchHit>)> done) override {
        pending.push_back({std::move(query), std::move(done)});
    }
};

using Kind = shell::UnifiedSearch::RowKind;

// The model fixture behind a panel in a window, its searches recorded.
struct Panel {
    Fixture    f;
    SearchFake backend{f.store, app().platform()};
#ifdef MSGA_HAVE_MESSAGES
    screens::ImageCache images{app().platform()};
    screens::Context    ctx{app(), f.store, backend, images, {}, {}, {}, {}, {}};
#else
    alignas(16) char noCache[16]{};
    screens::Context ctx{
        app(),
        f.store,
        backend,
        *reinterpret_cast<screens::ImageCache *>(noCache),
        {},
        {},
        {},
        {},
        {}
    };
#endif
    shell::Avatars              avatars{ctx.images};
    std::unique_ptr<ui::Window> win;
    shell::UnifiedSearch       *us     = nullptr;
    bool                        closed = false;

    // `tabs`: the workspaces besides this one (its tab comes first).
    // `setup`: fills the store before the panel takes its order.
    explicit Panel(
        std::vector<shell::QuickSwitchTab> others = {},
        std::function<Backend *(int)>      b      = {},
        std::function<void(Fixture &)>     setup  = {}
    ) {
        if (setup)
            setup(f);
        plat::WindowDesc d;
        d.size    = {1200, 800};
        d.visible = true; // frames (the debounce's ticks) only reach a shown window
        win       = std::make_unique<ui::Window>(d);
        std::vector<shell::QuickSwitchTab> tabs;
        if (!others.empty()) {
            tabs.push_back({"A", "Alpha", "", &f.store, shell::quickSwitchOrder(f.store, {})});
            for (auto &t : others)
                tabs.push_back(std::move(t));
        }
        auto own = [this](int) -> Backend * { return &backend; };
        auto p   = std::make_unique<shell::UnifiedSearch>(
            ctx, avatars, std::move(tabs), 0, b ? std::move(b) : own
        );
        us = p.get();
        p->setAnchor({340, 40, 520, 0}, ui::Popup::Place::Below);
        p->onClosed = [this] { closed = true; };
        win->showPopup(std::move(p));
        us->field().focus();
        pump();
    }
    std::vector<Kind> kinds() const {
        std::vector<Kind> k;
        for (const auto &r : us->rows())
            k.push_back(r.kind);
        return k;
    }
    // Waits out the debounce until a search has gone out (`n` in all).
    void waitForSearch(size_t n = 1) {
        waitFor([&] { return backend.pending.size() >= n; });
        REQUIRE(backend.pending.size() >= n);
    }
    static std::vector<Backend::SearchHit> hits(ConvRef conv, int n, const char *word) {
        std::vector<Backend::SearchHit> out;
        for (int i = 0; i < n; ++i)
            out.push_back(
                {conv, Ts(1000000 * (n - i)), 0, str::concat({word, " ", std::to_string(i)})}
            );
        return out;
    }
};

} // namespace

TEST("unified search: empty field lists recent places") {
    Panel p;
    REQUIRE(!p.us->rows().empty());
    for (const auto &r : p.us->rows())
        CHECK(r.kind == Kind::Recent);
    // The conversations first, then the pages.
    CHECK_STR(p.us->rows()[0].text, p.f.store.displayName(p.f.order()[0]));
    CHECK_STR(p.us->rows()[p.us->rows().size() - 4].text, "Threads");
    CHECK(p.us->selected() == 0);
    CHECK(p.backend.pending.empty()); // nothing to search for
}

TEST("unified search: typing groups people, channels, then messages") {
    Panel p;
    p.us->field().insertText("talk");
    auto k = p.kinds();
    CHECK(std::find(k.begin(), k.end(), Kind::Message) == k.end()); // not before the debounce
    CHECK(std::count(k.begin(), k.end(), Kind::Channel) == 2);
    p.waitForSearch();
    CHECK_STR(p.backend.pending[0].query, "talk");
    p.backend.pending[0].done(Panel::hits(p.f.talkAi, 20, "talk"));
    k = p.kinds();
    REQUIRE(k.size() >= 2);
    CHECK(k.back() == Kind::AllMessages);
    CHECK(std::is_sorted(k.begin(), k.end())); // Person*, Channel*, Message*, AllMessages
    CHECK(std::count(k.begin(), k.end(), Kind::Message) == 3);
    CHECK(p.us->selected() == 0);
}

TEST("unified search: modifier-only query sends raw text") {
    Panel p;
    p.us->field().insertText("from:@henry");
    p.waitForSearch();
    CHECK_STR(p.backend.pending.back().query, "from:@henry");
}

TEST("unified search: a stale message response is dropped") {
    Panel p;
    p.us->field().insertText("he");
    p.waitForSearch();
    p.us->field().insertText("nry");
    p.waitForSearch(2);
    CHECK_STR(p.backend.pending[1].query, "henry");
    p.backend.pending[1].done(Panel::hits(p.f.talkAi, 2, "henry"));
    p.backend.pending[0].done(Panel::hits(p.f.talkBackend, 2, "he"));
    int messages = 0;
    for (const auto &r : p.us->rows())
        if (r.kind == Kind::Message) {
            ++messages;
            CHECK(r.text.rfind("henry", 0) == 0);
        }
    CHECK(messages == 2);
}

TEST("unified search: enter on a person without a DM messages them") {
    Panel                                        p;
    std::vector<model::UserRef>                  messaged;
    std::vector<std::pair<std::string, ConvRef>> opened;
    p.us->onMessageUser = [&](const std::string &, model::UserRef u) { messaged.push_back(u); };
    p.us->onOpenConv    = [&](const std::string &k, ConvRef c) { opened.push_back({k, c}); };
    p.us->field().insertText("hanna");
    REQUIRE(!p.us->rows().empty());
    REQUIRE(p.us->rows()[0].kind == Kind::Person);
    press(*p.win, plat::Key::Enter);
    REQUIRE(messaged.size() == 1);
    CHECK(messaged[0] == p.f.hanna);
    CHECK(p.closed);

    Panel q;
    q.us->onOpenConv = [&](const std::string &k, ConvRef c) { opened.push_back({k, c}); };
    q.us->field().insertText("henry");
    REQUIRE(q.us->rows()[0].kind == Kind::Person);
    q.us->choose(0);
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].second == q.f.dm);
}

TEST("unified search: all messages expands and Esc returns") {
    Panel p;
    p.us->field().insertText("talk");
    p.waitForSearch();
    p.backend.pending[0].done(Panel::hits(p.f.talkAi, 20, "talk"));
    const auto k = p.kinds();
    REQUIRE(k.back() == Kind::AllMessages);
    p.us->choose(int(k.size()) - 1);
    CHECK(p.us->showingAll());
    CHECK_FALSE(p.closed);
    CHECK(p.us->rows().size() == 20);
    for (const auto &r : p.us->rows())
        CHECK(r.kind == Kind::FullResult);
    CHECK(p.backend.pending.size() == 1); // no second request
    ConvRef openedConv  = kNoConv;
    Ts      openedTs    = 0;
    p.us->onOpenMessage = [&](const std::string &, ConvRef c, Ts ts) {
        openedConv = c;
        openedTs   = ts;
    };
    press(*p.win, plat::Key::Escape);
    CHECK_FALSE(p.us->showingAll());
    CHECK_FALSE(p.closed);
    CHECK(p.kinds() == k);
    p.us->showAllMessages();
    p.us->choose(1);
    CHECK(openedConv == p.f.talkAi);
    CHECK(openedTs == 19000000);
    CHECK(p.closed);

    Panel q;
    q.us->field().insertText("talk");
    press(*q.win, plat::Key::Escape);
    CHECK(q.closed); // grouped: Esc closes
}

TEST("unified search: closing on workspace switch") {
    auto         other = std::make_unique<model::Store>();
    SearchFake   otherBackend{*other, app().platform()};
    Conversation c;
    c.id = "C9", c.name = "talk-other", c.kind = ConvKind::Channel;
    other->addConversation(std::move(c));
    Panel p(
        {shell::QuickSwitchTab{"B", "Beta", "", other.get(), shell::quickSwitchOrder(*other, {})}},
        [&](int tab) -> Backend * { return tab == 1 ? &otherBackend : nullptr; }
    );
    p.us->setTab(1);
    CHECK(p.us->tab() == 1);
    p.us->field().insertText("other"); // only Beta has it: the tab stays
    CHECK(p.us->tab() == 1);
    waitFor([&] { return !otherBackend.pending.empty(); });
    REQUIRE(otherBackend.pending.size() == 1);
    // The shell's detach: the panel closes, then the Store goes.
    p.us->close();
    pump();
    CHECK(p.closed);
    other.reset();
    otherBackend.pending[0].done(Panel::hits(0, 3, "late")); // nothing left to touch
    pump();
}

TEST("unified search: a reply after close is dropped") {
    Panel p;
    p.us->field().insertText("talk");
    p.waitForSearch();
    const auto kinds = p.kinds();
    p.us->close(); // the panel waits in the window's graveyard until the next flush
    CHECK(p.closed);
    p.backend.pending[0].done(Panel::hits(p.f.talkAi, 5, "talk"));
    CHECK(p.kinds() == kinds);
    pump();
}

TEST("unified search: the best person or channel is selected first") {
    Panel p({}, {}, [](Fixture &f) {
        Conversation c;
        c.id = "C9", c.name = "general", c.kind = ConvKind::Channel;
        f.store.addConversation(std::move(c));
        for (const char *name : {"Gene Ralston", "Generic Bot-free Person"}) {
            User u;
            u.id = std::string("U9") + name, u.name = "x", u.displayName = name;
            f.store.addUser(std::move(u));
        }
    });
    auto  selected = [&] { return p.us->rows()[size_t(p.us->selected())]; };
    // The channel's name is the query: it beats the people above it.
    p.us->field().insertText("general");
    REQUIRE(p.kinds().front() == Kind::Person);
    CHECK(selected().kind == Kind::Channel);
    CHECK_STR(selected().text, p.f.store.displayName(p.f.store.findConversation("C9")));
    // "gene": the person's whole first name, at least as good: people first.
    p.us->field().setText("gene");
    CHECK(selected().kind == Kind::Person);
    CHECK_STR(selected().text, "Gene Ralston");
    CHECK(p.us->selected() == 0);
}

TEST("unified search: Enter on Searching… opens the full list") {
    Panel p;
    p.us->field().insertText("zzqq words");
    REQUIRE(p.kinds() == std::vector<Kind>{Kind::Status});
    press(*p.win, plat::Key::Enter); // before the debounce is out
    CHECK(p.us->showingAll());
    CHECK_FALSE(p.closed);
    p.waitForSearch();
    p.backend.pending[0].done(Panel::hits(p.f.talkAi, 4, "words"));
    CHECK(p.kinds() == std::vector<Kind>(4, Kind::FullResult));
}
