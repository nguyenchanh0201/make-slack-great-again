// SlackBackend's realtime half against the local fake Slack, WebSockets
// included (tests/support/fake_slack.py /ws/…): Socket Mode envelopes, acks and
// events into the Store, the overlapping recycle, contention, the reconnect
// backfill (resyncUnreads), the polls a push workspace skips, the OAuth
// token refresh, and the RTM presence link. Nothing here talks to the real
// Slack.
#include "app/cache/workspace_cache.h"
#include "app/slack/rtm_presence.h"
#include "app/slack/slack_backend.h"
#include "app/slack/slack_json.h"
#include "app/slack/socket_mode.h"
#include "support/fake_slack_server.h"
#include "base/json.h"
#include "base/str.h"
#include "support/test.h"
#include "base/time.h"
#include "plat/plat.h"
#include "plat/testing.h"

#include <cstdlib>
#include <memory>
#include <string>

#ifndef _WIN32

namespace {

using fakeslack::app;
using fakeslack::ctl;
using fakeslack::pumpFor;
using fakeslack::pumpUntil;
using model::ConvRef;
using model::kNoConv;
using model::Ts;
using Link = model::Backend::PresenceLink;
using Mode = model::Backend::PresenceMode;

void set(const std::string &json) {
    ctl("POST", "/_ctl/set", json);
}
void script(const char *method, const std::string &responses) {
    ctl("POST",
        "/_ctl/script",
        std::string("{\"method\":\"") + method + "\",\"responses\":" + responses + "}");
}

struct Log {
    json::Document doc;
    Log() { doc.parse(ctl("GET", "/_ctl/log").body, nullptr); }
    int count(std::string_view method) const {
        int n = 0;
        for (json::Value r : doc.root())
            n += r["method"].str() == method;
        return n;
    }
    json::Value get(std::string_view method, int n = 0) const {
        for (json::Value r : doc.root())
            if (r["method"].str() == method && n-- == 0)
                return r;
        return {};
    }
    // A ws:sm / ws:rtm frame whose text contains `needle`.
    bool frame(std::string_view method, std::string_view needle) const {
        for (json::Value r : doc.root())
            if (r["method"].str() == method &&
                r["form"]["text"].str().find(needle) != std::string_view::npos)
                return true;
        return false;
    }
};

int count(std::string_view method) {
    return Log().count(method);
}

// The newest RTM socket's id (for push()).
std::string rtmConn() {
    std::string id;
    const Log   log;
    for (json::Value r : log.doc.root())
        if (r["method"].str() == "ws-open" && r["form"]["conn"].str().starts_with("rtm-"))
            id = std::string(r["form"]["conn"].str());
    return id;
}

int tickles() {
    int       n = 0;
    const Log log;
    for (json::Value r : log.doc.root())
        n += r["method"].str() == "ws:rtm" &&
             r["form"]["text"].str().find("tickle") != std::string_view::npos;
    return n;
}

// A server frame on socket `conn` (default: the newest open Socket Mode one).
bool push(const std::string &frame, const std::string &conn = {}) {
    std::string body = "{\"conn\":";
    json::escapeString(body, conn);
    body += ",\"text\":";
    json::escapeString(body, frame);
    body += '}';
    json::Document d;
    d.parse(ctl("POST", "/_ctl/ws/send", std::move(body)).body, nullptr);
    return d.root()["ok"].boolean();
}

// An events_api envelope for workspace `team`.
std::string envelope(const std::string &id, const std::string &event, const char *team = "T1") {
    return str::concat(
        {R"({"type":"events_api","envelope_id":")",
         id,
         R"(","payload":{"team_id":")",
         team,
         R"(","event":)",
         event,
         "}}"}
    );
}

const char *kWorkspace = R"({
 "auth.test": {"ok": true, "user_id": "UME", "team_id": "T1", "team": "Lumen",
               "url": "https://lumen.slack.com/"},
 "users.list": {"ok": true, "members": [
     {"id": "UME", "name": "me", "profile": {"real_name": "Me Myself"}},
     {"id": "UMIRA", "name": "mira", "profile": {"real_name": "Mira Okafor"}}],
   "response_metadata": {"next_cursor": ""}},
 "users.conversations": {"ok": true, "channels": [
     {"id": "C1", "name": "general", "is_channel": true, "is_member": true},
     {"id": "D1", "is_im": true, "user": "UMIRA"}],
   "response_metadata": {"next_cursor": ""}},
 "conversations.info?channel=D1": {"ok": true, "channel": {"id": "D1", "is_im": true,
     "user": "UMIRA", "unread_count": 4, "last_read": "1700000000.000000",
     "latest": {"ts": "1700000500.000000"}}},
 "conversations.info?channel=G9": {"ok": true, "channel": {"id": "G9", "is_mpim": true,
     "name": "mpdm-me--mira-1", "is_member": true, "unread_count": 7}},
 "conversations.info?channel=CX": {"ok": false, "error": "channel_not_found"},
 "stars.list": {"ok": true, "items": []},
 "emoji.list": {"ok": true, "emoji": {}},
 "usergroups.list": {"ok": true, "usergroups": []},
 "users.getPresence": {"ok": true, "presence": "away", "online": false},
 "conversations.history": {"ok": true, "messages": [], "has_more": false}
})";

struct Env {
    model::Store                         store;
    net::Client                          client{app()};
    std::shared_ptr<slack::SocketMode>   socket;
    std::unique_ptr<slack::SlackBackend> be;
    std::string                          lostAuth;
    int                                  parallel = 0;
    std::vector<slack::Credentials>      saved;

    // session: an xoxc workspace (no socket); else OAuth, with Socket Mode
    // when `realtime`.
    // `extra`: standing answers set before the backend exists (its first
    // calls may go out while a later ctl() pumps the loop).
    explicit Env(
        bool               session  = false,
        bool               realtime = true,
        slack::Credentials cr       = {},
        const char        *extra    = nullptr
    ) {
        base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", "100");
        ctl("POST", "/_ctl/reset");
        set(kWorkspace);
        if (extra)
            set(extra);
        cr.token            = cr.token.empty() ? (session ? "xoxc-test" : "xoxp-test") : cr.token;
        cr.cookie           = session ? "xoxd-test" : "";
        cr.teamId           = "T1";
        be                  = std::make_unique<slack::SlackBackend>(store, app(), client, cr);
        be->onAuthLost      = [this](const std::string &e) { lostAuth = e; };
        be->onParallelUsage = [this] { ++parallel; };
        be->onCredentialsChanged = [this](const slack::Credentials &c) { saved.push_back(c); };
        be->setAppConfig({"cid", "csecret", "xapp-test"});
        if (realtime) {
            socket = std::make_shared<slack::SocketMode>(app(), client, "xapp-test");
            slack::SocketMode::Timing t;
            t.backoffMs    = 20;
            t.backoffMaxMs = 200;
            socket->setTimingForTest(t);
            be->setRealtime(socket);
        }
    }
    ~Env() {
        be.reset();
        socket.reset();
        base::test::unsetEnv("MSGA_SLACK_TEST_SPEEDUP");
    }
    bool connect() {
        bool called = false, ok = false;
        be->connect([&](bool o, const std::string &) {
            called = true;
            ok     = o;
        });
        return pumpUntil([&] { return called && !be->connecting(); }) && ok;
    }
    bool live() {
        return pumpUntil([&] { return socket->connected() && count("ws-open") > 0; }, 5000);
    }
    ConvRef conv(const char *id) const { return store.findConversation(id); }
};

bool haveServer() {
    if (!fakeslack::server().empty())
        return true;
    std::fprintf(stderr, "  skip: no fake Slack (python3 missing)\n");
    return false;
}

} // namespace

TEST("slack realtime: socket mode opens with the app token, acks and delivers a message") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    CHECK(e.be->hasRealtimePush());
    // apps.connections.open carries the xapp token, not the workspace's.
    CHECK_STR(std::string(Log().get("apps.connections.open")["auth"].str()), "Bearer xapp-test");
    const ConvRef c = e.conv("C1");
    REQUIRE(push(envelope(
        "env-1",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"hi <@UME>","ts":"1800000000.000100"})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, model::parseTs("1800000000.000100")); }));
    CHECK(e.store.conversation(c).unread == 1 && e.store.conversation(c).mentions == 1);
    // Acked by envelope id.
    REQUIRE(pumpUntil([&] { return Log().frame("ws:sm", R"("envelope_id":"env-1")"); }));
    // The same envelope again (a redelivery) changes nothing.
    REQUIRE(push(envelope(
        "env-1",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"hi <@UME>","ts":"1800000000.000100"})"
    )));
    pumpFor(150);
    CHECK(e.store.conversation(c).unread == 1);
}

TEST("slack realtime: a message read as it lands (the open, focused chat) leaves no badge") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef c  = e.conv("C1");
    // The shell's observer: a new message on screen is read at once.
    const auto    id = e.store.observe(c, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Append && !ch.thread)
            e.be->markRead(c, e.store.conversation(c).messages.back().ts);
    });
    REQUIRE(push(envelope(
        "env-r1",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"hi <@UME>","ts":"1800000000.000100"})"
    )));
    const Ts ts = model::parseTs("1800000000.000100");
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts); }));
    e.store.unobserve(id);
    CHECK(e.store.conversation(c).lastRead == ts);
    CHECK(e.store.conversation(c).unread == 0);
    CHECK(e.store.conversation(c).mentions == 0);
    CHECK(e.store.conversation(c).latest == ts);
    // Not read (the window in the background): it counts.
    REQUIRE(push(envelope(
        "env-r2",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"again","ts":"1800000000.000200"})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, model::parseTs("1800000000.000200")); }));
    CHECK(e.store.conversation(c).unread == 1);
}

TEST("slack realtime: a reply to a thread I started is followed, its root unloaded") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef c    = e.conv("C1");
    const Ts      root = model::parseTs("1800000000.000050");
    REQUIRE(push(envelope(
        "env-p1",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"done","ts":"1800000000.000300",)"
        R"("thread_ts":"1800000000.000050","parent_user_id":"UME"})"
    )));
    REQUIRE(pumpUntil([&] { return e.be->threadFollowed(c, root); }));
    // Someone else's thread: not followed.
    REQUIRE(push(envelope(
        "env-p2",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"ok","ts":"1800000000.000400",)"
        R"("thread_ts":"1800000000.000060","parent_user_id":"UMIRA"})"
    )));
    pumpFor(150);
    CHECK(!e.be->threadFollowed(c, model::parseTs("1800000000.000060")));
}

