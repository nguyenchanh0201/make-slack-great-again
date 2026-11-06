// The unified search model: people, channels and recent places.
#include "app/model/store.h"
#include "screens/shell/unified_search_model.h"
#include "support/test.h"

#include <algorithm>

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
    CHECK(r == all); // the DM never matched "talk"; same order as the switcher
    // "henry" matches the DM by name: not a channel.
    CHECK(shell::searchChannels(f.store, order, names, "henry").empty());
}

TEST(
    "unified model: recent places lead with Threads, Saved, Activity, Scheduled then conversations"
) {
    Fixture              f;
    std::vector<ConvRef> order;
    for (ConvRef c = 0; c < 6; ++c)
        order.push_back(c % 3);
    const auto r = shell::recentPlaces(f.store, order);
    REQUIRE(r.size() == 8);
    const Sidebar::Nav pages[] = {
        Sidebar::Nav::Threads, Sidebar::Nav::Saved, Sidebar::Nav::Activity, Sidebar::Nav::Scheduled
    };
    for (size_t i = 0; i < 4; ++i) {
        CHECK(r[i].conv == kNoConv);
        CHECK(r[i].page == pages[i]);
    }
    for (size_t i = 4; i < 8; ++i) {
        CHECK(r[i].conv == order[i - 4]);
        CHECK(r[i].page == Sidebar::Nav::None);
    }
}

TEST("unified model: empty or whitespace query finds nothing") {
    Fixture    f;
    const auto order = f.order();
    const auto names = shell::quickSwitchNames(f.store, order);
    CHECK(shell::searchPeople(f.store, "  ").empty());
    CHECK(shell::searchChannels(f.store, order, names, "").empty());
}
