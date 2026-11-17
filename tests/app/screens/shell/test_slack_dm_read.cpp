// End-to-end backend -> Store -> Shell -> Dock badge regression for DM read scope.
#include "app/slack/slack_backend.h"
#include "app/slack/slack_json.h"
#include "base/json.h"
#include "base/time.h"
#include "net/net.h"
#include "plat/testing.h"
#include "screens/shell/shell.h"
#include "support/fake_slack_server.h"
#include "support/test.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/image_cache.h"
#endif

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#ifndef _WIN32

namespace {

ui::App &uiApp() {
    static std::unique_ptr<ui::App> a = [] {
        std::string err;
        auto        p = ui::App::create(&err);
        if (!p)
            std::fprintf(stderr, "ui::App::create: %s\n", err.c_str());
        return p;
    }();
    return *a;
}

void set(const std::string &json) {
    fakeslack::ctl("POST", "/_ctl/set", json);
}

void script(const char *method, const std::string &responses) {
    fakeslack::ctl(
        "POST",
        "/_ctl/script",
        std::string("{\"method\":\"") + method + "\",\"responses\":" + responses + "}"
    );
}

bool pumpUntil(ui::App &app, const std::function<bool()> &done, int timeoutMs = 6000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        app.pump(5);
    }
    return true;
}

void pumpFor(ui::App &app, int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline)
        app.pump(5);
}

plat::TestHooks &hooks() {
    plat::TestHooks *h = uiApp().platform().testHooks();
    assert(h != nullptr);
    return *h;
}

const char *kDmScopeWorkspace = R"({
 "rtm.connect": {"ok": false, "error": "not_allowed_token_type"},
 "auth.test": {"ok": true, "user_id": "UME", "team_id": "T1", "team": "Lumen",
               "url": "https://lumen.slack.com/"},
 "users.list": {"ok": true, "members": [
     {"id": "UME", "name": "me", "profile": {"real_name": "Me Myself"}},
     {"id": "UPEER", "name": "peer", "profile": {"real_name": "Peer User"}}],
   "response_metadata": {"next_cursor": ""}},
 "users.conversations": {"ok": true, "channels": [
     {"id": "C1", "name": "general", "is_channel": true, "is_member": true,
      "last_read": "1794909800.000000", "latest": {"ts": "1794909900.000000"},
      "unread_count": 1, "unread_count_display": 1, "mention_count": 1, "mention_count_display": 1},
     {"id": "D1", "is_im": true, "user": "UPEER",
      "last_read": "1794909941.811299",
      "latest": {"ts": "1794910537.557059", "thread_ts": "1794909869.919749"},
      "unread_count": 1, "unread_count_display": 1, "mention_count": 0, "mention_count_display": 0}],
   "response_metadata": {"next_cursor": ""}},
 "client.counts": {"ok": true,
   "channels": [{"id": "C1", "latest": "1794909900.000000", "last_read": "1794909800.000000",
                 "mention_count": 1, "unread_count_display": 1, "has_unreads": true}],
   "ims": [{"id": "D1", "latest": "1794909869.919749", "last_read": "1794909941.811299",
            "has_unreads": false, "mention_count": 0}],
   "mpims": []},
 "conversations.history": {"ok": true, "messages": [], "has_more": false},
 "stars.list": {"ok": true, "items": []},
 "emoji.list": {"ok": true, "emoji": {}},
 "usergroups.list": {"ok": true, "usergroups": []},
 "users.getPresence": {"ok": true, "presence": "away", "online": false}
})";

} // namespace