TEST("slack realtime: edits, deletions, reactions, presence, users and marks") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef c  = e.conv("C1");
    const Ts      ts = model::parseTs("1800000000.000200");
    REQUIRE(push(envelope(
        "a",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"v1","ts":"1800000000.000200"})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts); }));
    REQUIRE(push(envelope(
        "b",
        R"({"type":"message","subtype":"message_changed","channel":"C1","message":{"type":"message","user":"UMIRA","text":"v2","ts":"1800000000.000200","edited":{"ts":"1"}}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts)->text == "v2"; }));
    CHECK(e.store.findMessage(c, ts)->edited);
    REQUIRE(push(envelope(
        "c",
        R"({"type":"reaction_added","user":"UMIRA","reaction":"tada","item":{"type":"message","channel":"C1","ts":"1800000000.000200"}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts)->reactions.size() == 1; }));
    CHECK(e.store.findMessage(c, ts)->reactions[0].count == 1);
    REQUIRE(push(envelope(
        "d",
        R"({"type":"reaction_removed","user":"UMIRA","reaction":"tada","item":{"type":"message","channel":"C1","ts":"1800000000.000200"}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts)->reactions.empty(); }));
    REQUIRE(push(envelope(
        "e",
        R"({"type":"message","subtype":"message_deleted","channel":"C1","deleted_ts":"1800000000.000200","previous_message":{"ts":"1800000000.000200"}})"
    )));
    REQUIRE(pumpUntil([&] { return !e.store.findMessage(c, ts); }));
    // Presence, DND, a profile change (presence kept), a read mark.
    const auto mira = e.store.findUser("UMIRA");
    REQUIRE(
        push(envelope("f", R"({"type":"presence_change","user":"UMIRA","presence":"active"})"))
    );
    REQUIRE(pumpUntil([&] { return e.store.user(mira).active; }));
    REQUIRE(push(envelope(
        "g", R"({"type":"dnd_updated_user","user":"UMIRA","dnd_status":{"dnd_enabled":true}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.user(mira).dnd; }));
    REQUIRE(push(envelope(
        "h",
        R"({"type":"user_change","user":{"id":"UMIRA","name":"mira","profile":{"real_name":"Mira O."}}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.user(mira).displayName == "Mira O."; }));
    CHECK(e.store.user(mira).active && e.store.user(mira).dnd);
    REQUIRE(push(envelope(
        "i",
        R"({"type":"channel_marked","channel":"C1","ts":"1800000001.000000","unread_count_display":0,"mention_count_display":0})"
    )));
    REQUIRE(pumpUntil([&] {
        return e.store.conversation(c).lastRead == model::parseTs("1800000001.000000");
    }));
    // A user group change reloads the groups (debounced).
    const int groups = count("usergroups.list");
    REQUIRE(push(envelope("j", R"({"type":"subteam_members_changed","subteam_id":"S1"})")));
    REQUIRE(pumpUntil([&] { return count("usergroups.list") > groups; }, 3000));
}

TEST("slack realtime: new channels and group DMs join the list; foreign events don't") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    // A group DM nobody announced: its first message fetches it, then counts.
    REQUIRE(push(envelope(
        "a",
        R"({"type":"message","channel":"G9","user":"UMIRA","text":"yo","ts":"1800000000.000300"})"
    )));
    REQUIRE(pumpUntil([&] { return e.conv("G9") != kNoConv; }));
    REQUIRE(pumpUntil([&] {
        return e.store.findMessage(e.conv("G9"), model::parseTs("1800000000.000300"));
    }));
    // conversations.info's unread_count (7) is not taken: the replay counts.
    CHECK(e.store.conversation(e.conv("G9")).unread == 1);
    {
        // The count is opt-in on conversations.info; the header shows it.
        bool      withCount = false;
        const Log log;
        for (json::Value r : log.doc.root())
            withCount |= r["method"].str() == "conversations.info" &&
                         r["form"]["channel"].str() == "G9" &&
                         r["form"]["include_num_members"].str() == "true";
        CHECK(withCount);
    }
    // channel_created joins it.
    REQUIRE(push(envelope(
        "b", R"({"type":"channel_created","channel":{"id":"C7","name":"launch","is_channel":true}})"
    )));
    REQUIRE(pumpUntil([&] { return e.conv("C7") != kNoConv; }));
    // Another workspace's events (the socket is the app's): nothing fetched,
    // nothing added.
    const int infos = count("conversations.info");
    REQUIRE(push(envelope(
        "c",
        R"({"type":"message","channel":"CZ","user":"UX","text":"x","ts":"1800000000.000400"})",
        "T2"
    )));
    REQUIRE(push(
        envelope("d", R"({"type":"channel_created","channel":{"id":"C8","name":"other"}})", "T2")
    ));
    pumpFor(200);
    CHECK(e.conv("CZ") == kNoConv && e.conv("C8") == kNoConv);
    CHECK(count("conversations.info") == infos);
    // Ours but dead (channel_not_found): asked once, then remembered.
    REQUIRE(push(envelope(
        "e", R"({"type":"message","channel":"CX","user":"UX","text":"1","ts":"1800000000.000500"})"
    )));
    REQUIRE(pumpUntil([&] { return count("conversations.info") == infos + 1; }));
    pumpFor(100);
    REQUIRE(push(envelope(
        "f", R"({"type":"message","channel":"CX","user":"UX","text":"2","ts":"1800000000.000600"})"
    )));
    pumpFor(200);
    CHECK(count("conversations.info") == infos + 1);
}

TEST("slack realtime: a disconnect warning brings up an overlapping replacement") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    REQUIRE(push(R"({"type":"disconnect","reason":"warning"})"));
    REQUIRE(pumpUntil([&] { return count("ws-open") == 2 && e.socket->socketsForTest() == 1; }));
    CHECK(count("apps.connections.open") == 2);
    // The new socket delivers; the old one is gone.
    REQUIRE(push(envelope(
        "z",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"after","ts":"1800000000.000700"})"
    )));
    REQUIRE(pumpUntil([&] {
        return e.store.findMessage(e.conv("C1"), model::parseTs("1800000000.000700"));
    }));
    CHECK(pumpUntil([&] {
        json::Document d;
        d.parse(ctl("GET", "/_ctl/ws").body, nullptr);
        int n = 0;
        for (json::Value s : d.root())
            n += s["open"].boolean();
        return n == 1;
    }));
    // refresh_requested: the socket is closed on purpose and replaced.
    REQUIRE(push(R"({"type":"disconnect","reason":"refresh_requested"})"));
    REQUIRE(pumpUntil([&] { return count("ws-open") == 3 && e.socket->connected(); }));
    CHECK(e.parallel == 0);
}

TEST("slack realtime: the watchdog keeps the socket alive with pings, never closing on silence") {
    if (!haveServer())
        return;
    Env                       e;
    slack::SocketMode::Timing t;
    t.watchdogMs = 40; // staleMs stays 50 s: no suspend gap in a test
    t.backoffMs  = 20;
    e.socket->setTimingForTest(t);
    REQUIRE(e.connect());
    REQUIRE(e.live());
    // Slack never pongs them (the fake does; it changes nothing): pings go
    // out tick after tick on the one socket, which stays.
    REQUIRE(pumpUntil([&] { return count("ws-ping") >= 3; }, 5000));
    CHECK(e.socket->connected());
    CHECK(count("ws-open") == 1);
    CHECK(count("apps.connections.open") == 1);
}

TEST("slack realtime: the network coming back reconnects a dropped socket at once") {
    if (!haveServer())
        return;
    // The socket alone: a backend's safety poll would reconnect it too.
    Env                       e(false, false);
    slack::SocketMode         sock(app(), e.client, "xapp-test");
    slack::SocketMode::Timing t;
    t.backoffMs = t.backoffMaxMs = 60'000; // a retry the test would never see
    sock.setTimingForTest(t);
    sock.start();
    REQUIRE(pumpUntil([&] { return sock.connected(); }, 5000));
    // The OS reports the network the way main.cpp hands it on (headless
    // plat's fake reachability source).
    plat::App &pa = app();
    pa.setEventHandler([&](const plat::Event &ev) {
        if (ev.type == plat::EventType::NetworkChanged)
            sock.networkChanged(ev.online);
    });
    plat::TestHooks *hooks = pa.testHooks();
    REQUIRE(hooks);
    // Online while connected: a ping, nothing else.
    REQUIRE(hooks->simulateSystemEvent(plat::EventType::NetworkChanged, true));
    REQUIRE(pumpUntil([&] { return count("ws-ping") >= 1; }));
    CHECK(count("ws-open") == 1);
    // The network drops the socket; the backoff would wait a minute.
    ctl("POST", "/_ctl/ws/close", R"({"code": 1001})");
    REQUIRE(pumpUntil([&] { return !sock.connected(); }));
    REQUIRE(hooks->simulateSystemEvent(plat::EventType::NetworkChanged, false));
    pumpFor(150);
    CHECK(count("ws-open") == 1); // offline: nothing to do
    REQUIRE(hooks->simulateSystemEvent(plat::EventType::NetworkChanged, true));
    REQUIRE(pumpUntil([&] { return count("ws-open") == 2 && sock.connected(); }, 3000));
    CHECK(count("apps.connections.open") == 2);
    pa.setEventHandler({});
    sock.stop();
}

TEST("slack realtime: contention raises the parallel-usage notice once") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    CHECK(e.parallel == 0); // alone in the pool
    // Another device joins the app's pool: the next hello counts it.
    ctl("POST", "/_ctl/ws/opt", R"({"ws_extra": 1})");
    REQUIRE(push(R"({"type":"disconnect","reason":"refresh_requested"})"));
    REQUIRE(pumpUntil([&] { return e.parallel == 1; }));
    // Throttled: another contended hello within the window says nothing.
    REQUIRE(pumpUntil([&] { return count("ws-open") == 2 && e.socket->connected(); }));
    REQUIRE(push(R"({"type":"disconnect","reason":"refresh_requested"})"));
    REQUIRE(pumpUntil([&] { return count("ws-open") == 3 && e.socket->connected(); }));
    pumpFor(100);
    CHECK(e.parallel == 1);
}

TEST("slack realtime: a reconnect backfills the roster and the DM badges") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const int lists = count("users.conversations");
    CHECK(e.store.conversation(e.conv("D1")).unread == 0);
    // Slack drops the socket: a later hello is a reconnect after a gap.
    ctl("POST", "/_ctl/ws/close", R"({"code": 1001})");
    REQUIRE(pumpUntil([&] { return count("ws-open") == 2 && e.socket->connected(); }));
    REQUIRE(pumpUntil([&] { return count("users.conversations") > lists; }));
    // resyncUnreads: conversations.info on the DM, merged upward.
    REQUIRE(pumpUntil([&] { return e.store.conversation(e.conv("D1")).unread == 4; }, 5000));
    CHECK(e.store.conversation(e.conv("D1")).mentions == 4);
    CHECK(e.store.conversation(e.conv("D1")).lastRead == model::parseTs("1700000000.000000"));
}

TEST("slack realtime: a resync answer older than my read leaves the DM read") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    // Read here up to the server's latest; its conversations.info still has
    // the cursor from before (the mark on its way): 4 unread at 1700000000.
    const ConvRef d1   = e.conv("D1");
    const Ts      read = model::parseTs("1700000500.000000");
    e.store.updateConversation(d1, [read](model::Conversation &x) {
        x.lastRead = x.latest = read;
        x.unread = x.mentions = 0;
    });
    const int infos = count("conversations.info");
    ctl("POST", "/_ctl/ws/close", R"({"code": 1001})");
    REQUIRE(pumpUntil([&] { return count("ws-open") == 2 && e.socket->connected(); }));
    REQUIRE(pumpUntil([&] { return count("conversations.info") > infos; }, 5000));
    pumpFor(600); // the batched patch
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(e.store.conversation(d1).mentions == 0);
    CHECK(e.store.conversation(d1).lastRead == read);
}

TEST("slack realtime: a reconnect soon after a full DM sweep asks only the most active") {
    if (!haveServer())
        return;
    // 30 DMs: more than a short sweep takes.
    std::string list = R"({"users.conversations": {"ok": true, "channels": [)";
    for (int i = 0; i < 30; ++i)
        list += str::concat(
            {i ? "," : "",
             R"({"id": "D)",
             str::number(100 + i),
             R"(", "is_im": true, "user": "U)",
             str::number(100 + i),
             "\"}"}
        );
    list += R"(], "response_metadata": {"next_cursor": ""}}})";
    Env e(false, true, {}, list.c_str());
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const auto reconnect = [&](int opens) {
        ctl("POST", "/_ctl/ws/close", R"({"code": 1001})");
        REQUIRE(pumpUntil([&] { return count("ws-open") == opens && e.socket->connected(); }));
    };
    // A sweep's calls past `from`: waited for (the paced lane stalls under
    // load), then a quiet spell for any beyond them.
    const auto swept = [](int from, int expect) {
        pumpUntil([&] { return count("conversations.info") - from >= expect; }, 5000);
        pumpFor(300);
        return count("conversations.info") - from;
    };
    // The DM activity sweep after connect first.
    int infos = swept(0, 30);
    REQUIRE(infos == 30);
    reconnect(2);
    CHECK(swept(infos, 30) == 30); // the first: every DM
    // Past the 2 min gap (1.2 s here), inside the full sweep's 10 min (6 s).
    pumpFor(1000);
    infos = count("conversations.info");
    reconnect(3);
    CHECK(swept(infos, 24) == 24);
}

TEST("slack realtime: user group events patch the groups; only the unclear reload them") {
    if (!haveServer())
        return;
    Env e(false, true, {}, R"({"usergroups.list": {"ok": true, "usergroups": [
        {"id": "S1", "handle": "eng", "name": "Eng", "users": ["UMIRA"]}]}})");
    REQUIRE(e.connect());
    REQUIRE(e.live());
    REQUIRE(pumpUntil([&] { return e.store.findUsergroup("S1") != nullptr; }, 3000));
    CHECK(e.store.myGroups.empty());
    const int lists = count("usergroups.list");
    REQUIRE(push(envelope("a", R"({"type":"subteam_self_added","subteam_id":"S1"})")));
    REQUIRE(pumpUntil([&] { return e.store.myGroups.size() == 1; }));
    REQUIRE(push(envelope(
        "b",
        R"({"type":"subteam_updated","subteam":{"id":"S1","handle":"core","name":"Core","date_delete":0}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findUsergroup("S1")->handle == "core"; }));
    CHECK(e.store.findUsergroup("S1")->mine); // no member list in it: kept
    REQUIRE(push(envelope(
        "c",
        R"({"type":"subteam_created","subteam":{"id":"S9","handle":"ops","name":"Ops","date_delete":0,"users":["UME"]}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.myGroups.size() == 2; }));
    REQUIRE(push(envelope(
        "d",
        R"({"type":"subteam_members_changed","subteam_id":"S9","added_users":[],"removed_users":["UME"]})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.myGroups.size() == 1; }));
    REQUIRE(push(envelope("e", R"({"type":"subteam_self_removed","subteam_id":"S1"})")));
    REQUIRE(pumpUntil([&] { return e.store.myGroups.empty(); }));
    pumpFor(200); // past the reload's debounce (20 ms here)
    CHECK(count("usergroups.list") == lists);
    // A group we don't hold: read them all again.
    REQUIRE(push(envelope("f", R"({"type":"subteam_self_added","subteam_id":"S5"})")));
    REQUIRE(pumpUntil([&] { return count("usergroups.list") > lists; }, 3000));
}

TEST("slack realtime: an away author posting is probed once, not per message") {
    if (!haveServer())
        return;
    Env e;
    e.be->setWindowVisible(false); // no presence rounds: only the probes count
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const auto probes = [] {
        int       n = 0;
        const Log l;
        for (json::Value r : l.doc.root())
            n += r["method"].str() == "users.getPresence" && r["form"]["user"].str() == "UMIRA";
        return n;
    };
    pumpFor(200);
    const int before = probes();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(push(envelope(
            str::concat({"m", str::number(i)}),
            str::concat(
                {R"({"type":"message","channel":"C1","user":"UMIRA","text":"hi","ts":"18000000)",
                 str::number(10 + i),
                 R"(.000100"})"}
            )
        )));
        pumpFor(100);
    }
    REQUIRE(pumpUntil([&] {
        return e.store.findMessage(e.conv("C1"), model::parseTs("1800000012.000100")) != nullptr;
    }));
    CHECK(probes() - before == 1);
    // Another workspace's message is not mapped: its author stays unknown here.
    REQUIRE(push(envelope(
        "x",
        R"({"type":"message","channel":"CZ","user":"UFOREIGN","text":"x","ts":"1800000020.000100"})",
        "T2"
    )));
    pumpFor(200);
    CHECK(e.store.findUser("UFOREIGN") == model::kNoUser);
}

TEST("slack realtime: a push workspace polls only as a safety net") {
    if (!haveServer())
        return;
    {
        Env e;
        REQUIRE(e.connect());
        REQUIRE(e.live());
        const int lists = count("users.conversations");
        pumpFor(1500); // 2.5 roster gaps at 100× speed
        CHECK(count("users.conversations") == lists);
    }
    {
        Env e(false, false);
        REQUIRE(e.connect());
        CHECK_FALSE(e.be->hasRealtimePush());
        const int lists = count("users.conversations");
        REQUIRE(pumpUntil([&] { return count("users.conversations") > lists; }, 3000));
    }
    {
        // A session workspace never takes the app's socket.
        Env e(true, true);
        CHECK_FALSE(e.be->hasRealtimePush());
        REQUIRE(e.connect());
        pumpFor(100);
        CHECK(count("apps.connections.open") == 0);
    }
}

TEST("slack realtime: a missed message re-establishes the socket") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    e.be->setActiveConversation(e.conv("C1"), 0);
    // The open chat's poll finds what the socket never pushed.
    set(R"({"conversations.history": {"ok": true, "messages": [
        {"type": "message", "user": "UMIRA", "text": "first", "ts": "1700000600.000800"}],
        "has_more": false}})");
    REQUIRE(pumpUntil(
        [&] { return e.store.findMessage(e.conv("C1"), model::parseTs("1700000600.000800")); }, 5000
    ));
    set(R"({"conversations.history": {"ok": true, "messages": [
        {"type": "message", "user": "UMIRA", "text": "missed", "ts": "1700000600.000900"},
        {"type": "message", "user": "UMIRA", "text": "first", "ts": "1700000600.000800"}],
        "has_more": false}})");
    REQUIRE(pumpUntil(
        [&] { return e.store.findMessage(e.conv("C1"), model::parseTs("1700000600.000900")); }, 5000
    ));
    REQUIRE(pumpUntil([&] { return count("apps.connections.open") == 2; }));
    REQUIRE(pumpUntil([&] { return e.socket->connected() && e.socket->socketsForTest() == 1; }));
}

TEST("slack realtime: a rejected token is refreshed once and the call re-issued") {
    if (!haveServer())
        return;
    slack::Credentials cr;
    cr.refreshToken = "xoxe-1";
    Env e(false, false, cr);
    script("auth.test", R"([{"ok": false, "error": "invalid_auth"}])");
    set(R"({"oauth.v2.access": {"ok": true, "access_token": "xoxp-new", "refresh_token": "xoxe-2",
            "expires_in": 43200}})");
    REQUIRE(e.connect());
    CHECK(e.lostAuth.empty());
    Log log;
    REQUIRE(log.count("oauth.v2.access") == 1);
    const json::Value form = log.get("oauth.v2.access")["form"];
    CHECK_STR(std::string(form["grant_type"].str()), "refresh_token");
    CHECK_STR(std::string(form["client_id"].str()), "cid");
    CHECK_STR(std::string(form["client_secret"].str()), "csecret");
    CHECK_STR(std::string(form["refresh_token"].str()), "xoxe-1");
    // auth.test went again, with the new token; so does everything after.
    CHECK(log.count("auth.test") == 2);
    CHECK_STR(std::string(log.get("auth.test", 1)["auth"].str()), "Bearer xoxp-new");
    CHECK_STR(std::string(log.get("users.list")["auth"].str()), "Bearer xoxp-new");
    // Persisted through the owner.
    REQUIRE(e.saved.size() == 1);
    CHECK_STR(e.saved[0].token, "xoxp-new");
    CHECK_STR(e.saved[0].refreshToken, "xoxe-2");
    CHECK(e.saved[0].expiresAt > base::nowSecs() + 43000);
}

TEST(
    "slack realtime: calls wait while the token refreshes; a late rejection doesn't refresh again"
) {
    if (!haveServer())
        return;
    slack::Credentials cr;
    cr.refreshToken = "xoxe-1";
    Env e(false, false, cr);
    REQUIRE(e.connect());
    const ConvRef c1 = e.conv("C1"), d1 = e.conv("D1");
    REQUIRE(c1 != kNoConv && d1 != kNoConv);
    // The token is refused; the refresh takes a while to answer. The second
    // call, out on the old token too, hears so only after the refresh
    // started: it must not spend the next refresh token.
    set(R"({"oauth.v2.access": {"ok": true, "access_token": "xoxp-new", "refresh_token": "xoxe-2",
            "expires_in": 43200, "__delay": 0.4}})");
    script("stars.add", R"([{"ok": false, "error": "invalid_auth"},
                            {"ok": false, "error": "invalid_auth", "__delay": 0.15},
                            {"ok": true}])");
    e.be->setStarred(c1, true);
    e.be->setStarred(d1, true);
    REQUIRE(pumpUntil([&] { return Log().count("oauth.v2.access") == 1; }));
    // Issued during the refresh: held, then sent on the new token.
    e.be->setStarred(c1, false);
    REQUIRE(pumpUntil([&] { return Log().count("stars.add") == 4; }, 5000));
    REQUIRE(pumpUntil([&] { return Log().count("stars.remove") == 1; }, 5000));
    pumpFor(200);
    Log log;
    CHECK(log.count("oauth.v2.access") == 1);
    for (int i = 2; i < 4; ++i)
        CHECK_STR(std::string(log.get("stars.add", i)["auth"].str()), "Bearer xoxp-new");
    CHECK_STR(std::string(log.get("stars.remove")["auth"].str()), "Bearer xoxp-new");
    CHECK(e.lostAuth.empty());
}