TEST("dm scope ui: read main and thread stay read across contradictory refreshes") {
    if (fakeslack::server().empty())
        return;

    base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", "100");
    fakeslack::ctl("POST", "/_ctl/reset");
    set(kDmScopeWorkspace);

    ui::App     &app = uiApp();
    net::Client  client(app.platform());
    model::Store store;

    slack::Credentials creds;
    creds.teamId   = "T1";
    creds.teamName = "Lumen";
    creds.token    = "xoxc-test";
    creds.cookie   = "xoxd-test";

    auto be = std::make_unique<slack::SlackBackend>(store, app.platform(), client, creds);
    be->connect([](bool, const std::string &) {});

#ifdef MSGA_HAVE_MESSAGES
    screens::ImageCache images(app.platform());
    screens::Context    ctx{app, store, *be, images, {}, {}, {}, {}, {}};
#else
    alignas(16) char noCache[16]{};
    screens::Context ctx{app, store, *be, *reinterpret_cast<screens::ImageCache *>(noCache),
                         {}, {}, {}, {}, {}};
#endif
    shell::Settings settings;
    plat::WindowDesc wd;
    wd.size        = {1100, 720};
    wd.decorations = plat::Decorations::Custom;
    auto win       = std::make_unique<ui::Window>(wd);
    auto sh        = std::make_unique<shell::Shell>(ctx, *win, settings, std::string());

    // Wait for initial roster and client.counts to settle.
    REQUIRE(pumpUntil(app, [&] {
        return store.findConversation("C1") != model::kNoConv &&
               store.findConversation("D1") != model::kNoConv;
    }));
    const model::ConvRef c1 = store.findConversation("C1");
    const model::ConvRef dm = store.findConversation("D1");
    // Keep C1 selected so opening D1 never clears it as a side effect.
    sh->open(c1);
    REQUIRE(pumpUntil(app, [&] { return store.conversation(dm).unread == 0; }));
    pumpFor(app, 60);

    const uint64_t beforeRefresh = hooks().maxNotificationId();
    const int      otherAtt      = sh->sidebar().attentionCount();
    CHECK(sh->sidebar().rowState(dm).badge == 0);
    CHECK(hooks().badgeCount() == otherAtt);

    // Establish thread floor == reply via conversations.replies.
    const model::Ts root = model::parseTs("1794909869.919749");
    script("conversations.replies", R"([{"ok":true,"has_more":false,"messages":[
        {"type":"message","ts":"1794909869.919749","user":"UPEER","text":"root",
         "reply_count":3,"latest_reply":"1794910537.557059","last_read":"1794910537.557059"},
        {"type":"message","ts":"1794910537.557059","thread_ts":"1794909869.919749",
         "user":"UPEER","text":"already read reply"}]}])");
    bool threadLoaded = false;
    be->loadThread(dm, root, [&](bool ok, const std::string &) { threadLoaded = ok; });
    REQUIRE(pumpUntil(app, [&] { return threadLoaded; }));
    CHECK(store.unreadThreads() == 0);

    // Track every Meta emission: a transient false badge 1 is a failure.
    uint32_t maxBadgeDuringRefresh = 0;
    store.observe(dm, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Meta)
            maxBadgeDuringRefresh = std::max<uint32_t>(
                maxBadgeDuringRefresh, uint32_t(sh->sidebar().rowState(dm).badge)
            );
    });

    // Deliver contradictory conversations.info (unread=1, latest=reply) + roster refresh.
    script("conversations.info", R"([{"ok":true,"channel":{"id":"D1","is_im":true,"user":"UPEER",
        "last_read":"1794909941.811299",
        "latest":{"ts":"1794910537.557059","thread_ts":"1794909869.919749"},
        "unread_count":1,"unread_count_display":1,"mention_count":0,"mention_count_display":0}}])");
    be->readCallObservedForTest(
        "conversations.info",
        "channel=D1",
        [&](const json::Document &doc,
            const std::string    &err,
            const slack::readstate::DispatchContext &ctxObs) {
            if (err.empty()) {
                be->applyReadObservationForTest(
                    dm,
                    slack::mapjson::conversationReadObservation(
                        doc.root()["channel"], slack::readstate::Source::ConversationInfo
                    ),
                    ctxObs
                );
            }
        }
    );
    be->reloadConversationsForTest();
    pumpFor(app, 120);

    CHECK(sh->sidebar().rowState(dm).badge == 0);
    CHECK(maxBadgeDuringRefresh == 0);
    CHECK(sh->sidebar().attentionCount() == otherAtt);
    CHECK(hooks().badgeCount() == otherAtt);
    CHECK(hooks().maxNotificationId() == beforeRefresh);

    // Genuinely new main message arrives via backend poll/counts.
    set(R"({
      "client.counts": {"ok": true,
        "channels": [{"id": "C1", "latest": "1794909900.000000", "last_read": "1794909800.000000",
                      "mention_count": 1, "unread_count_display": 1, "has_unreads": true}],
        "ims": [{"id": "D1", "latest": "1794910538.000000", "last_read": "1794909941.811299",
                 "has_unreads": true, "dm_count": 1, "unread_count_display": 1, "mention_count": 1}],
        "mpims": []}
    })");
    be->pollUnreadCountsForTest();
    REQUIRE(pumpUntil(app, std::function<bool()>([&] { return sh->sidebar().rowState(dm).badge == 1; })));
    CHECK(sh->sidebar().attentionCount() == otherAtt + 1);
    REQUIRE(pumpUntil(app, std::function<bool()>([&] { return hooks().badgeCount() == otherAtt + 1; })));
}

#endif // !_WIN32