TEST("slack realtime: a dead refresh token signs out; a transient failure doesn't") {
    if (!haveServer())
        return;
    slack::Credentials cr;
    cr.refreshToken = "xoxe-1";
    {
        Env e(false, false, cr);
        script("auth.test", R"([{"ok": false, "error": "token_expired"}])");
        set(R"({"oauth.v2.access": {"ok": false, "error": "invalid_refresh_token"}})");
        bool        called = false;
        std::string err;
        e.be->connect([&](bool, const std::string &x) {
            called = true;
            err    = x;
        });
        REQUIRE(pumpUntil([&] { return called && !e.lostAuth.empty(); }));
        CHECK_STR(e.lostAuth, "invalid_refresh_token");
        CHECK_STR(err, "token_expired"); // what the call itself got: a dead sign-in
    }
    {
        Env e(false, false, cr);
        script("auth.test", R"([{"ok": false, "error": "invalid_auth"}])");
        set(R"({"oauth.v2.access": {"ok": false, "error": "internal_error"}})");
        std::string err;
        bool        called = false;
        e.be->connect([&](bool, const std::string &x) {
            called = true;
            err    = x;
        });
        REQUIRE(pumpUntil([&] { return called; }));
        CHECK_STR(err, "token_refresh_failed");
        pumpFor(100);
        CHECK(e.lostAuth.empty());
    }
    {
        // Still rejected after a "successful" refresh: no second refresh.
        Env e(false, false, cr);
        script("auth.test", R"([{"ok": false, "error": "invalid_auth"},
                                {"ok": false, "error": "invalid_auth"}])");
        set(R"({"oauth.v2.access": {"ok": true, "access_token": "xoxp-new"}})");
        bool called = false;
        e.be->connect([&](bool, const std::string &) { called = true; });
        REQUIRE(pumpUntil([&] { return called && !e.lostAuth.empty(); }));
        CHECK(count("oauth.v2.access") == 1);
        CHECK_STR(e.lostAuth, "invalid_auth");
    }
}

TEST("slack realtime: a token close to expiry is refreshed ahead of time") {
    if (!haveServer())
        return;
    slack::Credentials cr;
    cr.refreshToken = "xoxe-1";
    cr.expiresAt    = base::nowSecs() + 600; // inside the 1 h window
    Env e(
        false,
        false,
        cr,
        R"({"oauth.v2.access": {"ok": true, "access_token": "xoxp-new", "expires_in": 43200}})"
    );
    REQUIRE(pumpUntil([&] { return e.saved.size() == 1; }));
    CHECK_STR(e.be->auth().token, "xoxp-new");
    // Healthy now: the periodic check leaves it alone.
    pumpFor(1500);
    CHECK(count("oauth.v2.access") == 1);
}

TEST("slack realtime: the presence link holds an RTM socket with the d cookie") {
    if (!haveServer())
        return;
    Env                 e(true, false);
    slack::RtmPresence *rtm = e.be->presenceLinkForTest();
    REQUIRE(rtm != nullptr);
    CHECK(e.be->presenceLink() == Link::Off);
    e.be->setPresenceMode(Mode::WhileRunning);
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Active; }));
    Log log;
    CHECK_STR(std::string(log.get("ws-open")["cookie"].str()), "d=xoxd-test");
    CHECK_STR(std::string(log.get("rtm.connect")["auth"].str()), "Bearer xoxc-test");
    // Active at once: a tickle right after hello.
    REQUIRE(pumpUntil([&] { return Log().frame("ws:rtm", "tickle"); }));
    // Native keeps the socket (live delivery): presence Off.
    e.be->setPresenceMode(Mode::Native);
    CHECK(e.be->presenceLink() == Link::Off);
    CHECK(rtm->connected());
    CHECK(e.be->holdsPresenceLink());
    // An OAuth workspace has none.
    Env o(false, false);
    CHECK(o.be->presenceLinkForTest() == nullptr);
    CHECK_FALSE(o.be->holdsPresenceLink());
}

TEST("slack realtime: the presence link reconnects without pongs and idles while unused") {
    if (!haveServer())
        return;
    Env                        e(true, false);
    slack::RtmPresence        *rtm = e.be->presenceLinkForTest();
    slack::RtmPresence::Timing t;
    t.pingMs         = 30;
    t.reconnectMinMs = 20;
    t.reconnectMaxMs = 100;
    t.idleMs         = 400;
    rtm->setTimingForTest(t);
    ctl("POST", "/_ctl/ws/opt", R"({"rtm_nopong": true})");
    e.be->setPresenceMode(Mode::WhileRunning);
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Active; }));
    // Two unanswered pings: a fresh rtm.connect.
    REQUIRE(pumpUntil([&] { return count("rtm.connect") >= 2; }, 5000));
    ctl("POST", "/_ctl/ws/opt", R"({"rtm_nopong": false})");
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Active && rtm->connected(); }));
    // WhileUsing: no input for idleMs idles it (the socket stays: live
    // delivery); input makes it active again.
    e.be->setPresenceMode(Mode::WhileUsing);
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Idle; }, 5000));
    CHECK(rtm->connected());
    e.be->noteUserActivity();
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Active; }));
}

TEST("rtm presence: native holds the socket without tickles") {
    if (!haveServer())
        return;
    Env                 e(true, false);
    slack::RtmPresence *rtm = e.be->presenceLinkForTest();
    REQUIRE(rtm != nullptr);
    slack::RtmPresence::Timing t;
    t.alwaysTickleMs = 50;
    rtm->setTimingForTest(t);
    bool hello   = false;
    rtm->onHello = [&](bool) { hello = true; };
    rtm->start();
    REQUIRE(pumpUntil([&] { return rtm->connected() && hello; }));
    CHECK(e.be->presenceLink() == Link::Off);
    pumpFor(2 * t.alwaysTickleMs);
    CHECK_FALSE(Log().frame("ws:rtm", "tickle"));
    CHECK_STR(std::string(Log().get("ws-open")["cookie"].str()), "d=xoxd-test");
    // A holding mode reuses the socket: active at once, no second rtm.connect.
    e.be->setPresenceMode(Mode::WhileRunning);
    CHECK(e.be->presenceLink() == Link::Active);
    REQUIRE(pumpUntil([&] { return tickles() >= 2; })); // the at-once one + the clock
    CHECK(count("rtm.connect") == 1);
}

TEST("rtm presence: while-using idles without dropping the socket") {
    if (!haveServer())
        return;
    Env                        e(true, false);
    slack::RtmPresence        *rtm = e.be->presenceLinkForTest();
    slack::RtmPresence::Timing t;
    t.idleMs = 300;
    rtm->setTimingForTest(t);
    rtm->start();
    e.be->setPresenceMode(Mode::WhileUsing);
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Active; }));
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Idle; }, 5000));
    CHECK(rtm->connected());
    const int n = tickles();
    pumpFor(200);
    CHECK(tickles() == n);
    e.be->noteUserActivity();
    CHECK(e.be->presenceLink() == Link::Active);
    REQUIRE(pumpUntil([&] { return tickles() > n; }));
    CHECK(count("rtm.connect") == 1);
}

TEST("rtm presence: event frames reach onEvent, hello reports gaps") {
    if (!haveServer())
        return;
    Env                        e(true, false);
    slack::RtmPresence        *rtm = e.be->presenceLinkForTest();
    slack::RtmPresence::Timing t;
    t.pingMs         = 30;
    t.reconnectMinMs = 20;
    t.reconnectMaxMs = 100;
    rtm->setTimingForTest(t);
    std::vector<bool>        gaps;
    std::vector<std::string> events;
    rtm->onHello = [&](bool afterGap) { gaps.push_back(afterGap); };
    rtm->onEvent = [&](const json::Value &f) {
        events.push_back(std::string(f["type"].str()) + ":" + std::string(f["channel"].str()));
    };
    rtm->start();
    REQUIRE(pumpUntil([&] { return gaps.size() == 1; }));
    CHECK_FALSE(gaps[0]);
    REQUIRE(push(R"({"type":"message","channel":"C1","ts":"1.0","text":"x"})", rtmConn()));
    REQUIRE(pumpUntil([&] { return !events.empty(); }));
    CHECK_STR(events[0], "message:C1");
    REQUIRE(push(R"({"type":"pong","reply_to":1})", rtmConn()));
    pumpFor(100);
    CHECK(events.size() == 1);
    // A drop (pongs stop): the next hello says there was a gap.
    ctl("POST", "/_ctl/ws/opt", R"({"rtm_nopong": true})");
    REQUIRE(pumpUntil([&] { return count("rtm.connect") >= 2; }, 5000));
    ctl("POST", "/_ctl/ws/opt", R"({"rtm_nopong": false})");
    REQUIRE(pumpUntil([&] { return gaps.size() >= 2; }));
    CHECK(gaps[1]);
    CHECK(events.size() == 1);
}

TEST("slack realtime: rtm.connect refusing the token makes the link unavailable") {
    if (!haveServer())
        return;
    Env e(true, false, {}, R"({"rtm.connect": {"ok": false, "error": "not_allowed_token_type"}})");
    e.be->setPresenceMode(Mode::WhileRunning);
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Unavailable; }));
    pumpFor(200);
    CHECK(count("rtm.connect") == 1); // given up, not retried into a rate limit
}

// A session workspace whose RTM link is up: live delivery.
static bool rtmLive(Env &e) {
    return pumpUntil([&] { return e.be->hasRealtimePush() && !rtmConn().empty(); }, 5000);
}

// A bare RTM frame on the newest RTM socket.
static bool rtmPush(const std::string &frame) {
    return push(frame, rtmConn());
}

TEST("live rtm: a message frame reaches the Store without history calls") {
    if (!haveServer())
        return;
    Env e(true, false);
    REQUIRE(e.connect());
    REQUIRE(rtmLive(e));
    const int     history = count("conversations.history");
    const ConvRef c       = e.conv("C1");
    const Ts      ts      = model::parseTs("1800000000.000100");
    REQUIRE(rtmPush(
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"hi <@UME>","ts":"1800000000.000100"})"
    ));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts); }));
    CHECK(e.store.conversation(c).mentions == 1);
    CHECK(count("conversations.history") == history);
}

TEST("live rtm: edits, deletions and reactions apply") {
    if (!haveServer())
        return;
    Env e(true, false);
    REQUIRE(e.connect());
    REQUIRE(rtmLive(e));
    const ConvRef c  = e.conv("C1");
    const Ts      ts = model::parseTs("1800000000.000200");
    REQUIRE(rtmPush(
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"v1","ts":"1800000000.000200"})"
    ));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts); }));
    REQUIRE(rtmPush(
        R"({"type":"message","subtype":"message_changed","channel":"C1","message":{"type":"message","user":"UMIRA","text":"v2","ts":"1800000000.000200","edited":{"ts":"1"}}})"
    ));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts)->text == "v2"; }));
    CHECK(e.store.findMessage(c, ts)->edited);
    REQUIRE(rtmPush(
        R"({"type":"reaction_added","user":"UMIRA","reaction":"tada","item":{"type":"message","channel":"C1","ts":"1800000000.000200"}})"
    ));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts)->reactions.size() == 1; }));
    REQUIRE(rtmPush(
        R"({"type":"reaction_removed","user":"UMIRA","reaction":"tada","item":{"type":"message","channel":"C1","ts":"1800000000.000200"}})"
    ));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts)->reactions.empty(); }));
    REQUIRE(rtmPush(
        R"({"type":"message","subtype":"message_deleted","channel":"C1","deleted_ts":"1800000000.000200","previous_message":{"ts":"1800000000.000200"}})"
    ));
    REQUIRE(pumpUntil([&] { return !e.store.findMessage(c, ts); }));
}

TEST("live rtm: a message in an unknown DM surfaces") {
    if (!haveServer())
        return;
    Env e(true, false);
    REQUIRE(e.connect());
    REQUIRE(rtmLive(e));
    set(R"({"conversations.info?channel=D9": {"ok": true, "channel": {"id": "D9", "is_im": true,
        "user": "UMIRA", "latest": {"ts": "1800000000.000300"}}}})");
    REQUIRE(e.conv("D9") == kNoConv);
    REQUIRE(rtmPush(
        R"({"type":"message","channel":"D9","user":"UMIRA","text":"psst","ts":"1800000000.000300"})"
    ));
    REQUIRE(pumpUntil([&] { return e.conv("D9") != kNoConv; }));
    REQUIRE(pumpUntil([&] {
        return e.store.findMessage(e.conv("D9"), model::parseTs("1800000000.000300"));
    }));
}

TEST("live rtm: event then poll does not duplicate") {
    if (!haveServer())
        return;
    Env e(true, false);
    REQUIRE(e.connect());
    REQUIRE(rtmLive(e));
    const ConvRef c  = e.conv("C1");
    const Ts      ts = model::parseTs("1800000000.000400");
    REQUIRE(rtmPush(
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"once","ts":"1800000000.000400"})"
    ));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts); }));
    set(R"({"conversations.history": {"ok": true, "messages": [
        {"type": "message", "user": "UMIRA", "text": "once", "ts": "1800000000.000400"}],
        "has_more": false}})");
    const int history = count("conversations.history");
    e.be->setActiveConversation(c, 0);
    REQUIRE(pumpUntil([&] { return count("conversations.history") >= history + 2; }, 5000));
    pumpFor(100);
    int n = 0;
    for (const model::Message &m : e.store.conversation(c).messages)
        n += m.ts == ts;
    CHECK(n == 1);
}

TEST("live rtm: control frames are ignored") {
    if (!haveServer())
        return;
    Env e(true, false);
    REQUIRE(e.connect());
    REQUIRE(rtmLive(e));
    const ConvRef c      = e.conv("C1");
    const size_t  held   = e.store.conversation(c).messages.size();
    const size_t  convs  = e.store.conversationCount();
    const auto    mira   = e.store.findUser("UMIRA");
    const bool    active = e.store.user(mira).active;
    // A reply ack (no type, but a ts and text) must not become a message.
    REQUIRE(rtmPush(R"({"ok":true,"reply_to":7,"ts":"1800000000.000500","text":"sent"})"));
    REQUIRE(rtmPush(R"({"type":"reconnect_url","url":"wss://x"})"));
    REQUIRE(rtmPush(R"({"type":"presence_change","user":"UOTHER","presence":"away"})"));
    REQUIRE(rtmPush(R"({"type":"no_such_event","channel":"C1","ts":"1800000000.000600"})"));
    REQUIRE(rtmPush("not json at all"));
    REQUIRE(rtmPush("[1,2,3]"));
    REQUIRE(rtmPush(R"({"type":"error","error":{"code":1,"msg":"x"}})")); // last: it drops the link
    pumpFor(200);
    CHECK(e.store.conversation(c).messages.size() == held);
    CHECK(e.store.conversationCount() == convs);
    CHECK(e.store.user(mira).active == active);
    CHECK(e.store.findUser("UOTHER") == model::kNoUser);
}

TEST("live rtm: hasRealtimePush follows the link") {
    if (!haveServer())
        return;
    Env                 e(true, false);
    slack::RtmPresence *rtm = e.be->presenceLinkForTest();
    REQUIRE(rtm != nullptr);
    slack::RtmPresence::Timing t;
    t.pingMs         = 30;
    t.reconnectMinMs = 2'000; // stays down long enough to see
    rtm->setTimingForTest(t);
    CHECK_FALSE(e.be->hasRealtimePush());
    REQUIRE(e.connect());
    // Held in Native (the default mode) too: presence stays Off.
    REQUIRE(pumpUntil([&] { return e.be->hasRealtimePush(); }));
    CHECK(e.be->presenceLink() == Link::Off);
    ctl("POST", "/_ctl/ws/opt", R"({"rtm_nopong": true})");
    REQUIRE(pumpUntil([&] { return !e.be->hasRealtimePush(); }, 5000));
    ctl("POST", "/_ctl/ws/opt", R"({"rtm_nopong": false})");
}

TEST("live rtm: a refused rtm.connect is no push; polling continues") {
    if (!haveServer())
        return;
    Env e(
        true,
        false,
        {},
        R"({"rtm.connect": {"ok": false, "error": "not_allowed_token_type"},
              "client.counts": {"ok": true, "channels": [], "ims": [], "mpims": []}})"
    );
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return count("rtm.connect") == 1; }));
    CHECK_FALSE(e.be->hasRealtimePush());
    const int counts = count("client.counts");
    REQUIRE(pumpUntil([&] { return count("client.counts") > counts; }, 3000));
    CHECK_FALSE(e.be->hasRealtimePush());
    CHECK(count("rtm.connect") == 1);
}

TEST("live rtm: a missed message re-establishes the link, throttled") {
    if (!haveServer())
        return;
    Env                        e(true, false);
    slack::RtmPresence        *rtm = e.be->presenceLinkForTest();
    slack::RtmPresence::Timing t;
    t.reconnectMinMs = 20;
    t.reconnectMaxMs = 100;
    rtm->setTimingForTest(t);
    REQUIRE(e.connect());
    REQUIRE(rtmLive(e));
    const ConvRef c = e.conv("C1");
    // Opened with it already there: loaded, not missed.
    set(R"({"conversations.history": {"ok": true, "messages": [
        {"type": "message", "user": "UMIRA", "text": "first", "ts": "1700000600.000800"}],
        "has_more": false}})");
    e.be->setActiveConversation(c, 0);
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, model::parseTs("1700000600.000800")); }));
    const int connects = count("rtm.connect");
    // The open chat's poll finds what the socket never pushed: re-established.
    set(R"({"conversations.history": {"ok": true, "messages": [
        {"type": "message", "user": "UMIRA", "text": "missed", "ts": "1700000600.000900"},
        {"type": "message", "user": "UMIRA", "text": "first", "ts": "1700000600.000800"}],
        "has_more": false}})");
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, model::parseTs("1700000600.000900")); }));
    REQUIRE(pumpUntil([&] { return count("rtm.connect") == connects + 1 && rtm->connected(); }));
    // A second miss inside kReestablishGapMs (the live safety poll is that
    // slow: showing the window again polls at once): no second reconnect.
    set(R"({"conversations.history": {"ok": true, "messages": [
        {"type": "message", "user": "UMIRA", "text": "again", "ts": "1700000600.001000"},
        {"type": "message", "user": "UMIRA", "text": "missed", "ts": "1700000600.000900"},
        {"type": "message", "user": "UMIRA", "text": "first", "ts": "1700000600.000800"}],
        "has_more": false}})");
    e.be->setWindowVisible(false);
    e.be->setWindowVisible(true);
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, model::parseTs("1700000600.001000")); }));
    pumpFor(100);
    CHECK(count("rtm.connect") == connects + 1);
}

// A message `ago` seconds old, as a history answer (newest first) over "first".
static std::string historyWith(int64_t ago) {
    return str::concat(
        {R"({"conversations.history": {"ok": true, "messages": [
        {"type": "message", "user": "UMIRA", "text": "late", "ts": ")",
         str::number(base::nowSecs() - ago),
         R"(.000100"},
        {"type": "message", "user": "UMIRA", "text": "first", "ts": "1700000600.000800"}],
        "has_more": false}})"}
    );
}

TEST("live rtm: a poll that beats its event is no miss; an old find is") {
    if (!haveServer())
        return;
    for (const int64_t ago : {0, 30}) {
        Env                        e(true, false);
        slack::RtmPresence::Timing t;
        t.reconnectMinMs = 20;
        e.be->presenceLinkForTest()->setTimingForTest(t);
        REQUIRE(e.connect());
        REQUIRE(rtmLive(e));
        const ConvRef c = e.conv("C1");
        set(R"({"conversations.history": {"ok": true, "messages": [
            {"type": "message", "user": "UMIRA", "text": "first", "ts": "1700000600.000800"}],
            "has_more": false}})");
        e.be->setActiveConversation(c, 0);
        REQUIRE(pumpUntil([&] {
            return e.store.findMessage(c, model::parseTs("1700000600.000800"));
        }));
        const int connects = count("rtm.connect");
        // The poll's answer lands before the RTM frame (ts = now), or brings
        // one 30 s old the socket never pushed.
        set(historyWith(ago));
        const size_t held = e.store.conversation(c).messages.size();
        e.be->setWindowVisible(false);
        e.be->setWindowVisible(true); // polls at once
        REQUIRE(pumpUntil([&] { return e.store.conversation(c).messages.size() > held; }));
        pumpFor(200);
        CHECK(count("rtm.connect") == connects + (ago ? 1 : 0));
    }
}

TEST("live rtm: a link dropping after each hello backs off and backfills once per 30 s") {
    if (!haveServer())
        return;
    Env e(true, false);
    // 10x, not 100x: 30 s is 3 s, well past the reconnects below however
    // the scheduler stalls.
    e.be.reset();
    base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", "10");
    slack::Credentials cr;
    cr.token  = "xoxc-test";
    cr.cookie = "xoxd-test";
    cr.teamId = "T1";
    e.be      = std::make_unique<slack::SlackBackend>(e.store, app(), e.client, cr);
    slack::RtmPresence        *rtm = e.be->presenceLinkForTest();
    slack::RtmPresence::Timing t;
    t.reconnectMinMs = 40;
    t.reconnectMaxMs = 1'000;
    rtm->setTimingForTest(t);
    REQUIRE(e.connect());
    REQUIRE(rtmLive(e));
    // Hidden: the open chat polls once a minute (6 s here), so its history
    // calls below are the backfills alone.
    e.be->setWindowVisible(false);
    const ConvRef c = e.conv("C1");
    e.be->setActiveConversation(c, 0);
    pumpFor(100);
    const auto history = [] {
        int       n = 0;
        const Log log;
        for (json::Value r : log.doc.root())
            n += r["method"].str() == "conversations.history" && r["form"]["channel"].str() == "C1";
        return n;
    };
    const int            h0 = history();
    // Three drops right after hello: back 40, 80, 160 ms later (not 40 each),
    // all inside 30 s (3 s here) of the first backfill.
    std::vector<int64_t> gaps;
    for (int i = 0; i < 3; ++i) {
        const int     opens = count("ws-open");
        const int64_t at    = base::monotonicMs();
        ctl("POST", "/_ctl/ws/close", "{\"conn\":\"" + rtmConn() + "\",\"code\":1001}");
        REQUIRE(
            pumpUntil([&] { return count("ws-open") > opens && e.be->hasRealtimePush(); }, 3000)
        );
        gaps.push_back(base::monotonicMs() - at);
    }
    CHECK(gaps[2] >= 140);
    pumpFor(50);
    CHECK(history() - h0 == 1);
}

TEST("slack realtime: a huddle_thread starts a live huddle, its edit ends it") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef c = e.conv("C1");
    REQUIRE(push(envelope(
        "h-1",
        R"({"type":"message","subtype":"huddle_thread","channel":"C1","ts":"1800000000.000700",
            "text":"","room":{"call_family":"huddle","huddle_link":"https://app.slack.com/huddle/T1/C1",
            "participants":["UMIRA"],"date_start":1800000000,"date_end":0}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.conversation(c).huddleActive; }));
    CHECK_STR(e.store.conversation(c).huddleLink, "https://app.slack.com/huddle/T1/C1");
    // The message itself arrives too: a huddle row (no Slackbot author).
    const model::Message *m = e.store.findMessage(c, model::parseTs("1800000000.000700"));
    REQUIRE(m && m->isHuddle());
    CHECK(!m->extra->huddle.ended);
    // Slack edits it when the huddle ends (date_end as a string).
    REQUIRE(push(envelope(
        "h-2",
        R"({"type":"message","subtype":"message_changed","channel":"C1",
            "message":{"type":"message","subtype":"huddle_thread","ts":"1800000000.000700","text":"",
            "room":{"call_family":"huddle","participants":[],"participant_history":["UMIRA","UME"],
                    "date_start":1800000000,"date_end":"1800000600"}}})"
    )));
    REQUIRE(pumpUntil([&] { return !e.store.conversation(c).huddleActive; }));
    m = e.store.findMessage(c, model::parseTs("1800000000.000700"));
    REQUIRE(m && m->isHuddle());
    CHECK(m->extra->huddle.ended);
    CHECK(m->extra->huddle.attendees.size() == 2);
}

TEST("slack realtime: a conversation found dead is never looked up again, across restarts") {
    if (!haveServer())
        return;
    cache::WorkspaceCache::remove(app(), "slack:T1");
    {
        Env e;
        e.be->openCache(cache::WorkspaceCache::dirFor(app(), "slack:T1"));
        REQUIRE(e.connect());
        REQUIRE(e.live());
        REQUIRE(push(envelope(
            "d-1",
            R"({"type":"message","channel":"CX","user":"UX","text":"1","ts":"1800000000.000800"})"
        )));
        REQUIRE(pumpUntil([&] { return e.be->isDead("CX"); }));
    }
    Env e;
    e.be->openCache(cache::WorkspaceCache::dirFor(app(), "slack:T1"));
    REQUIRE(e.connect());
    REQUIRE(e.live());
    REQUIRE(push(envelope(
        "d-2",
        R"({"type":"message","channel":"CX","user":"UX","text":"2","ts":"1800000000.000900"})"
    )));
    fakeslack::pumpFor(200);
    // (the DM sweep may ask about D1; nothing asks about CX)
    int       asked = 0;
    const Log log; // outlives the loop (a temporary in the range would not)
    for (json::Value r : log.doc.root())
        asked += r["method"].str() == "conversations.info" && r["form"]["channel"].str() == "CX";
    CHECK(asked == 0);
    e.be.reset();
    cache::WorkspaceCache::remove(app(), "slack:T1");
}

TEST("slack realtime: marked events preserve newest local read and mark-unread intent") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef        d1  = e.conv("D1");
    const model::UserRef mr  = e.store.findUser("UMIRA");
    const Ts      tsA = model::parseTs("1800000000.000100");
    const Ts      tsB = model::parseTs("1800000000.000200");
    const Ts      tsC = model::parseTs("1800000000.000300");
    auto          add = [&](ConvRef c, Ts ts, const char *txt) {
        model::Message m;
        m.ts   = ts;
        m.user = mr;
        m.text = txt;
        e.store.addMessage(c, std::move(m));
    };
    add(d1, tsA, "one");
    add(d1, tsB, "two");

    // 1. Read A, Read B, then an older marked event for A: keep B and zero counts.
    script("conversations.mark", R"([{"ok":true,"__delay":0.15},{"ok":true}])");
    e.be->markRead(d1, tsA);
    e.be->markRead(d1, tsB);
    REQUIRE(push(envelope(
        "m1",
        R"({"type":"im_marked","channel":"D1","ts":"1800000000.000100","unread_count_display":1,"mention_count_display":1})"
    )));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).lastRead == tsB);
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(e.store.conversation(d1).mentions == 0);
    REQUIRE(pumpUntil([&] { return count("conversations.mark") == 2; }));

    // 2. Read C, Mark unread A, then the earlier read echo for C: keep A-1 and unread badge.
    add(d1, tsC, "three");
    script("conversations.mark", R"([{"ok":true,"__delay":0.15},{"ok":true}])");
    e.be->markRead(d1, tsC);
    e.be->markUnread(d1, tsA);
    REQUIRE(push(envelope(
        "m2",
        R"({"type":"im_marked","channel":"D1","ts":"1800000000.000300","unread_count_display":0,"mention_count_display":0})"
    )));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).lastRead == tsA - 1);
    CHECK(e.store.conversation(d1).unread == 3);
    REQUIRE(pumpUntil([&] { return count("conversations.mark") == 4; }));
    // Even after HTTP mark queue drains, a delayed echo for C must not undo A-1.
    script(
        "conversations.info",
        R"([{"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA","unread_count":3,"unread_count_display":3,"last_read":"1800000000.000099","latest":{"ts":"1800000000.000300"}}}])"
    );
    script(
        "conversations.history",
        R"([{"ok":true,"has_more":false,"messages":[{"type":"message","ts":"1800000000.000300","user":"UMIRA","text":"unread"}]}])"
    );
    REQUIRE(push(envelope(
        "m2b",
        R"({"type":"im_marked","channel":"D1","ts":"1800000000.000300","unread_count_display":0,"mention_count_display":0})"
    )));
    fakeslack::pumpFor(100);
    CHECK(e.store.conversation(d1).lastRead == tsA - 1);
    CHECK(e.store.conversation(d1).unread == 3);

    // 3. Mark unread A, Read C, then the earlier unread echo: keep C with no stale badge.
    script("conversations.mark", R"([{"ok":true,"__delay":0.15},{"ok":true}])");
    e.be->markUnread(d1, tsA);
    e.be->markRead(d1, tsC);
    REQUIRE(push(envelope(
        "m3",
        R"({"type":"im_marked","channel":"D1","ts":"1800000000.000099","unread_count_display":3,"mention_count_display":3})"
    )));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).lastRead == tsC);
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(e.store.conversation(d1).mentions == 0);
    REQUIRE(pumpUntil([&] { return count("conversations.mark") == 6; }));

    // Confirm C echo so no local intent is pending.
    REQUIRE(push(envelope(
        "m3b",
        R"({"type":"im_marked","channel":"D1","ts":"1800000000.000300","unread_count_display":0,"mention_count_display":0})"
    )));
    fakeslack::pumpFor(50);

    // 4. No local pending intent: legitimate remote Mark unread lowers cursor
    // without replaying Append/Arrived banners; remote read clears badge.
    int appends = 0;
    const auto obs = e.store.observe(d1, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Append || ch.kind == model::ChangeKind::Arrived)
            ++appends;
    });
    script(
        "conversations.info",
        R"([{"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA","unread_count":3,"unread_count_display":3,"last_read":"1800000000.000099","latest":{"ts":"1800000000.000300"}}}])"
    );
    script(
        "conversations.history",
        R"([{"ok":true,"has_more":false,"messages":[{"type":"message","ts":"1800000000.000300","user":"UMIRA","text":"unread"}]}])"
    );
    REQUIRE(push(envelope(
        "m4",
        R"({"type":"im_marked","channel":"D1","ts":"1800000000.000099","unread_count_display":3,"mention_count_display":3})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.conversation(d1).lastRead == tsA - 1; }));
    CHECK(e.store.conversation(d1).unread >= 1);
    CHECK(appends == 0);
    e.store.unobserve(obs);

    REQUIRE(push(envelope(
        "m5",
        R"({"type":"im_marked","channel":"D1","ts":"1800000000.000300","unread_count_display":0,"mention_count_display":0})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.conversation(d1).lastRead == tsC; }));
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(e.store.conversation(d1).mentions == 0);

    // 5. Group DM isolation: pending intent on D1 does not block mpim_marked on G9.
    REQUIRE(push(envelope(
        "m6",
        R"({"type":"message","channel":"G9","user":"UMIRA","text":"hi group","ts":"1800000000.000400"})"
    )));
    REQUIRE(pumpUntil([&] { return e.conv("G9") != kNoConv; }));
    const ConvRef g9 = e.conv("G9");
    script("conversations.mark", R"([{"ok":true,"__delay":0.15}])");
    e.be->markUnread(d1, tsA);
    REQUIRE(push(envelope(
        "m7",
        R"({"type":"mpim_marked","channel":"G9","ts":"1800000000.000400","unread_count_display":0,"mention_count_display":0})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.conversation(g9).lastRead == model::parseTs("1800000000.000400"); }));
    CHECK(e.store.conversation(g9).unread == 0);
    CHECK(e.store.conversation(d1).lastRead == tsA - 1);
    REQUIRE(pumpUntil([&] { return count("conversations.mark") == 7; }));
}

TEST("dm zero sources: reconnect patch clears own-newer zero, preserves missing, and respects post-HTTP markUnread") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    REQUIRE(push(envelope(
        "seed-g9",
        R"({"type":"message","channel":"G9","user":"UMIRA","text":"hi group","ts":"1800000000.000100"})"
    )));
    REQUIRE(pumpUntil([&] { return e.conv("G9") != kNoConv; }));
    const ConvRef d1 = e.conv("D1");
    const ConvRef d2 = e.conv("G9");
    REQUIRE(d1 != kNoConv);
    REQUIRE(d2 != kNoConv);

    const Ts ts200 = model::parseTs("1800000000.000200");
    const Ts ts300 = model::parseTs("1800000000.000300");

    // Seed D1 with stale unread=1 at 199 and own newer at 300; D2 with backlog=4.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts200 - 1;
        c.latest   = ts300;
        c.unread   = 1;
        c.mentions = 1;
    });
    e.store.updateConversation(d2, [&](model::Conversation &c) {
        c.lastRead = ts200;
        c.latest   = ts300;
        c.unread   = 4;
        c.mentions = 1;
    });
    model::Message mD2;
    mD2.ts   = ts300;
    mD2.user = e.store.findUser("UEXT");
    mD2.text = "d2 msg";
    e.store.addMessage(d2, std::move(mD2));

    // Reconnect resyncUnreads fetches conversations.info:
    // D1 gets explicit zero at last_read=200, latest=300.
    // D2 gets missing counts (must preserve unread=4), and we also test local markUnread after HTTP
    // completes but before the batched patch timer applies it.
    set(R"({
        "conversations.info?channel=D1":{"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA",
            "last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"},
            "unread_count":0,"unread_count_display":0,"mention_count":0,"mention_count_display":0}},
        "conversations.info?channel=G9":{"ok":true,"channel":{"id":"G9","is_mpim":true,"name":"mpdm-me--mira-1",
            "last_read":"1800000000.000300","latest":{"ts":"1800000000.000300"},
            "unread_count":0,"unread_count_display":0,"mention_count":0,"mention_count_display":0}}
    })");

    const int infoBefore = count("conversations.info");
    // Drop and reconnect socket to trigger resyncUnreads.
    REQUIRE(push(R"({"type":"disconnect","reason":"refresh_requested"})"));
    REQUIRE(pumpUntil([&] { return count("conversations.info") >= infoBefore + 2; }, 5000));
    // Immediately mark D2 unread after HTTP completed, before the batched patch timer fires!
    e.be->markUnread(d2, ts300);
    REQUIRE(pumpUntil([&] { return e.store.conversation(d1).lastRead == ts200; }, 3000));
    fakeslack::pumpFor(200);
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(e.store.conversation(d1).mentions == 0);
    CHECK(e.store.conversation(d2).lastRead == ts300 - 1);
    CHECK(e.store.conversation(d2).unread >= 1);
}

TEST("dm zero events: remote read clears own-newer DM via reconciliation without message replay") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef d1 = e.conv("D1");
    REQUIRE(d1 != kNoConv);
    const Ts incoming = model::parseTs("1800000000.000200");
    const Ts own      = model::parseTs("1800000000.000300");

    model::Message received;
    received.ts   = incoming;
    received.user = e.store.findUser("UMIRA");
    received.text = "message read in the other client";
    e.store.addMessage(d1, std::move(received));
    model::Message sent;
    sent.ts   = own;
    sent.user = e.store.me;
    sent.text = "my newer reply";
    e.store.addMessage(d1, std::move(sent));
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = incoming - 1;
        c.latest   = own;
        c.unread = c.mentions = 1;
    });

    int appends = 0;
    e.store.observe(d1, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Append)
            ++appends;
    });

    script("conversations.info", R"([{"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA",
        "last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"},
        "unread_count":0,"unread_count_display":0,"mention_count":0,"mention_count_display":0}}])");
    script("conversations.history", R"([{"ok":true,"has_more":false,"messages":[
        {"type":"message","ts":"1800000000.000300","user":"UME","text":"my newer reply"}]}])");

    REQUIRE(push(envelope("remote-read-own-newer",
        R"({"type":"im_marked","channel":"D1","ts":"1800000000.000200",
        "unread_count_display":0,"mention_count_display":0})")));
    REQUIRE(pumpUntil([&] {
        const auto &c = e.store.conversation(d1);
        return c.lastRead == incoming && c.unread == 0 && c.mentions == 0;
    }));
    CHECK(appends == 0);
}

TEST("dm zero events: same-domain confirmation, stale-positive suppression, and incoming arrival during reconcile") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef d1 = e.conv("D1");
    REQUIRE(d1 != kNoConv);
    const Ts ts200 = model::parseTs("1800000000.000200");
    const Ts ts300 = model::parseTs("1800000000.000300");
    const Ts ts400 = model::parseTs("1800000000.000400");

    model::Message m200;
    m200.ts   = ts200;
    m200.user = e.store.findUser("UMIRA");
    m200.text = "in";
    e.store.addMessage(d1, std::move(m200));
    model::Message m300;
    m300.ts   = ts300;
    m300.user = e.store.me;
    m300.text = "own";
    e.store.addMessage(d1, std::move(m300));

    // 1. Raw=2, Display=0 confirms display zero.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts200 - 1;
        c.latest   = ts300;
        c.unread = c.mentions = 1;
    });
    script("conversations.info", R"([{"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA",
        "last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"},
        "unread_count":2,"unread_count_display":0,"mention_count":0,"mention_count_display":0}}])");
    script("conversations.history", R"([{"ok":true,"has_more":false,"messages":[
        {"type":"message","ts":"1800000000.000300","user":"UME","text":"my newer reply"}]}])");
    REQUIRE(push(envelope("e-disp0",
        R"({"type":"im_marked","channel":"D1","ts":"1800000000.000200",
        "unread_count_display":0,"mention_count_display":0})")));
    REQUIRE(pumpUntil([&] { return e.store.conversation(d1).unread == 0; }));
    CHECK(e.store.conversation(d1).lastRead == ts200);

    // 2. Missing display/has_unreads in counts falls back to history; incomplete/missing unread preserves safe state.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts200 - 1;
        c.latest   = ts300;
        c.unread = c.mentions = 1;
    });
    script("client.counts", R"([{"ok":true,"channels":[],
        "ims":[{"id":"D1","last_read":"1800000000.000200","latest":"1800000000.000300",
                "dm_count":2}],"mpims":[]}])");
    script("conversations.info", R"([{"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA",
        "last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"},
        "unread_count":2}}])");
    script("conversations.history", R"([{"ok":false,"error":"missing_scope"}])");
    const int infoBefore = count("conversations.info");
    REQUIRE(push(envelope("e-nodisp",
        R"({"type":"im_marked","channel":"D1","ts":"1800000000.000200",
        "unread_count_display":0,"mention_count_display":0})")));
    REQUIRE(pumpUntil([&] { return count("conversations.info") > infoBefore; }));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).unread == 1);

    // 3. Stale positive count at same cursor 200 cannot revive cleared badge.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts200;
        c.latest   = ts300;
        c.unread = c.mentions = 0;
    });
    script("conversations.info", R"([{"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA",
        "last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"},
        "unread_count":0,"unread_count_display":0,"mention_count":0,"mention_count_display":0}}])");
    script("conversations.history", R"([{"ok":true,"has_more":false,"messages":[
        {"type":"message","ts":"1800000000.000300","user":"UME","text":"my newer reply"}]}])");
    const int infoPos = count("conversations.info");
    REQUIRE(push(envelope("e-stalepos",
        R"({"type":"im_marked","channel":"D1","ts":"1800000000.000200",
        "unread_count_display":1,"mention_count_display":1})")));
    REQUIRE(pumpUntil([&] { return count("conversations.info") > infoPos; }));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).unread == 0);

    // 4. Eligible incoming=400 during reconciliation stays unread.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts200 - 1;
        c.latest   = ts300;
        c.unread = c.mentions = 1;
    });
    script("conversations.info", R"([{"__delay":0.15,"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA",
        "last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"},
        "unread_count":0,"unread_count_display":0,"mention_count":0,"mention_count_display":0}}])");
    script("conversations.history", R"([{"ok":true,"has_more":false,"messages":[
        {"type":"message","ts":"1800000000.000300","user":"UME","text":"my newer reply"}]}])");
    const int infoArr = count("conversations.info");
    REQUIRE(push(envelope("e-arr",
        R"({"type":"im_marked","channel":"D1","ts":"1800000000.000200",
        "unread_count_display":0,"mention_count_display":0})")));
    REQUIRE(pumpUntil([&] { return count("conversations.info") > infoArr; }));
    REQUIRE(push(envelope("e-msg400",
        R"({"type":"message","channel":"D1","user":"UMIRA","text":"new arrival","ts":"1800000000.000400"})")));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(d1, ts400) != nullptr; }));
    fakeslack::pumpFor(200);
    CHECK(e.store.conversation(d1).unread >= 1);
}

TEST("dm zero events: burst coalescing, failure recovery, and 20-DM workspace concurrency cap") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef d1 = e.conv("D1");
    REQUIRE(d1 != kNoConv);
    const Ts ts200 = model::parseTs("1800000000.000200");
    const Ts ts300 = model::parseTs("1800000000.000300");

    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts200 - 1;
        c.latest   = ts300;
        c.unread = c.mentions = 1;
    });

    // 1. Burst of 20 conflicting events coalesces, survives initial failure, and converges on success.
    script("client.counts", R"([
        {"ok":false,"error":"server_error"},
        {"ok":true,"channels":[],"ims":[{"id":"D1","last_read":"1800000000.000200","latest":"1800000000.000300","has_unreads":false,"mention_count":0}],"mpims":[]}])");
    const int countsStart = count("client.counts");
    for (int i = 0; i < 20; ++i) {
        REQUIRE(push(envelope(
            "burst-" + std::to_string(i),
            R"({"type":"im_marked","channel":"D1","ts":"1800000000.000200",
            "unread_count_display":0,"mention_count_display":0})"
        )));
    }
    REQUIRE(pumpUntil([&] { return e.store.conversation(d1).unread == 0; }, 4000));
    // Coalesced: far fewer than 20 requests.
    CHECK(count("client.counts") - countsStart <= 3);

    // 2. 20 distinct DMs: workspace reconciliation stays bounded and every DM eventually gets a turn.
    std::vector<std::string> dms;
    for (int i = 0; i < 20; ++i) {
        std::string id = "D" + std::to_string(10 + i);
        dms.push_back(id);
        model::Conversation mc;
        mc.id       = id;
        mc.kind     = model::ConvKind::Dm;
        mc.member   = true;
        mc.lastRead = ts200 - 1;
        mc.latest   = ts300;
        mc.unread = mc.mentions = 1;
        e.store.addConversation(std::move(mc));
    }
    set(R"({"conversations.info":{"__delay":0.02,"ok":true,"channel":{"is_im":true,"user":"UMIRA",
        "last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"},
        "unread_count":0,"unread_count_display":0,"mention_count":0,"mention_count_display":0}}})");
    for (int i = 0; i < 20; ++i) {
        REQUIRE(push(envelope(
            "dm20-" + std::to_string(i),
            std::string(R"({"type":"im_marked","channel":")") + dms[i] +
                R"(","ts":"1800000000.000200","unread_count_display":0,"mention_count_display":0})"
        )));
    }
    // Foreground history request completes while background reconciliations remain queued.
    bool fgDone = false;
    e.be->loadHistory(e.conv("C1"), 0, [&](bool, const std::string &) { fgDone = true; });
    REQUIRE(pumpUntil([&] { return fgDone; }, 2000));
    REQUIRE(pumpUntil(
        [&] {
            for (const auto &id : dms)
                if (e.store.conversation(e.conv(id.c_str())).unread != 0)
                    return false;
            return true;
        },
        6000
    ));
    json::Document statsDoc;
    statsDoc.parse(ctl("GET", "/_ctl/request_stats").body, nullptr);
    const json::Value infoStats = statsDoc.root()["conversations.info"];
    CHECK(infoStats["peak"].integer() <= 1);
    CHECK(infoStats["active"].integer() == 0);
}

TEST("dm scope live: captured main/thread read survives info=1, reconnect, and reversed order") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef d1 = e.conv("D1");
    REQUIRE(d1 != kNoConv);

    const Ts root     = model::parseTs("1794909869.919749");
    const Ts mainRead = model::parseTs("1794909941.811299");
    const Ts reply    = model::parseTs("1794910537.557059");
    const Ts newMain  = model::parseTs("1794910538.000000");

    // Seed root + thread replies with mainRead < reply.
    model::Message mRoot;
    mRoot.ts   = root;
    mRoot.user = e.store.findUser("UMIRA");
    mRoot.text = "root message";
    e.store.addMessage(d1, std::move(mRoot));

    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = mainRead;
        c.latest   = reply;
        c.unread   = 1;
        c.mentions = 1;
    });

    uint32_t maxObservedDuringZero = 0;
    bool     trackZero             = false;
    e.store.observe(d1, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Meta && trackZero)
            maxObservedDuringZero =
                std::max(maxObservedDuringZero, e.store.conversation(d1).unread);
    });

    // Accept main read via client.counts (or history confirmation when counts unavailable).
    script("client.counts", R"([{"ok":true,"channels":[],
        "ims":[{"id":"D1","latest":"1794909869.919749","last_read":"1794909941.811299",
                "has_unreads":false,"mention_count":0}],
        "mpims":[]}])");
    script("conversations.info", R"([{"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA",
        "last_read":"1794909941.811299",
        "latest":{"ts":"1794910537.557059","thread_ts":"1794909869.919749"},
        "unread_count":1,"unread_count_display":1,"mention_count":0,"mention_count_display":0}}])");
    script("conversations.history", R"([{"ok":true,"has_more":false,"messages":[
        {"type":"message","ts":"1794910537.557059","thread_ts":"1794909869.919749","user":"UMIRA","text":"reply"},
        {"type":"message","ts":"1794909869.919749","user":"UMIRA","text":"root"}]}])");
    REQUIRE(push(envelope("main-read-marked",
        R"({"type":"im_marked","channel":"D1","ts":"1794909941.811299",
        "unread_count_display":0,"mention_count_display":0})")));
    REQUIRE(pumpUntil([&] { return e.store.conversation(d1).unread == 0; }));
    trackZero = true;

    // Reconnect sweep / conversations.info returns unread_count=1, unread_count_display=1.
    script("conversations.info", R"([{"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA",
        "last_read":"1794909941.811299",
        "latest":{"ts":"1794910537.557059","thread_ts":"1794909869.919749"},
        "unread_count":1,"unread_count_display":1,"mention_count":0,"mention_count_display":0}}])");
    const int infoCountBefore = count("conversations.info");
    e.be->readCallObservedForTest(
        "conversations.info",
        "channel=D1",
        [&](const json::Document &doc, const std::string &err, const slack::readstate::DispatchContext &ctx) {
            if (err.empty()) {
                e.be->applyReadObservationForTest(
                    d1,
                    slack::mapjson::conversationReadObservation(
                        doc.root()["channel"], slack::readstate::Source::ConversationInfo
                    ),
                    ctx
                );
            }
        }
    );
    REQUIRE(pumpUntil([&] { return count("conversations.info") >= infoCountBefore + 1; }));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(maxObservedDuringZero == 0);

    // Genuinely new top-level message arrives via RTM and stays unread despite delayed old zero.
    trackZero = false;
    REQUIRE(push(envelope(
        "new-main",
        R"({"type":"message","channel":"D1","user":"UMIRA","ts":"1794910538.000000","text":"new main"})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.conversation(d1).latest == newMain; }));
    CHECK(e.store.conversation(d1).unread == 1);

    script("conversations.history", R"([{"ok":true,"has_more":false,"messages":[
        {"type":"message","ts":"1794910538.000000","user":"UMIRA","text":"new main"}]}])");
    e.be->pollUnreadCountsForTest();
    fakeslack::pumpFor(80);
    CHECK(e.store.conversation(d1).unread == 1);
}

TEST("dm scope threads: thread replies and floors stay independent of main DM unread") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef d1 = e.conv("D1");
    REQUIRE(d1 != kNoConv);

    const Ts root     = model::parseTs("1794909869.919749");
    const Ts mainRead = model::parseTs("1794909941.811299");
    const Ts reply    = model::parseTs("1794910537.557059");

    // Seed root message and mainRead < reply.
    model::Message mRoot;
    mRoot.ts   = root;
    mRoot.user = e.store.findUser("UMIRA");
    mRoot.text = "root message";
    e.store.addMessage(d1, std::move(mRoot));
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = mainRead;
        c.latest   = root;
        c.unread   = 0;
        c.mentions = 0;
    });

    // 1. Thread read floor established via conversations.replies (root.last_read == reply).
    script("conversations.replies", R"([{"ok":true,"has_more":false,"messages":[
        {"type":"message","ts":"1794909869.919749","user":"UMIRA","text":"root message",
         "reply_count":1,"latest_reply":"1794910537.557059","last_read":"1794910537.557059"},
        {"type":"message","ts":"1794910537.557059","thread_ts":"1794909869.919749",
         "parent_user_id":"UMIRA","user":"UMIRA","text":"already read reply"}]}])");
    e.be->loadThread(d1, root, [](bool, const std::string &) {});
    REQUIRE(pumpUntil([&] { return e.be->threadReadFloor(d1, root) == reply; }));
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(e.store.conversation(d1).mentions == 0);
    CHECK(e.store.unreadThreads() == 0);

    // 2. Late RTM delivery of the already-read reply must NOT increment main DM unread or unreadThreads.
    REQUIRE(push(envelope(
        "late-read-reply",
        R"({"type":"message","channel":"D1","user":"UMIRA","ts":"1794910537.557059",
            "thread_ts":"1794909869.919749","parent_user_id":"UMIRA","text":"already read reply"})"
    )));
    fakeslack::pumpFor(80);
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(e.store.conversation(d1).mentions == 0);
    CHECK(e.store.unreadThreads() == 0);

    // 3. A reply after the accepted floor in a followed thread is independently unread in Threads,
    // without incrementing main DM unread.
    REQUIRE(push(envelope(
        "new-thread-reply",
        R"({"type":"message","channel":"D1","user":"UMIRA","ts":"1794910539.000000",
            "thread_ts":"1794909869.919749","parent_user_id":"UME","text":"new reply"})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.unreadThreads() > 0; }));
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(e.store.conversation(d1).mentions == 0);

    // 4. thread_broadcast counts once on the main DM and is not retired by threadRead.
    const Ts bcast = model::parseTs("1794910540.000000");
    REQUIRE(push(envelope(
        "bcast-reply",
        R"({"type":"message","channel":"D1","subtype":"thread_broadcast","user":"UMIRA",
            "ts":"1794910540.000000","thread_ts":"1794909869.919749","text":"broadcast"})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.conversation(d1).latest == bcast; }));
    CHECK(e.store.conversation(d1).unread == 1);
    e.be->markThreadRead(d1, root, bcast);
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).unread == 1);
    CHECK(e.store.unreadThreads() == 0);
}

#endif
