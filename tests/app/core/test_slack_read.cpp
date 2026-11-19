// SlackBackend's read side against the local fake Slack (tests/support/fake_slack.py
// via fake_slack_server.h): connect (roster, users, unread, stars, emoji,
// user groups), history pages, threads, the poll that delivers new
// messages, rate limits, auth errors, the Enterprise Grid fallback, and the
// JSON mappers. Nothing here talks to the real Slack. Delays are compressed
// (MSGA_SLACK_TEST_SPEEDUP) so the 5 s poll runs every 50 ms.
#include "app/slack/rtm_presence.h"
#include "app/slack/slack_backend.h"
#include "app/slack/slack_json.h"
#include "support/fake_slack_server.h"
#include "base/json.h"
#include "base/log.h"
#include "base/time.h"
#include "support/test.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32

namespace {

using fakeslack::app;
using fakeslack::ctl;
using fakeslack::pumpUntil;
using model::ConvRef;
using model::kNoConv;
using model::Ts;

// Standing answers ({key: answer-or-list}), see fake_slack.py /_ctl/set.
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
    // Calls of `method` whose form has field == value.
    int count(std::string_view method, std::string_view field, std::string_view value) const {
        int n = 0;
        for (json::Value r : doc.root())
            n += r["method"].str() == method && r["form"][field].str() == value;
        return n;
    }
    json::Value get(std::string_view method, int n = 0) const {
        for (json::Value r : doc.root())
            if (r["method"].str() == method && n-- == 0)
                return r;
        return {};
    }
};

// The workspace every connect test starts from: two pages of users, two of
// conversations (a channel, a muted channel, DMs incl. a Slack Connect peer
// users.list never lists, a group DM known only by its mpdm- name).
// Env refuses rtm.connect with it (kNoRtm): a polling session workspace
// unless the test asks for the live RTM link.
const char *kNoRtm     = R"({"rtm.connect": {"ok": false, "error": "not_allowed_token_type"}})";
const char *kWorkspace = R"({
 "auth.test": {"ok": true, "user_id": "UME", "team_id": "T1", "team": "Lumen",
               "url": "https://lumen.slack.com/"},
 "users.list": {"ok": true, "members": [
     {"id": "UME", "name": "me", "profile": {"real_name": "Me Myself"}},
     {"id": "UMIRA", "name": "mira", "tz_offset": 3600,
      "profile": {"real_name": "Mira Okafor", "image_72": "https://a/mira72.png",
                  "status_emoji": ":palm_tree:", "status_text": "away"}},
     {"id": "UJONAS", "name": "jonas", "profile": {"display_name": "Jonas"}}],
   "response_metadata": {"next_cursor": "p2"}},
 "users.list?cursor=p2": {"ok": true, "members": [
     {"id": "UBOT", "name": "deploybot", "is_bot": true, "profile": {}}],
   "response_metadata": {"next_cursor": ""}},
 "users.conversations": {"ok": true, "channels": [
     {"id": "C1", "name": "general", "is_channel": true, "is_member": true,
      "topic": {"value": "Company news"}, "num_members": 12},
     {"id": "D1", "is_im": true, "user": "UMIRA"}],
   "response_metadata": {"next_cursor": "c2"}},
 "users.conversations?cursor=c2": {"ok": true, "channels": [
     {"id": "D2", "is_im": true, "user": "UEXT"},
     {"id": "G1", "is_mpim": true, "name": "mpdm-me--mira--jonas-1"},
     {"id": "C2", "name": "random", "is_channel": true, "is_member": true, "is_muted": true}],
   "response_metadata": {"next_cursor": ""}},
 "users.info?user=UEXT": {"ok": true, "user": {"id": "UEXT", "name": "ext",
     "profile": {"real_name": "Ext Partner"}}},
 "client.counts": {"ok": true,
   "channels": [{"id": "C1", "latest": "1700000100.000000", "last_read": "1700000000.000000",
                 "mention_count": 1, "has_unreads": true}],
   "ims": [{"id": "D1", "latest": "1700000050.000000", "last_read": "1700000000.000000",
            "dm_count": 2, "has_unreads": true}],
   "mpims": []},
 "stars.list": {"ok": true, "items": [{"type": "channel", "channel": "C2"},
                                      {"type": "message", "channel": "C1", "message": {}}]},
 "emoji.list": {"ok": true, "emoji": {"party": "https://e/party.png", "yay": "alias:party"}},
 "usergroups.list": {"ok": true, "usergroups": [{"id": "S1", "users": ["UJONAS", "UME"]},
                                                {"id": "S2", "users": ["UMIRA"]}]},
 "users.getPresence": {"ok": true, "presence": "active", "online": true},
 "conversations.history": {"ok": true, "messages": [], "has_more": false}
})";

// One test's world: the fake reset to `table`, a Store and a backend.
struct Env {
    model::Store                         store;
    net::Client                          client{app()};
    std::unique_ptr<slack::SlackBackend> be;
    std::string                          lostAuth;

    // live: kWorkspace with rtm.connect allowed (a session workspace's RTM link).
    explicit Env(const char *table = kWorkspace, bool session = true, bool live = false) {
        base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", "100");
        ctl("POST", "/_ctl/reset");
        if (table)
            set(table);
        if (table == kWorkspace && !live)
            set(kNoRtm);
        slack::Credentials cr;
        cr.token       = session ? "xoxc-test" : "xoxp-test";
        cr.cookie      = session ? "xoxd-test" : "";
        cr.teamId      = "T1";
        be             = std::make_unique<slack::SlackBackend>(store, app(), client, cr);
        be->onAuthLost = [this](const std::string &e) { lostAuth = e; };
    }
    ~Env() {
        be.reset(); // no callback or timer of it runs afterwards
        base::test::unsetEnv("MSGA_SLACK_TEST_SPEEDUP");
    }
    bool connect(std::string *err = nullptr) {
        bool called = false, ok = false;
        be->connect([&](bool o, const std::string &e) {
            called = true;
            ok     = o;
            if (err)
                *err = e;
        });
        // done(true) comes with the conversations; these tests read the
        // users too, so wait for the whole load.
        return pumpUntil([&] { return called && !be->connecting(); }) && ok;
    }
    bool history(ConvRef c, Ts before) {
        bool called = false, ok = false;
        be->loadHistory(c, before, [&](bool o, const std::string &) {
            called = true;
            ok     = o;
        });
        return pumpUntil([&] { return called; }) && ok;
    }
    // A slower clock (MSGA_SLACK_TEST_SPEEDUP=x): for tests timing the gap
    // between requests, which 100x would squeeze under the scheduler's jitter.
    void respeed(const char *x) {
        be.reset();
        base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", x);
        slack::Credentials cr;
        cr.token  = "xoxc-test";
        cr.cookie = "xoxd-test";
        cr.teamId = "T1";
        be        = std::make_unique<slack::SlackBackend>(store, app(), client, cr);
    }
    ConvRef            conv(const char *id) const { return store.findConversation(id); }
    const model::User &user(const char *id) const { return store.user(store.findUser(id)); }
    // A conversation by hand (the history/thread tests skip connect: no polls).
    ConvRef            addChannel(const char *id) {
        model::User me;
        me.id    = "UME";
        store.me = store.addUser(me);
        model::Conversation c;
        c.id   = id;
        c.name = "general";
        return store.addConversation(std::move(c));
    }
};

bool haveServer() {
    if (!fakeslack::server().empty())
        return true;
    std::fprintf(stderr, "  skip: no fake Slack (python3 missing)\n");
    return false;
}

} // namespace

TEST("slack read: connect fills users, conversations, unread, stars, emoji, groups") {
    if (!haveServer())
        return;
    Env e;
    e.store.setCustomEmoji("gone", "https://e/gone.png"); // cached, since removed
    REQUIRE(e.connect());
    CHECK_STR(e.store.workspaceName, "Lumen");
    CHECK_STR(e.store.user(e.store.me).id, "UME");
    // Both pages of users, mapped.
    const model::User &mira = e.user("UMIRA");
    CHECK_STR(mira.displayName, "Mira Okafor");
    CHECK_STR(mira.avatar, "https://a/mira72.png");
    CHECK_STR(mira.statusEmoji, "palm_tree");
    CHECK(mira.hasTz && mira.tzOffset == 3600);
    CHECK(e.user("UBOT").bot);
    // Both pages of conversations.
    REQUIRE(e.conv("C1") != kNoConv && e.conv("C2") != kNoConv && e.conv("G1") != kNoConv);
    const model::Conversation &general = e.store.conversation(e.conv("C1"));
    CHECK_STR(general.topic, "Company news");
    CHECK(general.memberCount == 12);
    CHECK(e.store.conversation(e.conv("C2")).muted);
    const model::Conversation &dm = e.store.conversation(e.conv("D1"));
    CHECK(dm.kind == model::ConvKind::Dm && dm.dmUser == e.store.findUser("UMIRA"));
    CHECK_STR(e.store.displayName(e.conv("D1")), "Mira Okafor");
    // The group DM's members come from its mpdm- name.
    CHECK(e.store.conversation(e.conv("G1")).members.size() == 3);
    CHECK_STR(e.store.displayName(e.conv("G1")), "Mira Okafor, Jonas");
    // client.counts seeds the badges: a DM's unread are all red badges.
    REQUIRE(pumpUntil([&] { return e.store.conversation(e.conv("D1")).unread == 2; }, 3000));
    CHECK(e.store.conversation(e.conv("D1")).mentions == 2);
    CHECK(general.unread == 1 && general.mentions == 1);
    CHECK(general.lastRead == model::parseTs("1700000000.000000"));
    CHECK(general.latest == model::parseTs("1700000100.000000"));
    // The DM peer users.list omits is resolved through users.info.
    REQUIRE(pumpUntil([&] { return !e.user("UEXT").placeholder; }, 3000));
    CHECK_STR(e.store.displayName(e.conv("D2")), "Ext Partner");
    // Stars (conversation stars only), custom emoji with aliases, my groups,
    // my presence.
    REQUIRE(pumpUntil([&] { return e.store.conversation(e.conv("C2")).starred; }, 3000));
    CHECK(!e.store.conversation(e.conv("C1")).starred);
    REQUIRE(pumpUntil([&] { return !e.store.emojiFor("yay").image.empty(); }, 3000));
    CHECK_STR(e.store.emojiFor("yay").image, "https://e/party.png");
    CHECK_FALSE(e.store.emojiFor("gone").resolved()); // emoji.list is the whole set
    REQUIRE(e.store.customEmojiNames().size() == 2);
    CHECK(e.store.customEmojiNames()[0] == "party" && e.store.customEmojiNames()[1] == "yay");
    REQUIRE(e.store.customEmojiImages().size() == 1);
    CHECK(e.store.customEmojiImages()[0].name == "party");
    REQUIRE(pumpUntil([&] { return e.store.myGroups.size() == 1; }, 3000));
    CHECK_STR(e.store.myGroups[0], "S1");
    REQUIRE(pumpUntil([&] { return e.be->selfPresence().loaded; }, 3000));
    CHECK(e.be->selfPresence().active && e.be->selfPresence().online);
    CHECK(e.store.user(e.store.me).active);
    // The listing parameters sent.
    Log               l;
    const json::Value list = l.get("users.conversations");
    CHECK_STR(list["form"]["types"].str(), "public_channel,private_channel,im,mpim");
    CHECK_STR(list["form"]["limit"].str(), "1000");
    CHECK_STR(list["form"]["exclude_archived"].str(), "true");
    CHECK_STR(list["form"]["team_id"].str(), "T1");
    CHECK_STR(l.get("users.conversations", 1)["form"]["cursor"].str(), "c2");
    CHECK_STR(l.get("users.list", 1)["form"]["cursor"].str(), "p2");
    CHECK_STR(l.get("usergroups.list")["form"]["include_users"].str(), "1");
    CHECK_STR(l.get("auth.test")["auth"].str(), "Bearer xoxc-test");
    CHECK_STR(l.get("auth.test")["cookie"].str(), "d=xoxd-test");
    const slack::SlackBackend::Capabilities caps = e.be->capabilities();
    CHECK(caps.threadsView && caps.messageReminders && caps.huddles && caps.memberList);
}

TEST("slack read: a users.list snapshot merges, never drops or blanks users") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return !e.user("UEXT").placeholder; }, 3000));
    // The next snapshot lacks Jonas and the external peer, and Mira's avatar.
    set(R"({"users.list": {"ok": true, "members": [
              {"id": "UME", "name": "me", "profile": {}},
              {"id": "UMIRA", "name": "mira", "profile": {"real_name": "Mira O."}}],
            "response_metadata": {"next_cursor": ""}}})");
    // A new backend on the same Store: this run's list doesn't stand in.
    slack::Credentials cr;
    cr.token  = "xoxc-test";
    cr.cookie = "xoxd-test";
    cr.teamId = "T1";
    e.be      = std::make_unique<slack::SlackBackend>(e.store, app(), e.client, cr);
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return !e.be->connecting(); }, 3000));
    CHECK_STR(e.user("UMIRA").displayName, "Mira O.");
    CHECK_STR(e.user("UMIRA").avatar, "https://a/mira72.png"); // kept
    CHECK(!e.user("UJONAS").placeholder);
    CHECK_STR(e.user("UJONAS").displayName, "Jonas");
    CHECK_STR(e.user("UEXT").displayName, "Ext Partner");
}

TEST("slack read: history pages arrive oldest first, hasMoreBefore follows has_more") {
    if (!haveServer())
        return;
    Env           e(nullptr);
    const ConvRef c = e.addChannel("C1");
    script("conversations.history", R"([
      {"ok": true, "has_more": true, "messages": [
        {"type": "message", "ts": "1700000300.000300", "bot_id": "B1", "username": "Deploys",
         "text": "fallback", "subtype": "bot_message",
         "bot_profile": {"name": "deploy", "icons": {"image_48": "https://b/48.png"}},
         "blocks": [{"type": "header", "text": {"type": "plain_text", "text": "Deployed <3"}},
                    {"type": "section", "text": {"type": "mrkdwn", "text": "build *42*"}}]},
        {"type": "message", "ts": "1700000200.000200", "user": "UMIRA", "text": "pic",
         "edited": {"user": "UMIRA"}, "thread_ts": "1700000200.000200", "reply_count": 2,
         "latest_reply": "1700000250.000000", "reply_users": ["UJONAS"],
         "reactions": [{"name": "tada", "count": 3, "users": ["UME", "UJONAS"]}],
         "files": [{"id": "F1", "name": "cat.png", "mimetype": "image/png", "pretty_type": "PNG",
                    "url_private": "https://files/cat.png", "thumb_360": "https://files/cat360.png",
                    "thumb_720": "https://files/cat720.png", "original_w": 1600, "original_h": 900,
                    "size": 12345}],
         "attachments": [{"from_url": "https://news/x", "title": "News", "service_name": "News",
                          "color": "3FCB8E", "text": "story", "thumb_url": "https://news/t.png",
                          "thumb_width": 80, "thumb_height": 60}]}]},
      {"ok": true, "has_more": false, "messages": [
        {"type": "message", "ts": "1700000100.000100", "user": "UME", "text": "first"}]}
    ])");
    REQUIRE(e.history(c, 0));
    const model::Conversation &conv = e.store.conversation(c);
    REQUIRE(conv.messages.size() == 2);
    CHECK(conv.hasMoreBefore);
    const model::Message &pic = conv.messages[0];
    CHECK(pic.ts == model::parseTs("1700000200.000200"));
    CHECK(pic.edited && pic.replyCount == 2 && !pic.isReply());
    CHECK(pic.latestReply == model::parseTs("1700000250.000000"));
    CHECK(pic.replyUsers.size() == 1 && pic.replyUsers[0] == e.store.findUser("UJONAS"));
    REQUIRE(pic.reactions.size() == 1);
    CHECK(pic.reactions[0].count == 3 && pic.reactions[0].users.size() == 2);
    REQUIRE(pic.files().size() == 1);
    CHECK_STR(pic.files()[0].path, "https://files/cat720.png");
    CHECK(pic.files()[0].isImage() && pic.files()[0].width == 1600);
    REQUIRE(pic.attachments().size() == 1);
    CHECK(pic.attachments()[0].linkPreview);
    CHECK_STR(pic.attachments()[0].color, "#3FCB8E");
    CHECK_STR(pic.attachments()[0].link, "https://news/x");
    CHECK_STR(pic.attachments()[0].image, "https://news/t.png");
    const model::Message &bot = conv.messages[1];
    CHECK_STR(bot.extra->botName, "Deploys");
    CHECK_STR(bot.extra->botAvatar, "https://b/48.png");
    CHECK_STR(bot.subtype(), "bot_message");
    CHECK_STR(bot.text, "*Deployed &lt;3*\nbuild *42*"); // the blocks, not the fallback
    // The older page: latest = the oldest we hold, exclusive.
    REQUIRE(e.history(c, conv.messages.front().ts));
    CHECK(conv.messages.size() == 3 && !conv.hasMoreBefore);
    CHECK_STR(conv.messages.front().text, "first");
    Log l;
    CHECK_STR(l.get("conversations.history")["form"]["limit"].str(), "50");
    CHECK(!l.get("conversations.history")["form"].has("latest"));
    CHECK_STR(l.get("conversations.history", 1)["form"]["latest"].str(), "1700000200.000200");
    CHECK_STR(l.get("conversations.history", 1)["form"]["inclusive"].str(), "false");
}

TEST("slack read: a thread loads every replies page and refreshes its root") {
    if (!haveServer())
        return;
    Env            e(nullptr);
    const ConvRef  c    = e.addChannel("C1");
    const Ts       root = model::parseTs("1700000200.000200");
    model::Message m;
    m.ts       = root;
    m.threadTs = root;
    m.text     = "root";
    std::vector<model::Message> page;
    page.push_back(std::move(m));
    e.store.addPage(c, std::move(page));
    script("conversations.replies", R"([
      {"ok": true, "has_more": true, "response_metadata": {"next_cursor": "r2"}, "messages": [
        {"type": "message", "ts": "1700000200.000200", "thread_ts": "1700000200.000200",
         "user": "UMIRA", "text": "root", "reply_count": 2, "latest_reply": "1700000400.000000",
         "reply_users": ["UJONAS", "UMIRA"]},
        {"type": "message", "ts": "1700000300.000000", "thread_ts": "1700000200.000200",
         "user": "UJONAS", "text": "one"}]},
      {"ok": true, "messages": [
        {"type": "message", "ts": "1700000400.000000", "thread_ts": "1700000200.000200",
         "user": "UMIRA", "text": "two", "subtype": "thread_broadcast"}]}
    ])");
    bool called = false, ok = false;
    e.be->loadThread(c, root, [&](bool o, const std::string &) {
        called = true;
        ok     = o;
    });
    REQUIRE(pumpUntil([&] { return called; }) && ok);
    const std::vector<model::Message> *replies = e.store.replies(c, root);
    REQUIRE(replies && replies->size() == 2);
    CHECK_STR((*replies)[0].text, "one");
    CHECK_STR((*replies)[1].text, "two");
    CHECK((*replies)[1].threadTs == root);
    const model::Message *r = e.store.findMessage(c, root);
    REQUIRE(r);
    CHECK(r->replyCount == 2 && r->replyUsers.size() == 2);
    CHECK(e.store.conversation(c).messages.size() == 1); // replies stay in the thread
    Log l;
    CHECK_STR(l.get("conversations.replies", 1)["form"]["cursor"].str(), "r2");
    CHECK_STR(l.get("conversations.replies")["form"]["ts"].str(), "1700000200.000200");
}

// The thread download relies on it: one failed page fails the whole load
// (no partial thread reported as complete), and a looping cursor ends.
TEST("slack read: a failed replies page fails the thread load; a looping cursor stops") {
    if (!haveServer())
        return;
    Env           e(nullptr);
    const ConvRef c    = e.addChannel("C1");
    const Ts      root = model::parseTs("1700000200.000200");
    script("conversations.replies", R"([
      {"ok": true, "has_more": true, "response_metadata": {"next_cursor": "r2"}, "messages": [
        {"type": "message", "ts": "1700000200.000200", "thread_ts": "1700000200.000200",
         "user": "UMIRA", "text": "root", "reply_count": 3},
        {"type": "message", "ts": "1700000300.000000", "thread_ts": "1700000200.000200",
         "user": "UJONAS", "text": "one"}]},
      {"ok": false, "error": "thread_not_found"}
    ])");
    bool        called = false, ok = true;
    std::string err;
    e.be->loadThread(c, root, [&](bool o, const std::string &er) {
        called = true;
        ok     = o;
        err    = er;
    });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(!ok);
    CHECK_STR(err, "thread_not_found");
    const std::vector<model::Message> *replies = e.store.replies(c, root);
    CHECK(!replies || replies->empty()); // the first page is not kept as the thread
    CHECK(Log().count("conversations.replies") == 2);

    // A server repeating its next_cursor forever (the table's answer sticks).
    set(R"({"conversations.replies": {"ok": true, "messages": [],
              "response_metadata": {"next_cursor": "again"}}})");
    called = false;
    e.be->loadThread(c, root, [&](bool o, const std::string &er) {
        called = true;
        ok     = o;
        err    = er;
    });
    REQUIRE(pumpUntil([&] { return called; }, 30000));
    CHECK(!ok);
    CHECK_STR(err, "page_limit");
    CHECK(Log().count("conversations.replies") == 2 + 400);
}

TEST("slack read: the open chat's poll delivers new messages and deletions") {
    if (!haveServer())
        return;
    Env e;
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000100.000000", "user": "UJONAS", "text": "old"}]}})");
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(e.history(c, 0));
    REQUIRE(pumpUntil([&] { return e.store.conversation(c).unread == 1; }, 3000)); // seeded
    e.be->setActiveConversation(c, 0);
    // Someone mentions me: the poll delivers it as a live message (badge up).
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000200.000000", "user": "UMIRA", "text": "hi <@UME>"},
              {"type": "message", "ts": "1700000100.000000", "user": "UJONAS", "text": "old"}]}})");
    const Ts hi = model::parseTs("1700000200.000000");
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, hi) != nullptr; }, 3000));
    CHECK(e.store.conversation(c).unread == 2);
    CHECK(e.store.conversation(c).mentions == 2);
    CHECK(e.store.conversation(c).latest == hi);
    // Edited elsewhere: the head page merges it in place.
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000200.000000", "user": "UMIRA",
               "text": "hi <@UME>!", "edited": {"user": "UMIRA"}},
              {"type": "message", "ts": "1700000100.000000", "user": "UJONAS", "text": "old"}]}})");
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, hi)->edited; }, 3000));
    CHECK_STR(e.store.findMessage(c, hi)->text, "hi <@UME>!");
    CHECK(e.store.conversation(c).unread == 2); // an edit is not news
    // Deleted elsewhere: gone from the head page, so gone here.
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000100.000000", "user": "UJONAS", "text": "old"}]}})");
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, hi) == nullptr; }, 3000));
}

TEST("slack read: a message deleted while its chat was closed goes on the first poll") {
    if (!haveServer())
        return;
    Env e;
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000300.000000", "user": "UMIRA", "text": "two"},
              {"type": "message", "ts": "1700000200.000000", "user": "UMIRA", "text": "one"},
              {"type": "message", "ts": "1700000100.000000", "user": "UJONAS", "text": "old"}]}})");
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(e.history(c, 0));
    const Ts one = model::parseTs("1700000200.000000");
    REQUIRE(e.store.findMessage(c, one) != nullptr);
    // Deleted with no event reaching us; the chat is opened afterwards.
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000300.000000", "user": "UMIRA", "text": "two"},
              {"type": "message", "ts": "1700000100.000000", "user": "UJONAS", "text": "old"}]}})");
    e.be->setActiveConversation(c, 0);
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, one) == nullptr; }, 3000));
    CHECK(e.store.findMessage(c, model::parseTs("1700000300.000000")) != nullptr);
    CHECK(e.store.findMessage(c, model::parseTs("1700000100.000000")) != nullptr);
}

TEST("slack read: hidden, the open chat polls slowly, client.counts brings it forward") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(e.history(c, 0));
    e.be->setActiveConversation(c, 0);
    e.be->setWindowVisible(false);
    fakeslack::pumpFor(100);
    // The open chat: 60 s instead of 5 s (600 ms here), and no presence rounds.
    Log before;
    fakeslack::pumpFor(1500);
    Log l;
    CHECK(l.count("conversations.history") - before.count("conversations.history") <= 4);
    CHECK(
        l.count("users.getPresence", "user", "UMIRA") ==
        before.count("users.getPresence", "user", "UMIRA")
    );
    // Something lands there: the counts snapshot moves and the chat is polled.
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000300.000000", "user": "UMIRA", "text": "news"}]},
            "client.counts": {"ok": true,
              "channels": [{"id": "C1", "latest": "1700000300.000000",
                            "last_read": "1700000000.000000", "has_unreads": true}],
              "ims": [], "mpims": []}})");
    REQUIRE(pumpUntil(
        [&] { return e.store.findMessage(c, model::parseTs("1700000300.000000")) != nullptr; }, 3000
    ));
    // Shown again: the dots are asked for at once.
    const int presence = Log().count("users.getPresence", "user", "UMIRA");
    e.be->setWindowVisible(true);
    REQUIRE(pumpUntil(
        [&] { return Log().count("users.getPresence", "user", "UMIRA") > presence; }, 1000
    ));
}

// ── Live (a session workspace's RTM link up): polling is a safety net ──────

// The live Env's RTM link up, the open chat C1 settled into its cadence.
static bool liveOpen(Env &e) {
    if (!e.connect() || !pumpUntil([&] { return e.be->hasRealtimePush(); }, 5000))
        return false;
    const ConvRef c = e.conv("C1");
    if (!e.history(c, 0))
        return false;
    e.be->setActiveConversation(c, 0);
    fakeslack::pumpFor(200);
    return true;
}

// conversations.history calls for anything but `open`.
static int otherHistory(const Log &l, std::string_view open) {
    int n = 0;
    for (json::Value r : l.doc.root())
        n += r["method"].str() == "conversations.history" && r["form"]["channel"].str() != open;
    return n;
}

TEST("live poll: while live only the 60 s safety polls run") {
    if (!haveServer())
        return;
    Env e(kWorkspace, true, /*live=*/true);
    REQUIRE(liveOpen(e));
    const Log before;
    fakeslack::pumpFor(1800); // 3 min
    const Log l;
    CHECK(e.be->hasRealtimePush());
    // The Activity badge and Threads: once a minute (10 s / 20 s would be 18 / 9).
    const int counts = l.count("client.counts") - before.count("client.counts");
    CHECK(counts >= 2 && counts <= 4);
    const int threads = l.count("subscriptions.thread.getView") +
                        l.count("subscriptions.thread.getTimestamps") -
                        before.count("subscriptions.thread.getView") -
                        before.count("subscriptions.thread.getTimestamps");
    CHECK(threads >= 2 && threads <= 4);
    CHECK(l.count("users.conversations") == before.count("users.conversations"));
    const int c1 = l.count("conversations.history", "channel", "C1") -
                   before.count("conversations.history", "channel", "C1");
    CHECK(c1 >= 2 && c1 <= 4);
    CHECK(otherHistory(l, "C1") == otherHistory(before, "C1"));
}

TEST("live poll: drop falls back to fast polling, hello backfills once") {
    if (!haveServer())
        return;
    Env                        e(kWorkspace, true, /*live=*/true);
    slack::RtmPresence::Timing t;
    t.pingMs         = 30;
    t.reconnectMinMs = 2'000; // stays down long enough to see the fallback
    e.be->presenceLinkForTest()->setTimingForTest(t);
    REQUIRE(liveOpen(e));
    ctl("POST", "/_ctl/ws/opt", R"({"rtm_nopong": true})");
    REQUIRE(pumpUntil([&] { return !e.be->hasRealtimePush(); }, 5000));
    ctl("POST", "/_ctl/ws/opt", R"({"rtm_nopong": false})");
    // Down: the open chat at 5 s (50 ms here), client.counts back.
    const Log down;
    fakeslack::pumpFor(400);
    const Log polled;
    CHECK(
        polled.count("conversations.history", "channel", "C1") -
            down.count("conversations.history", "channel", "C1") >=
        4
    );
    CHECK(polled.count("client.counts") > down.count("client.counts"));
    CHECK_FALSE(e.be->hasRealtimePush());
    // Hidden for the rest of the gap: the open chat's poll slows to once a
    // minute, so what follows the new socket's handshake is the backfill.
    e.be->setWindowVisible(false);
    // The hello after the gap: one roster reload, one backfill of the open
    // chat's head page, then the slow cadence.
    REQUIRE(pumpUntil([&] { return e.be->hasRealtimePush(); }, 5000));
    fakeslack::pumpFor(300);
    const Log back;
    CHECK(
        back.count("users.conversations", "cursor", "") -
            polled.count("users.conversations", "cursor", "") ==
        1
    );
    int  backfills = 0;
    bool reopened  = false;
    for (json::Value r : back.doc.root()) {
        if (r["method"].str() == "ws-open") {
            reopened  = true;
            backfills = 0;
        }
        backfills += reopened && r["method"].str() == "conversations.history" &&
                     r["form"]["channel"].str() == "C1";
    }
    CHECK(backfills == 1);
    fakeslack::pumpFor(1300);
    const Log l;
    CHECK(l.count("client.counts") - back.count("client.counts") <= 3);
    CHECK(
        l.count("conversations.history", "channel", "C1") -
            back.count("conversations.history", "channel", "C1") <=
        3
    );
}

TEST("live poll: a background find does not reconnect the socket") {
    if (!haveServer())
        return;
    Env e(kWorkspace, true, /*live=*/true);
    // C1 open, D1 the background sweep's only other conversation.
    set(R"({"users.conversations": {"ok": true, "channels": [
              {"id": "C1", "name": "general", "is_channel": true, "is_member": true},
              {"id": "D1", "is_im": true, "user": "UMIRA"}],
            "response_metadata": {"next_cursor": ""}}})");
    REQUIRE(liveOpen(e));
    REQUIRE(pumpUntil([&] { return Log().count("conversations.history", "channel", "D1") > 0; }));
    const int rtm = Log().count("rtm.connect");
    set(R"({"conversations.history?channel=D1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1800000000.000100", "user": "UMIRA", "text": "psst"}]}})");
    const ConvRef d = e.conv("D1");
    // The next sweep (10 min: 6 s here) brings it.
    REQUIRE(pumpUntil(
        [&] { return e.store.findMessage(d, model::parseTs("1800000000.000100")) != nullptr; }, 9000
    ));
    fakeslack::pumpFor(200);
    CHECK(Log().count("rtm.connect") == rtm);
    CHECK(e.be->hasRealtimePush());
}

TEST("slack read: a channel read elsewhere loses its badge with the next client.counts") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(pumpUntil([&] { return e.store.conversation(c).mentions == 1; }, 3000)); // seeded
    set(R"({"client.counts": {"ok": true,
              "channels": [{"id": "C1", "latest": "1700000100.000000",
                            "last_read": "1700000100.000000", "has_unreads": false}],
              "ims": [], "mpims": []}})");
    REQUIRE(pumpUntil(
        [&] {
            const model::Conversation &x = e.store.conversation(c);
            return x.unread == 0 && x.mentions == 0;
        },
        3000
    ));
    CHECK(e.store.conversation(c).lastRead == model::parseTs("1700000100.000000"));
}

TEST("slack read: seeing a read channel's newest message clears a badge left over") {
    if (!haveServer())
        return;
    Env           e(nullptr);
    const ConvRef c = e.addChannel("C1");
    set(R"({"conversations.history": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000100.000000", "user": "UMIRA", "text": "hi"}]}})");
    REQUIRE(e.history(c, 0));
    const Ts newest = model::parseTs("1700000100.000000");
    // Read up to the newest message, yet badged: thread replies counted on
    // the channel (issue #92), or a stale cached count.
    e.store.updateConversation(c, [&](model::Conversation &x) {
        x.lastRead = newest;
        x.unread = x.mentions = 5;
    });
    e.be->markRead(c, newest - 1); // not the newest: left alone
    CHECK(e.store.conversation(c).mentions == 5);
    e.be->markRead(c, newest);
    CHECK(e.store.conversation(c).unread == 0);
    CHECK(e.store.conversation(c).mentions == 0);
    fakeslack::pumpFor(200);
    CHECK(Log().count("conversations.mark") == 0); // nothing to tell Slack
}

TEST("slack read: a failed author lookup is not re-asked on every poll") {
    if (!haveServer())
        return;
    Env e;
    // Authors users.list doesn't know: a deleted account (user_not_found), one
    // users.info answers without a user, a bot bots.info can't find, and a
    // Slack Connect peer that resolves.
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000400.000000", "user": "UGONE", "text": "a"},
              {"type": "message", "ts": "1700000300.000000", "user": "UBLANK", "text": "b"},
              {"type": "message", "ts": "1700000200.000000", "bot_id": "BGONE", "text": "c"},
              {"type": "message", "ts": "1700000100.000000", "user": "UPEER", "text": "d"}]},
            "users.info?user=UGONE": {"ok": false, "error": "user_not_found"},
            "users.info?user=UBLANK": {"ok": true},
            "users.info?user=UPEER": {"ok": true, "user": {"id": "UPEER", "name": "peer"}},
            "bots.info?bot=BGONE": {"ok": false, "error": "bot_not_found"}})");
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(e.history(c, 0));
    e.be->setActiveConversation(c, 0);
    // The open chat polls every 50 ms here (5 s × 1/100): ~30 polls.
    const int polls = Log().count("conversations.history");
    fakeslack::pumpFor(1500);
    Log l;
    CHECK(l.count("conversations.history") - polls >= 10);
    CHECK(l.count("users.info", "user", "UGONE") == 1);
    CHECK(l.count("users.info", "user", "UBLANK") == 1);
    CHECK(l.count("bots.info", "bot", "BGONE") == 1);
    CHECK(l.count("users.info", "user", "UPEER") == 1);
    CHECK_FALSE(e.user("UPEER").placeholder);
    CHECK(e.user("UGONE").placeholder);
}

TEST("slack read: a background call already queued is not queued twice") {
    if (!haveServer())
        return;
    Env e(nullptr);
    // A slow interactive call holds the paced lane.
    set(R"({"conversations.history": {"ok": true, "has_more": false, "messages": [],
                                      "__delay": 0.4}})");
    bool slow = false;
    e.be->readCallForTest(
        "conversations.history",
        "channel=C1",
        [&](const json::Document &, const std::string &) { slow = true; },
        false
    );
    int a = 0, b = 0, other = 0;
    e.be->readCallForTest(
        "users.getPresence",
        "user=UMIRA",
        [&](const json::Document &, const std::string &err) { a += err.empty(); },
        true
    );
    e.be->readCallForTest(
        "users.getPresence",
        "user=UJONAS",
        [&](const json::Document &, const std::string &err) { other += err.empty(); },
        true
    );
    e.be->readCallForTest(
        "users.getPresence",
        "user=UMIRA",
        [&](const json::Document &, const std::string &err) { b += err.empty(); },
        true
    );
    REQUIRE(pumpUntil([&] { return slow && a && b && other; }, 5000));
    fakeslack::pumpFor(100);
    Log l;
    CHECK(l.count("users.getPresence", "user", "UMIRA") == 1); // both callers answered by one
    CHECK(l.count("users.getPresence", "user", "UJONAS") == 1);
    CHECK(a == 1 && b == 1 && other == 1);
}

TEST("slack read: the channel browser's list brings the channels I'm not in, as non-members") {
    if (!haveServer())
        return;
    Env e;
    set(R"({"conversations.list": {"ok": true, "channels": [
              {"id": "C1", "name": "general", "is_channel": true, "is_member": true,
               "num_members": 99},
              {"id": "C7", "name": "watercooler", "is_channel": true, "is_member": false,
               "num_members": 40}],
            "response_metadata": {"next_cursor": ""}}})");
    REQUIRE(e.connect());
    CHECK(e.conv("C7") == kNoConv); // the roster is mine only
    CHECK(Log().count("conversations.list") == 0);
    e.be->loadChannelDirectory();
    REQUIRE(pumpUntil([&] { return e.conv("C7") != kNoConv; }, 3000));
    CHECK(!e.store.conversation(e.conv("C7")).member);
    CHECK(e.store.conversation(e.conv("C7")).memberCount == 40);
    // Mine stay the roster's copy.
    CHECK(e.store.conversation(e.conv("C1")).member);
    CHECK(e.store.conversation(e.conv("C1")).memberCount == 12);
    CHECK_STR(Log().get("conversations.list")["form"]["types"].str(), "public_channel");
    // Opened again soon after: nothing is asked.
    e.be->loadChannelDirectory();
    fakeslack::pumpFor(100);
    CHECK(Log().count("conversations.list") == 1);
}

TEST("slack read: a roster reload that changed nothing emits no Meta") {
    if (!haveServer())
        return;
    // App keys: no client.counts, so the roster reloads every minute (it is
    // the activity source there).
    Env e(kWorkspace, /*session=*/false);
    REQUIRE(e.connect());
    fakeslack::pumpFor(300); // the connect's own follow-ups settle
    int        meta = 0, roster = 0;
    const auto id    = e.store.observe(model::Store::kAnyConv, [&](const model::Change &ch) {
        meta += ch.kind == model::ChangeKind::Meta;
        roster += ch.kind == model::ChangeKind::Roster;
    });
    // Without push the roster reloads every 60 s (0.6 s here): wait for two.
    const int  lists = Log().count("users.conversations");
    REQUIRE(pumpUntil([&] { return Log().count("users.conversations") >= lists + 4; }, 5000));
    fakeslack::pumpFor(100);
    e.store.unobserve(id);
    CHECK(meta == 0);
    CHECK(roster == 0);
}

TEST("slack read: with client.counts the roster reloads when counts names a new id") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    fakeslack::pumpFor(300);
    // counts every 10 s (0.1 s here): nothing new, so no reload in ~15
    // snapshots (the fallback is 15 min, 9 s here).
    const int lists = Log().count("users.conversations");
    fakeslack::pumpFor(1500);
    CHECK(Log().count("client.counts") >= 5);
    CHECK(Log().count("users.conversations") == lists);
    // A new DM shows up in counts first: one reload brings it.
    set(R"({"client.counts": {"ok": true,
              "channels": [{"id": "C1", "latest": "1700000100.000000",
                            "last_read": "1700000000.000000", "mention_count": 1}],
              "ims": [{"id": "D1", "latest": "1700000050.000000", "last_read": "1700000000.000000"},
                      {"id": "D9", "latest": "1700000900.000000", "last_read": "0"}],
              "mpims": []},
            "users.conversations?cursor=c2": {"ok": true, "channels": [
              {"id": "D2", "is_im": true, "user": "UEXT"},
              {"id": "G1", "is_mpim": true, "name": "mpdm-me--mira--jonas-1"},
              {"id": "C2", "name": "random", "is_channel": true, "is_member": true},
              {"id": "D9", "is_im": true, "user": "UJONAS"}],
              "response_metadata": {"next_cursor": ""}}})");
    REQUIRE(pumpUntil([&] { return e.conv("D9") != kNoConv; }, 3000));
    fakeslack::pumpFor(300);
    const int after = Log().count("users.conversations", "cursor", "c2");
    CHECK(after >= 1);
    // An id the reload doesn't bring (not ours to list) asks only once.
    set(R"({"client.counts": {"ok": true, "channels": [{"id": "C1"}, {"id": "CZ"}],
              "ims": [{"id": "D1"}, {"id": "D9"}], "mpims": []}})");
    fakeslack::pumpFor(1500);
    const int lists2 = Log().count("users.conversations", "cursor", "c2");
    CHECK(lists2 <= after + 1);
    fakeslack::pumpFor(1000);
    CHECK(Log().count("users.conversations", "cursor", "c2") == lists2);
}

TEST("slack read: an open thread whose root left the head page asks for its root alone") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    const ConvRef c    = e.conv("C1");
    const Ts      root = model::parseTs("1700000200.000200");
    // A long thread, its root long gone from the channel's head page.
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": true, "messages": [
              {"type": "message", "ts": "1700009000.000000", "user": "UJONAS", "text": "later"}]},
            "conversations.replies?limit=1": {"ok": true, "has_more": true, "messages": [
              {"type": "message", "ts": "1700000200.000200", "thread_ts": "1700000200.000200",
               "user": "UMIRA", "text": "root", "reply_count": 1,
               "latest_reply": "1700000300.000000"}]},
            "conversations.replies": {"ok": true, "messages": [
              {"type": "message", "ts": "1700000200.000200", "thread_ts": "1700000200.000200",
               "user": "UMIRA", "text": "root", "reply_count": 1,
               "latest_reply": "1700000300.000000"},
              {"type": "message", "ts": "1700000300.000000", "thread_ts": "1700000200.000200",
               "user": "UJONAS", "text": "one"}]}})");
    REQUIRE(e.history(c, 0));
    const Ts newer = model::parseTs("1700009000.000000");
    const auto unread = e.store.conversation(c).unread;
    REQUIRE(e.store.conversation(c).messages.size() == 1);
    REQUIRE(e.store.conversation(c).messages.front().ts == newer);
    REQUIRE(e.store.conversation(c).hasMoreBefore);
    bool loaded = false, ok = false;
    e.be->loadThread(c, root, [&](bool success, const std::string &) {
        loaded = true;
        ok = success;
    });
    REQUIRE(pumpUntil([&] { return loaded; }) && ok);
    REQUIRE(e.store.replies(c, root) && e.store.replies(c, root)->size() == 1);
    REQUIRE(e.store.threadRoot(c, root));
    CHECK_STR(e.store.threadRoot(c, root)->text, "root");
    CHECK(e.store.findMessage(c, root) == nullptr);
    const auto &conv = e.store.conversation(c);
    REQUIRE(conv.messages.size() == 1);
    CHECK(conv.messages.front().ts == newer);
    CHECK(conv.hasMoreBefore);
    CHECK(conv.latest == newer);
    CHECK(conv.unread == unread);
    e.be->setActiveConversation(c, root);
    // ~20 polls of the open chat: each asks for the root alone, none
    // re-reads the thread while latest_reply stays where we are.
    const int full0 = Log().count("conversations.replies", "limit", "50");
    fakeslack::pumpFor(1000);
    Log l;
    CHECK(l.count("conversations.replies", "limit", "1") >= 5);
    CHECK(l.count("conversations.replies", "limit", "50") == full0);
    // A reply lands: latest_reply moves, one full read delivers it.
    set(R"({"conversations.replies?limit=1": {"ok": true, "has_more": true, "messages": [
              {"type": "message", "ts": "1700000200.000200", "thread_ts": "1700000200.000200",
               "user": "UMIRA", "text": "root", "reply_count": 2,
               "latest_reply": "1700000400.000000"}]},
            "conversations.replies": {"ok": true, "messages": [
              {"type": "message", "ts": "1700000200.000200", "thread_ts": "1700000200.000200",
               "user": "UMIRA", "text": "root", "reply_count": 2,
               "latest_reply": "1700000400.000000"},
              {"type": "message", "ts": "1700000300.000000", "thread_ts": "1700000200.000200",
               "user": "UJONAS", "text": "one"},
              {"type": "message", "ts": "1700000400.000000", "thread_ts": "1700000200.000200",
               "user": "UMIRA", "text": "two"}]}})");
    REQUIRE(pumpUntil([&] { return e.store.replies(c, root)->size() == 2; }, 3000));
    CHECK_STR(e.store.replies(c, root)->back().text, "two");
    fakeslack::pumpFor(500);
    CHECK(Log().count("conversations.replies", "limit", "50") <= full0 + 2);
}

TEST("slack read: opening a chat fetches its head page once") {
    if (!haveServer())
        return;
    Env e;
    // Delays compressed 10× only: the open chat's poll every 500 ms, so a
    // loaded machine can't make the next regular poll look like a second
    // fetch.
    base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", "10");
    slack::Credentials cr;
    cr.token  = "xoxc-test";
    cr.cookie = "xoxd-test";
    cr.teamId = "T1";
    e.be.reset();
    e.be = std::make_unique<slack::SlackBackend>(e.store, app(), e.client, cr);
    REQUIRE(e.connect());
    const ConvRef c  = e.conv("C1");
    const int     h0 = Log().count("conversations.history", "channel", "C1");
    // What the shell does: the backend learns the chat is open, then the
    // list loads its head page; that load is the open's fetch, and the poll
    // waits its full gap after it.
    e.be->setActiveConversation(c, 0);
    REQUIRE(e.history(c, 0));
    fakeslack::pumpFor(150);
    CHECK(Log().count("conversations.history", "channel", "C1") == h0 + 1);
    // Then it polls as before.
    REQUIRE(pumpUntil(
        [&] { return Log().count("conversations.history", "channel", "C1") >= h0 + 2; }, 3000
    ));
}

TEST("slack read: a rate-limited call waits out Retry-After and retries") {
    if (!haveServer())
        return;
    Env           e(nullptr);
    const ConvRef c = e.addChannel("C1");
    script("conversations.history", R"([
      {"__status": 429, "__headers": {"Retry-After": "2"}, "ok": false, "error": "ratelimited"},
      {"ok": true, "has_more": false, "messages": [
        {"type": "message", "ts": "1700000100.000000", "user": "UME", "text": "made it"}]}
    ])");
    REQUIRE(e.history(c, 0));
    CHECK(e.store.conversation(c).messages.size() == 1);
    CHECK(Log().count("conversations.history") == 2);
}

TEST("slack read: identical throttled reads merge") {
    if (!haveServer())
        return;
    Env e(nullptr);
    e.respeed("10"); // the 1 s cooldown is 100 ms: room to ask again inside it
    const ConvRef c = e.addChannel("C1");
    script("conversations.history", R"([
      {"__status": 429, "__headers": {"Retry-After": "1"}, "ok": false, "error": "ratelimited"},
      {"ok": true, "has_more": false, "messages": [
        {"type": "message", "ts": "1700000100.000000", "user": "UME", "text": "made it"}]}
    ])");
    int  calls = 0, oks = 0;
    auto done = [&](bool ok, const std::string &) {
        ++calls;
        oks += ok;
    };
    e.be->loadHistory(c, 0, done);
    REQUIRE(pumpUntil([&] { return Log().count("conversations.history") == 1; }));
    fakeslack::pumpFor(20); // the 429 lands: C1's history cools down
    for (int i = 0; i < 3; ++i)
        e.be->loadHistory(c, 0, done);
    REQUIRE(pumpUntil([&] { return calls == 4; }));
    fakeslack::pumpFor(300);
    CHECK(calls == 4); // each caller heard once
    CHECK(oks == 4);   // the one answer
    CHECK(Log().count("conversations.history", "channel", "C1") == 2);
    CHECK(e.store.conversation(c).messages.size() == 1);
}

TEST("slack read: a cooldown releases queued reads one at a time") {
    if (!haveServer())
        return;
    Env e(nullptr);
    e.respeed("10"); // 1 s apart is 100 ms here
    const ConvRef c1 = e.addChannel("C1");
    const ConvRef c2 = e.addChannel("C2");
    const ConvRef c3 = e.addChannel("C3");
    script("conversations.history", R"([
      {"__status": 429, "__headers": {"Retry-After": "1"}, "ok": false, "error": "ratelimited"}
    ])");
    std::vector<std::pair<ConvRef, int64_t>> answered; // conv, when its answer came
    auto                                     load = [&](ConvRef c) {
        e.be->loadHistory(c, 0, [&, c](bool, const std::string &) {
            answered.emplace_back(c, base::monotonicMs());
        });
    };
    load(c1);
    REQUIRE(pumpUntil([&] { return Log().count("conversations.history") == 1; }));
    fakeslack::pumpFor(20);
    load(c2);
    load(c3);
    REQUIRE(pumpUntil([&] { return answered.size() == 3; }));
    Log log;
    REQUIRE(log.count("conversations.history") == 4);
    CHECK_STR(log.get("conversations.history", 1)["form"]["channel"].str(), "C1");
    CHECK_STR(log.get("conversations.history", 2)["form"]["channel"].str(), "C2");
    CHECK_STR(log.get("conversations.history", 3)["form"]["channel"].str(), "C3");
    CHECK(answered[0].first == c1 && answered[1].first == c2 && answered[2].first == c3);
    // Not a burst: ~100 ms between them (some slack for the answers' own latency).
    CHECK(answered[1].second - answered[0].second >= 70);
    CHECK(answered[2].second - answered[1].second >= 70);
}

TEST("slack read: a user's read goes out first after a throttled retry") {
    if (!haveServer())
        return;
    Env e(nullptr);
    e.respeed("10");
    const ConvRef c5 = e.addChannel("C5");
    script("conversations.history", R"([
      {"__status": 429, "__headers": {"Retry-After": "1"}, "ok": false, "error": "ratelimited"}
    ])");
    int  answered = 0;
    auto poll     = [&](const char *id) { // a poll's read: Normal lane, not urgent
        e.be->readCallForTest(
            "conversations.history",
            std::string("channel=") + id,
            [&](const json::Document &, const std::string &) { ++answered; },
            false
        );
    };
    poll("C1");
    REQUIRE(pumpUntil([&] { return Log().count("conversations.history") == 1; }));
    fakeslack::pumpFor(20); // C1's 429 lands: the method cools down
    poll("C2");
    poll("C3");
    poll("C4");
    e.be->loadHistory(c5, 0, [&](bool, const std::string &) { ++answered; });
    REQUIRE(pumpUntil([&] { return answered == 5; }));
    Log log;
    REQUIRE(log.count("conversations.history") == 6);
    // The 429, then: the throttled retry, the user's load, the polls in turn.
    const char *order[] = {"C1", "C1", "C5", "C2", "C3", "C4"};
    for (int i = 0; i < 6; ++i)
        CHECK_STR(log.get("conversations.history", i)["form"]["channel"].str(), order[i]);
}

TEST("slack read: identical writes riding the read path are never merged") {
    if (!haveServer())
        return;
    Env e(nullptr);
    e.respeed("10");
    const ConvRef c  = e.addChannel("C1");
    const Ts      ts = model::parseTs("1700000100.000000");
    script("chat.delete", R"([
      {"__status": 429, "__headers": {"Retry-After": "1"}, "ok": false, "error": "ratelimited"}
    ])");
    // chat.delete rides the read path (retried like a read); two more of
    // the same delete during its cooldown are two more requests, not one.
    int  answered = 0;
    auto del      = [&] {
        e.be->deleteAgentReply(c, ts, [&](bool, const std::string &) { ++answered; });
    };
    del();
    REQUIRE(pumpUntil([&] { return Log().count("chat.delete") == 1; }));
    fakeslack::pumpFor(20);
    del();
    del();
    REQUIRE(pumpUntil([&] { return answered == 3; }));
    fakeslack::pumpFor(300);
    CHECK(Log().count("chat.delete") == 4);
}

TEST("slack read: writes rate-limited together retry in their original order") {
    if (!haveServer())
        return;
    // Both in flight before either 429 lands (__delay); both answer orders:
    // W1's first, then W2's first. The cooldown (300 ms) outlasts both.
    for (const bool w2First : {false, true}) {
        Env e(nullptr);
        e.respeed("10");
        const ConvRef c = e.addChannel("C1");
        script(
            "chat.delete",
            std::string(R"([{"__status": 429, "__headers": {"Retry-After": "3"}, "ok": false,
                "error": "ratelimited", "__delay": )") +
                (w2First ? "0.2" : "0.1") +
                R"(}, {"__status": 429, "__headers": {"Retry-After": "3"}, "ok": false,
                "error": "ratelimited", "__delay": )" +
                (w2First ? "0" : "0.2") + "}]"
        );
        int  answered = 0;
        auto del      = [&](const char *ts) {
            e.be->deleteAgentReply(c, model::parseTs(ts), [&](bool, const std::string &) {
                ++answered;
            });
        };
        del("1700000100.000000"); // W1
        REQUIRE(pumpUntil([&] { return Log().count("chat.delete") == 1; }));
        del("1700000200.000000"); // W2, in flight together
        REQUIRE(pumpUntil([&] { return answered == 2; }, 3000));
        Log log;
        REQUIRE(log.count("chat.delete") == 4);
        CHECK_STR(log.get("chat.delete", 2)["form"]["ts"].str(), "1700000100.000000");
        CHECK_STR(log.get("chat.delete", 3)["form"]["ts"].str(), "1700000200.000000");
    }
}

TEST("slack read: writes are never merged") {
    if (!haveServer())
        return;
    Env           e(nullptr);
    const ConvRef c = e.addChannel("C1");
    script("conversations.history", R"([
      {"__status": 429, "__headers": {"Retry-After": "30"}, "ok": false, "error": "ratelimited"}
    ])");
    e.be->loadHistory(c, 0, nullptr);
    REQUIRE(pumpUntil([&] { return Log().count("conversations.history") == 1; }));
    fakeslack::pumpFor(20); // history cools down for 300 ms
    int sent = 0;
    for (int i = 0; i < 2; ++i)
        e.be->send(c, "same words", 0, [&](bool ok, const std::string &) { sent += ok; });
    REQUIRE(pumpUntil([&] { return sent == 2; }, 250)); // inside the cooldown
    CHECK(Log().count("chat.postMessage") == 2);
    CHECK(Log().count("conversations.history") == 1);
}

TEST("slack read: a gateway page or a lost connection is retried, not the end") {
    if (!haveServer())
        return;
    Env           e(nullptr);
    const ConvRef c = e.addChannel("C1");
    script("conversations.history", R"([
      {"__status": 502, "__raw": "<html><body>502 Bad Gateway</body></html>"},
      {"__partial": true},
      {"__status": 503, "__raw": "upstream connect error"},
      {"__partial": true},
      {"ok": true, "has_more": false, "messages": [
        {"type": "message", "ts": "1700000100.000000", "user": "UME", "text": "made it"}]}
    ])");
    REQUIRE(e.history(c, 0));
    CHECK(e.store.conversation(c).messages.size() == 1);
    CHECK(Log().count("conversations.history") == 5);
}

TEST("slack read: dead credentials fail connect and report the loss") {
    if (!haveServer())
        return;
    Env e;
    script("auth.test", R"([{"ok": false, "error": "invalid_auth"}])");
    std::string err;
    CHECK(!e.connect(&err));
    CHECK_STR(err, "invalid_auth");
    CHECK(slack::SlackBackend::authError(err));
    REQUIRE(pumpUntil([&] { return !e.lostAuth.empty(); }, 2000));
    CHECK_STR(e.lostAuth, "invalid_auth");
    CHECK(Log().count("users.conversations") == 0);
}

TEST("slack read: Enterprise Grid lists through client.userBoot + im.list") {
    if (!haveServer())
        return;
    Env e;
    set(R"({"users.conversations": {"ok": false, "error": "enterprise_is_restricted"},
            "client.userBoot": {"ok": true, "channels": [
              {"id": "C1", "name": "general", "is_channel": true, "is_member": true},
              {"id": "C9", "name": "old", "is_channel": true, "is_archived": true}]},
            "im.list": {"ok": true, "ims": [{"id": "D1", "is_im": true, "user": "UMIRA"}],
                        "response_metadata": {"next_cursor": ""}}})");
    REQUIRE(e.connect());
    CHECK(e.conv("C1") != kNoConv && e.conv("D1") != kNoConv);
    CHECK(e.conv("C9") == kNoConv); // archived: left out, as exclude_archived did
    CHECK_STR(Log().get("im.list")["form"]["get_read_state"].str(), "true");
}

TEST("slack read: the Names preference — override × workspace default, as Slack decides") {
    using namespace slack::mapjson;
    const auto prefs = [](const char *text) {
        auto d = std::make_shared<json::Document>();
        CHECK(d->parse(std::string(text), nullptr));
        return d;
    };
    CHECK(namesOverride(prefs(R"({"display_real_names_override": 1})")->root()) == 1);
    CHECK(namesOverride(prefs(R"({"display_real_names_override": -1})")->root()) == -1);
    CHECK(namesOverride(prefs(R"({"display_real_names_override": 0})")->root()) == 0);
    CHECK(namesOverride(prefs(R"({"display_real_names_override": "-1"})")->root()) == -1);
    CHECK(namesOverride(prefs(R"({"display_real_names_override": 7})")->root()) == kNoPref);
    CHECK(namesOverride(prefs(R"({"display_display_names": true})")->root()) == kNoPref);
    CHECK(namesDefault(prefs(R"({"display_real_names": true})")->root()) == 1);
    CHECK(namesDefault(prefs(R"({"display_real_names": false})")->root()) == 0);
    CHECK(namesDefault(prefs(R"({})")->root()) == kNoPref);
    // Full names: override 1, or 0 with a full-names workspace; display
    // names: override -1, or 0 with a display-names one; else undecided.
    for (int d : {0, 1, kNoPref}) {
        CHECK(realNamesFrom(1, d) == 1);
        CHECK(realNamesFrom(-1, d) == 0);
        CHECK(realNamesFrom(kNoPref, d) == kNoPref);
    }
    CHECK(realNamesFrom(0, 1) == 1);
    CHECK(realNamesFrom(0, 0) == 0);
    CHECK(realNamesFrom(0, kNoPref) == kNoPref);
    // toUser keeps both names ("" is no name); displayName starts full-first.
    const auto u = toUser(prefs(R"({"id": "UMEG", "name": "meg",
                  "profile": {"real_name": " Meg Ryan ", "display_name": ""}})")
                              ->root());
    CHECK_STR(u.realName, "Meg Ryan");
    CHECK(u.profileName.empty());
    CHECK_STR(u.displayName, "Meg Ryan");
}

namespace {
// kWorkspace plus Meg, who has both names (Mira has a full name only,
// Jonas a display name only).
const char *kMeg = R"({"users.list": {"ok": true, "members": [
     {"id": "UME", "name": "me", "profile": {"real_name": "Me Myself"}},
     {"id": "UMIRA", "name": "mira", "profile": {"real_name": "Mira Okafor"}},
     {"id": "UJONAS", "name": "jonas", "profile": {"display_name": "Jonas"}},
     {"id": "UMEG", "name": "meg", "profile": {"real_name": "Meg Ryan", "display_name": "Meg"}}],
   "response_metadata": {"next_cursor": ""}}})";
} // namespace

TEST("slack read: names follow the user's Slack override, the local choice wins") {
    if (!haveServer())
        return;
    Env e;
    set(kMeg);
    set(R"({"users.prefs.get": {"ok": true, "prefs": {"display_real_names_override": -1}},
            "team.prefs.get": {"ok": true, "prefs": {"display_real_names": true}}})");
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return !e.store.realNames(); }, 3000));
    CHECK_STR(e.user("UMEG").displayName, "Meg");
    CHECK_STR(e.user("UMIRA").displayName, "Mira Okafor"); // no display name
    CHECK_STR(e.user("UJONAS").displayName, "Jonas");
    CHECK_STR(e.store.displayName(e.conv("D1")), "Mira Okafor");
    CHECK(Log().count("users.prefs.get") == 1);
    CHECK(Log().count("team.prefs.get") == 0); // the override decided
    // Settings → Names over Slack's, both ways, at once.
    e.be->setNamesMode(model::Backend::NamesMode::Full);
    CHECK(e.store.realNames());
    CHECK_STR(e.user("UMEG").displayName, "Meg Ryan");
    CHECK_STR(e.user("UJONAS").displayName, "Jonas"); // no full name
    e.be->setNamesMode(model::Backend::NamesMode::Display);
    CHECK_STR(e.user("UMEG").displayName, "Meg");
    e.be->setNamesMode(model::Backend::NamesMode::Service);
    CHECK(!e.store.realNames());
    CHECK(Log().count("users.prefs.get") == 1); // known already: not asked again
}

TEST("slack read: an override of 0 follows the workspace default (team.prefs.get)") {
    if (!haveServer())
        return;
    for (bool teamReal : {false, true}) {
        Env e;
        set(kMeg);
        set(std::string(
                R"({"users.prefs.get": {"ok": true, "prefs": {"display_real_names_override": 0}},
              "team.prefs.get": {"ok": true, "prefs": {"display_real_names": )"
            ) +
            (teamReal ? "true" : "false") + "}}}");
        REQUIRE(e.connect());
        REQUIRE(pumpUntil([&] { return Log().count("team.prefs.get") == 1; }, 3000));
        REQUIRE(pumpUntil([&] { return e.store.realNames() == teamReal; }, 3000));
        CHECK_STR(e.user("UMEG").displayName, teamReal ? "Meg Ryan" : "Meg");
    }
    // No pref in the answer (an old server): full names, nothing more asked.
    Env e;
    set(kMeg);
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return Log().count("users.prefs.get") == 1; }, 3000));
    pumpUntil([] { return false; }, 200);
    CHECK(Log().count("team.prefs.get") == 0);
    CHECK(e.store.realNames());
    CHECK_STR(e.user("UMEG").displayName, "Meg Ryan");
}

TEST("slack read: Grid takes the workspace default from client.userBoot") {
    if (!haveServer())
        return;
    Env e;
    set(kMeg);
    set(R"({"users.conversations": {"ok": false, "error": "enterprise_is_restricted"},
            "client.userBoot": {"ok": true, "channels": [],
              "prefs": {"display_real_names_override": 0},
              "team": {"id": "T1", "prefs": {"display_real_names": false}}},
            "im.list": {"ok": true, "ims": [{"id": "D1", "is_im": true, "user": "UMIRA"}],
                        "response_metadata": {"next_cursor": ""}},
            "users.prefs.get": {"ok": true, "prefs": {"display_real_names_override": 0}},
            "team.prefs.get": {"ok": true, "prefs": {"display_real_names": true}}})");
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return Log().count("users.prefs.get") == 1; }, 3000));
    pumpUntil([] { return false; }, 300);
    CHECK(!e.store.realNames());
    CHECK_STR(e.user("UMEG").displayName, "Meg");
    CHECK(Log().count("team.prefs.get") == 0); // userBoot had it
}

TEST("slack read: app keys never ask for the Names prefs; a local choice still applies") {
    if (!haveServer())
        return;
    Env e(kWorkspace, /*session=*/false);
    set(kMeg);
    REQUIRE(e.connect());
    pumpUntil([] { return false; }, 200);
    CHECK(Log().count("users.prefs.get") == 0 && Log().count("team.prefs.get") == 0);
    CHECK(e.store.realNames());
    CHECK_STR(e.user("UMEG").displayName, "Meg Ryan");
    e.be->setNamesMode(model::Backend::NamesMode::Display);
    CHECK_STR(e.user("UMEG").displayName, "Meg");
    e.be->setNamesMode(model::Backend::NamesMode::Service);
    CHECK_STR(e.user("UMEG").displayName, "Meg Ryan");
    CHECK(Log().count("users.prefs.get") == 0);
}

TEST("slack read: a local Names choice asks Slack nothing until switched back") {
    if (!haveServer())
        return;
    Env e;
    set(kMeg);
    set(R"({"users.prefs.get": {"ok": true, "prefs": {"display_real_names_override": -1}}})");
    e.be->setNamesMode(model::Backend::NamesMode::Full);
    REQUIRE(e.connect());
    pumpUntil([] { return false; }, 200);
    CHECK(Log().count("users.prefs.get") == 0);
    CHECK_STR(e.user("UMEG").displayName, "Meg Ryan");
    e.be->setNamesMode(model::Backend::NamesMode::Service);
    REQUIRE(pumpUntil([&] { return !e.store.realNames(); }, 3000));
    CHECK_STR(e.user("UMEG").displayName, "Meg");
    CHECK(Log().count("users.prefs.get") == 1);
}

TEST("slack read: app-key workspaces sweep DM activity instead of client.counts") {
    if (!haveServer())
        return;
    Env e(kWorkspace, /*session=*/false);
    set(
        R"({"conversations.info?channel=D1": {"ok": true, "channel": {"id": "D1", "is_im": true,
              "user": "UMIRA", "last_read": "1700000000.000000", "latest": "1700000500.000000"}}})"
    );
    REQUIRE(e.connect());
    REQUIRE(pumpUntil(
        [&] {
            return e.store.conversation(e.conv("D1")).latest == model::parseTs("1700000500.000000");
        },
        3000
    ));
    CHECK(Log().count("client.counts") == 0);
    CHECK(!e.be->capabilities().threadsView && !e.be->capabilities().messageReminders);
}

TEST("slack read: mappers — blocks, rich text, message_changed, conversations") {
    model::Store   store;
    json::Document d;
    REQUIRE(d.parse(
        std::string_view(
            R"({"subtype": "message_changed", "message": {
        "ts": "1.000001", "user": "U1", "text": "", "blocks": [{"type": "rich_text", "elements": [
          {"type": "rich_text_section", "elements": [
            {"type": "text", "text": "hey "}, {"type": "user", "user_id": "U2"},
            {"type": "text", "text": " bold ", "style": {"bold": true}},
            {"type": "link", "url": "https://x.io", "text": "x"},
            {"type": "emoji", "name": "wave", "skin_tone": 3}]},
          {"type": "rich_text_list", "style": "ordered", "elements": [
            {"type": "rich_text_section", "elements": [{"type": "text", "text": "a"}]},
            {"type": "rich_text_section", "elements": [{"type": "text", "text": "b"}]}]},
          {"type": "rich_text_preformatted", "elements": [{"type": "text", "text": "code"}]}]}]}})"
        ),
        nullptr
    ));
    const model::Message m = slack::mapjson::toMessage(d.root(), store);
    CHECK(m.ts == model::parseTs("1.000001"));
    CHECK_STR(
        m.text, "hey <@U2> *bold* <https://x.io|x>:wave::skin-tone-3:\n1. a\n2. b\n```code```"
    );
    // rich_text alongside a text keeps the text (the blocks mirror it).
    REQUIRE(d.parse(
        std::string_view(R"({"ts": "2.0", "text": "*raw*", "blocks": [
        {"type": "rich_text", "elements": []}]})"),
        nullptr
    ));
    CHECK_STR(slack::mapjson::toMessage(d.root(), store).text, "*raw*");
    // A flattened text (line breaks gone) gives way to the blocks' lines,
    // their raw & < > escaped like text.
    REQUIRE(d.parse(
        std::string_view(
            R"({"ts": "3.0", "text": "a &amp; b.  did:  • x -&gt; y • z  end", "blocks": [
        {"type": "rich_text", "elements": [
          {"type": "rich_text_section", "elements": [{"type": "text", "text": "a & b.\n\ndid:"}]},
          {"type": "rich_text_list", "style": "bullet", "indent": 0, "elements": [
            {"type": "rich_text_section", "elements": [{"type": "text", "text": "x -> y"}]},
            {"type": "rich_text_section", "elements": [{"type": "text", "text": "z"}]}]},
          {"type": "rich_text_section", "elements": [{"type": "text", "text": "end"}]}]}]})"
        ),
        nullptr
    ));
    CHECK_STR(
        slack::mapjson::toMessage(d.root(), store).text, "a &amp; b.\n\ndid:\n• x -&gt; y\n• z\nend"
    );
    REQUIRE(d.parse(
        std::string_view(R"({"id": "C5", "name": "dev", "is_private": true,
        "last_read": "10.0", "latest": {"ts": "20.0"}, "unread_count": 1,
        "notification_preference": "mentions",
        "properties": {"canvas": {"file_id": "F9"}}})"),
        nullptr
    ));
    const model::Conversation c = slack::mapjson::toConversation(d.root(), store);
    CHECK(c.kind == model::ConvKind::Private);
    CHECK(c.unread == 1);
    CHECK(c.notify == model::NotifyLevel::Mentions);
    CHECK(!c.canvasTitle.empty());
    CHECK_STR(c.canvasId, "F9");
    // A DM closed in Slack (im.list lists every one) is not open here either.
    for (const char *open : {"false", "true"}) {
        REQUIRE(d.parse(
            str::concat({R"({"id": "D7", "is_im": true, "user": "U7", "is_open": )", open, "}"}),
            nullptr
        ));
        CHECK(slack::mapjson::toConversation(d.root(), store).member == (open[0] == 't'));
    }
}

TEST("slack read: stale snapshots do not restore unread over newer read or erase newer messages") {
    if (!haveServer())
        return;
    Env e(kWorkspace, /*session=*/true);
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 1; }));
    const ConvRef d1 = e.conv("D1");
    REQUIRE(d1 != kNoConv);
    const Ts ts100 = model::parseTs("1800000000.000100");
    const Ts ts300 = model::parseTs("1800000000.000300");

    // 1. In-flight client.counts reports lastRead=100, latest=300, unread=2 while
    // local markRead(300) occurs before its response arrives: final cursor 300,
    // unread/mentions zero.
    script("client.counts", R"([{"__delay":0.15,"ok":true,"channels":[],
        "ims":[{"id":"D1","latest":"1800000000.000300","last_read":"1800000000.000100",
                "unread_count":2,"mention_count":2}],"mpims":[]}])");
    e.respeed("1");
    e.be->pollUnreadCountsForTest();
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 1; }));
    e.be->markRead(d1, ts300);
    fakeslack::pumpFor(200);
    CHECK(e.store.conversation(d1).lastRead == ts300);
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(e.store.conversation(d1).mentions == 0);

    // 2. Genuine unread backlog outside loaded history retains its count.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.latest   = model::parseTs("1800000000.000500");
        c.lastRead = ts300;
        c.unread   = 5;
        c.mentions = 5;
    });
    model::Message one;
    one.ts   = model::parseTs("1800000000.000500");
    one.user = e.store.findUser("UMIRA");
    one.text = "only newest loaded";
    std::vector<model::Message> page;
    page.push_back(std::move(one));
    e.store.addPage(d1, std::move(page));
    CHECK(e.store.conversation(d1).unread == 5);
    CHECK(e.store.conversation(d1).mentions == 5);

    // 3. Stale snapshot cannot undo local Mark unread.
    script("client.counts", R"([{"__delay":0.15,"ok":true,"channels":[],
        "ims":[{"id":"D1","latest":"1800000000.000500","last_read":"1800000000.000500",
                "unread_count":0,"mention_count":0}],"mpims":[]}])");
    e.be->pollUnreadCountsForTest();
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 3; }));
    e.be->markUnread(d1, ts300);
    fakeslack::pumpFor(200);
    CHECK(e.store.conversation(d1).lastRead == ts300 - 1);

    // 4. Snapshot reports zero through T; unread message T+1 arrives while in flight:
    // T+1 remains unread.
    const Ts ts600 = model::parseTs("1800000000.000600");
    script("client.counts", R"([{"__delay":0.15,"ok":true,"channels":[],
        "ims":[{"id":"D1","latest":"1800000000.000500","last_read":"1800000000.000500",
                "unread_count":0,"mention_count":0}],"mpims":[]}])");
    e.be->pollUnreadCountsForTest();
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 4; }));
    model::Message newer;
    newer.ts   = ts600;
    newer.user = e.store.findUser("UMIRA");
    newer.text = "after snapshot";
    e.store.addMessage(d1, std::move(newer));
    CHECK(e.store.conversation(d1).unread >= 1);
    fakeslack::pumpFor(200);
    CHECK(e.store.conversation(d1).unread >= 1);
    (void)ts100;
}

TEST("dm zero parsing: explicit zero, missing, malformed, and raw/display domains") {
    model::Store store;
    auto         parse = [](std::string_view text) -> json::Document {
        json::Document doc;
        const bool     ok = doc.parse(std::string(text), nullptr);
        CHECK(ok);
        return doc;
    };

    // Explicit zero with latest > last_read (restored investigation repro).
    {
        const auto doc = parse(R"({"id":"D1","is_im":true,"user":"UMIRA",
            "last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"},
            "unread_count":0,"unread_count_display":0,"mention_count":0})");
        const auto obs = slack::mapjson::conversationReadObservation(
            doc.root(), slack::readstate::Source::ConversationInfo
        );
        const auto c = slack::mapjson::toConversation(doc.root(), store);
        CHECK(obs.raw.unread.value.has_value());
        CHECK(*obs.raw.unread.value == 0);
        CHECK(obs.display.unread.value == 0);
        CHECK(obs.raw.mentions.value == 0);
        CHECK(c.unread == 0);
    }

    // Omitted counts remain unknown.
    {
        const auto doc = parse(R"({"id":"D1","is_im":true,"user":"UMIRA",
            "last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"}})");
        const auto obs = slack::mapjson::conversationReadObservation(
            doc.root(), slack::readstate::Source::ConversationInfo
        );
        CHECK(!obs.raw.unread.value.has_value());
        CHECK(!obs.display.unread.value.has_value());
        CHECK(!obs.raw.mentions.value.has_value());
        CHECK(!obs.hasUnreads.has_value());
    }

    // Positive count.
    {
        const auto doc = parse(R"({"unread_count":4,"mention_count":0})");
        const auto obs = slack::mapjson::conversationReadObservation(
            doc.root(), slack::readstate::Source::Roster
        );
        CHECK(obs.raw.unread.value == 4);
        CHECK(obs.raw.mentions.value == 0);
    }

    // Malformed counts (negative, > UINT32_MAX, string, object, null) => unknown, not wraparound.
    for (const char *frag : {
             R"({"unread_count":-1})",
             R"({"unread_count":4294967296})",
             R"({"unread_count":"0"})",
             R"({"unread_count":{}})",
             R"({"unread_count":null})",
             R"({"unread_count":1.5})",
         }) {
        const auto doc = parse(frag);
        const auto obs = slack::mapjson::conversationReadObservation(
            doc.root(), slack::readstate::Source::ConversationInfo
        );
        CHECK(!obs.raw.unread.value.has_value());
    }

    // Raw/display disagreement and domain-only fixtures survive without substitution.
    {
        const auto d1 = parse(R"({"unread_count":2,"unread_count_display":0})");
        const auto o1 = slack::mapjson::conversationReadObservation(
            d1.root(), slack::readstate::Source::ConversationInfo
        );
        CHECK(o1.raw.unread.value == 2);
        CHECK(o1.display.unread.value == 0);

        const auto d2 = parse(R"({"unread_count":0,"unread_count_display":2})");
        const auto o2 = slack::mapjson::conversationReadObservation(
            d2.root(), slack::readstate::Source::ConversationInfo
        );
        CHECK(o2.raw.unread.value == 0);
        CHECK(o2.display.unread.value == 2);

        const auto dOnly = parse(R"({"unread_count_display":3,"mention_count_display":1})");
        const auto oDisp = slack::mapjson::conversationReadObservation(
            dOnly.root(), slack::readstate::Source::Marked
        );
        CHECK(!oDisp.raw.unread.value.has_value());
        CHECK(oDisp.display.unread.value == 3);
        CHECK(!oDisp.raw.mentions.value.has_value());
        CHECK(oDisp.display.mentions.value == 1);

        const auto rOnly = parse(R"({"unread_count":3,"mention_count":1})");
        const auto oRaw  = slack::mapjson::conversationReadObservation(
            rOnly.root(), slack::readstate::Source::ConversationInfo
        );
        CHECK(oRaw.raw.unread.value == 3);
        CHECK(!oRaw.display.unread.value.has_value());
        CHECK(oRaw.raw.mentions.value == 1);
        CHECK(!oRaw.display.mentions.value.has_value());
    }

    // Explicit false vs absent has_unreads, and client.counts dm_count/has_unreads semantics.
    {
        const auto cFalse = parse(R"({"id":"D1","dm_count":0,"mention_count":0,"has_unreads":false})");
        const auto oFalse = slack::mapjson::countsReadObservation(cFalse.root(), true);
        CHECK(oFalse.raw.unread.value == 0);
        CHECK(!oFalse.raw.unread.exact);
        CHECK(!oFalse.display.unread.value.has_value());
        CHECK(oFalse.raw.mentions.value == 0);
        CHECK(oFalse.raw.mentions.exact);
        REQUIRE(oFalse.hasUnreads.has_value());
        CHECK(!*oFalse.hasUnreads);

        const auto cConflict = parse(R"({"id":"D1","dm_count":0,"has_unreads":true})");
        const auto oConflict = slack::mapjson::countsReadObservation(cConflict.root(), true);
        CHECK(oConflict.raw.unread.value == 0);
        CHECK(!oConflict.raw.unread.exact);
        REQUIRE(oConflict.hasUnreads.has_value());
        CHECK(*oConflict.hasUnreads);

        const auto cAbsent = parse(R"({"id":"C1","mention_count":2})");
        const auto oAbsent = slack::mapjson::countsReadObservation(cAbsent.root(), false);
        CHECK(!oAbsent.raw.unread.value.has_value());
        CHECK(!oAbsent.hasUnreads.has_value());
        CHECK(oAbsent.raw.mentions.value == 2);
    }
}

TEST("dm zero polling: zero observation clears stale badge with newer own reply but protects post-dispatch arrivals") {
    if (!haveServer())
        return;
    Env e(kWorkspace, /*session=*/true);
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 1; }));
    const ConvRef d1 = e.conv("D1");
    REQUIRE(d1 != kNoConv);

    const Ts ts200 = model::parseTs("1800000000.000200");
    const Ts ts300 = model::parseTs("1800000000.000300");
    const Ts ts400 = model::parseTs("1800000000.000400");

    // Seed: incoming at 200, own reply at 300, stale unread=1, mentions=1, lastRead=199.
    model::Message m200;
    m200.ts   = ts200;
    m200.user = e.store.findUser("UMIRA");
    m200.text = "incoming";
    e.store.addMessage(d1, std::move(m200));
    model::Message m300;
    m300.ts   = ts300;
    m300.user = e.store.me;
    m300.text = "own reply";
    e.store.addMessage(d1, std::move(m300));
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts200 - 1;
        c.latest   = ts300;
        c.unread   = 1;
        c.mentions = 1;
    });

    // 1. Zero observation at read=200 clears old count=1 despite latest=300 (own reply),
    // even when mention_count is omitted.
    script("client.counts", R"([{"ok":true,"channels":[],
        "ims":[{"id":"D1","latest":"1800000000.000300","last_read":"1800000000.000200",
                "dm_count":0,"has_unreads":false}],"mpims":[]}])");
    e.be->pollUnreadCountsForTest();
    REQUIRE(pumpUntil([&] { return e.store.conversation(d1).lastRead == ts200; }));
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(e.store.conversation(d1).mentions == 0);

    // 2. Delayed zero dispatched before incoming=400 preserves that arrival.
    script("client.counts", R"([{"__delay":0.15,"ok":true,"channels":[],
        "ims":[{"id":"D1","latest":"1800000000.000300","last_read":"1800000000.000200",
                "dm_count":0,"has_unreads":false}],"mpims":[]}])");
    e.be->pollUnreadCountsForTest();
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 2; }));
    set(R"({"conversations.history":{"ok":true,"has_more":false,"messages":[
        {"type":"message","ts":"1800000000.000400","user":"UMIRA","text":"newer incoming"}]}})");
    e.be->setActiveConversation(d1, 0);
    REQUIRE(pumpUntil([&] { return e.store.findMessage(d1, ts400) != nullptr; }));
    e.be->setActiveConversation(kNoConv, 0);
    fakeslack::pumpFor(200);
    CHECK(e.store.conversation(d1).unread >= 1);

    // 3. Duplicate 450 counts once; own send at 500 creates no arrival evidence.
    const Ts ts450 = model::parseTs("1800000000.000450");
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts400;
        c.latest   = ts450;
        c.unread   = 0;
        c.mentions = 0;
    });
    const auto ctxBeforeDup = e.be->captureReadDispatchForTest();
    e.be->recordUnreadArrivalForTest(d1, ts450, 1, 1);
    e.be->recordUnreadArrivalForTest(d1, ts450, 1, 1); // duplicate counts once
    slack::readstate::Observation zeroAt400;
    zeroAt400.source            = slack::readstate::Source::Counts;
    zeroAt400.lastRead          = ts400;
    zeroAt400.latest            = ts450;
    zeroAt400.display.unread    = {0, true};
    zeroAt400.display.mentions  = {0, true};
    zeroAt400.coverage.scope    = slack::readstate::Scope::MainConversation;
    zeroAt400.coverage.through  = ts450;
    zeroAt400.coverage.complete = true;
    e.be->applyReadObservationForTest(d1, zeroAt400, ctxBeforeDup);
    CHECK(e.store.conversation(d1).unread == 1);

    // Clear with own=500 (no arrival recorded for own send).
    const Ts ts500 = model::parseTs("1800000000.000500");
    e.be->markRead(d1, ts450);
    REQUIRE(pumpUntil([&] { return Log().count("conversations.mark") >= 1; }));
    const auto ctxOwn = e.be->captureReadDispatchForTest();
    model::Message own500;
    own500.ts   = ts500;
    own500.user = e.store.me;
    own500.text = "my newer message";
    e.store.addMessage(d1, std::move(own500));
    zeroAt400.lastRead = ts450;
    zeroAt400.latest   = ts500;
    e.be->applyReadObservationForTest(d1, zeroAt400, ctxOwn);
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(e.store.conversation(d1).mentions == 0);

    // 4. Missing unread with mention zero must not clear a known backlog of 5.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.unread   = 5;
        c.mentions = 2;
    });
    slack::readstate::Observation missingUnreadMentionZero;
    missingUnreadMentionZero.source            = slack::readstate::Source::Counts;
    missingUnreadMentionZero.lastRead          = ts450;
    missingUnreadMentionZero.latest            = ts500;
    missingUnreadMentionZero.display.mentions  = {0, true};
    missingUnreadMentionZero.coverage.scope    = slack::readstate::Scope::MainConversation;
    missingUnreadMentionZero.coverage.through  = ts500;
    missingUnreadMentionZero.coverage.complete = true;
    e.be->applyReadObservationForTest(d1, missingUnreadMentionZero, e.be->captureReadDispatchForTest());
    CHECK(e.store.conversation(d1).unread == 5);
    CHECK(e.store.conversation(d1).mentions == 0);

    // 5. Synchronous-observer Read on arrival leaves no stale evidence.
    const Ts ts600 = model::parseTs("1800000000.000600");
    e.store.observe(d1, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Append)
            e.be->markRead(d1, ts600);
    });
    const auto ctxPreSync = e.be->captureReadDispatchForTest();
    set(R"({"conversations.history":{"ok":true,"has_more":false,"messages":[
        {"ts":"1800000000.000600","user":"UMIRA","text":"sync read arrival"}]}})");
    e.be->setActiveConversation(d1, 0);
    REQUIRE(pumpUntil([&] { return e.store.findMessage(d1, ts600) != nullptr; }));
    e.be->setActiveConversation(kNoConv, 0);
    zeroAt400.lastRead = ts500;
    zeroAt400.latest   = ts500;
    e.be->applyReadObservationForTest(d1, zeroAt400, ctxPreSync);
    CHECK(e.store.conversation(d1).unread == 0);
}

TEST("dm zero polling: exact count decreases, disjoint arrivals, overlap, and lower-bound") {
    if (!haveServer())
        return;
    Env e(kWorkspace, /*session=*/true);
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 1; }));
    const ConvRef d1 = e.conv("D1");
    REQUIRE(d1 != kNoConv);

    const Ts ts200 = model::parseTs("1800000000.000200");
    const Ts ts300 = model::parseTs("1800000000.000300");
    const Ts ts400 = model::parseTs("1800000000.000400");
    const Ts ts500 = model::parseTs("1800000000.000500");

    // Start with exact unread=5, mentions=3, lastRead=200, latest=400.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts200;
        c.latest   = ts400;
        c.unread   = 5;
        c.mentions = 3;
    });

    // Exact count decreases from 5 to 2 (and mentions 3 to 1) at newer cursor 300 while latest=400.
    slack::readstate::Observation obs;
    obs.source            = slack::readstate::Source::Counts;
    obs.lastRead          = ts300;
    obs.latest            = ts400;
    obs.display.unread    = {2, true};
    obs.display.mentions  = {1, true};
    obs.coverage.scope    = slack::readstate::Scope::MainConversation;
    obs.coverage.through  = ts400;
    obs.coverage.complete = true;
    e.be->applyReadObservationForTest(d1, obs, e.be->captureReadDispatchForTest());
    CHECK(e.store.conversation(d1).unread == 2);
    CHECK(e.store.conversation(d1).mentions == 1);

    // Lower-bound-only observation of 1 cannot shrink a known exact total of 5.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.unread   = 5;
        c.mentions = 2;
    });
    slack::readstate::Observation lowerBound;
    lowerBound.source     = slack::readstate::Source::Counts;
    lowerBound.lastRead   = ts300;
    lowerBound.latest     = ts400;
    lowerBound.hasUnreads = true;
    e.be->applyReadObservationForTest(d1, lowerBound, e.be->captureReadDispatchForTest());
    CHECK(e.store.conversation(d1).unread == 5);

    // With one definitely disjoint newer arrival (ts500 > obs.latest=400), 2 + 1 = 3.
    const auto ctxDisjoint = e.be->captureReadDispatchForTest();
    e.be->recordUnreadArrivalForTest(d1, ts500, 1, 0);
    obs.display.mentions = {0, true};
    e.be->applyReadObservationForTest(d1, obs, ctxDisjoint);
    CHECK(e.store.conversation(d1).unread == 3);
    CHECK(e.store.conversation(d1).mentions == 0); // unread and mention totals checked independently

    // With overlap unresolved (post-dispatch arrival at ts350 <= obs.latest=400), do not double-count.
    const ConvRef d2 = e.conv("D2");
    REQUIRE(d2 != kNoConv);
    e.store.updateConversation(d2, [&](model::Conversation &c) {
        c.lastRead = ts200;
        c.latest   = ts300;
        c.unread   = 1;
        c.mentions = 0;
    });
    const Ts ts350 = model::parseTs("1800000000.000350");
    const auto ctxOverlap = e.be->captureReadDispatchForTest();
    e.be->recordUnreadArrivalForTest(d2, ts350, 1, 0);
    set(R"({"client.counts":{"ok":true,"channels":[],
        "ims":[{"id":"D2","last_read":"1800000000.000300","latest":"1800000000.000400",
                "unread_count_display":2,"mention_count":0}],"mpims":[]}})");
    const int countsBefore = Log().count("client.counts");
    e.be->applyReadObservationForTest(d2, obs, ctxOverlap);
    CHECK(e.store.conversation(d2).unread == 2); // max(2, 1), not 2 + 1
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") > countsBefore; }));
}

TEST("dm zero polling: 429 queue, transport retry, joined calls, overflow 257, and destruction") {
    if (!haveServer())
        return;
    Env e(kWorkspace, /*session=*/true);
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 1; }));
    const ConvRef d1 = e.conv("D1");
    REQUIRE(d1 != kNoConv);

    // 1. Request waits behind a 429; local markUnread happens while queued -> dispatch context
    // is captured at actual retry dispatch after the cooldown.
    script("conversations.info",
           R"([{"ok":false,"error":"ratelimited","retry_after":1},
               {"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA",
                "last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"},
                "unread_count":0,"unread_count_display":0}}])");
    slack::readstate::DispatchContext ctx429{};
    bool                              done429 = false;
    e.be->readCallObservedForTest(
        "conversations.info",
        "channel=D1",
        [&](const json::Document &, const std::string &err, const slack::readstate::DispatchContext &ctx) {
            CHECK(err.empty());
            ctx429  = ctx;
            done429 = true;
        }
    );
    const Ts ts200 = model::parseTs("1800000000.000200");
    e.be->markUnread(d1, ts200);
    const auto ctxAfterMark = e.be->captureReadDispatchForTest();
    REQUIRE(pumpUntil([&] { return done429; }, 4000));
    CHECK(ctx429.readEpoch >= ctxAfterMark.readEpoch);

    // 2. Transport retry (500 then 200) with a new incoming arrival before retry dispatch.
    script("conversations.info",
           R"([{"__status":500,"ok":false,"error":"internal_error"},
               {"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA",
                "last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"},
                "unread_count":0,"unread_count_display":0}}])");
    slack::readstate::DispatchContext ctxRetry{};
    bool                              doneRetry = false;
    e.be->readCallObservedForTest(
        "conversations.info",
        "channel=D1",
        [&](const json::Document &, const std::string &err, const slack::readstate::DispatchContext &ctx) {
            CHECK(err.empty());
            ctxRetry  = ctx;
            doneRetry = true;
        }
    );
    e.be->recordUnreadArrivalForTest(d1, model::parseTs("1800000000.000350"), 1, 0);
    const auto ctxAfterArrival = e.be->captureReadDispatchForTest();
    REQUIRE(pumpUntil([&] { return doneRetry; }, 4000));
    CHECK(ctxRetry.arrivalEpoch >= ctxAfterArrival.arrivalEpoch);

    // 3. Joined Background calls share the exact same DispatchContext.
    // Issue one background call first so pacedBusy is true and the next two sit in `paced` and join.
    e.be->readCallObservedForTest(
        "conversations.info",
        "channel=D1&prime=1",
        [](const json::Document &, const std::string &, const slack::readstate::DispatchContext &) {},
        /*background=*/true
    );
    slack::readstate::DispatchContext joinA{}, joinB{};
    int                               joinedCount = 0;
    e.be->readCallObservedForTest(
        "conversations.info",
        "channel=D1&join=1",
        [&](const json::Document &, const std::string &, const slack::readstate::DispatchContext &ctx) {
            joinA = ctx;
            ++joinedCount;
        },
        /*background=*/true
    );
    e.be->readCallObservedForTest(
        "conversations.info",
        "channel=D1&join=1",
        [&](const json::Document &, const std::string &, const slack::readstate::DispatchContext &ctx) {
            joinB = ctx;
            ++joinedCount;
        },
        /*background=*/true
    );
    REQUIRE(pumpUntil([&] { return joinedCount == 2; }, 4000));
    CHECK(joinA.dispatchId == joinB.dispatchId);

    // 4. Overflow at entry 257 retains unread state and recovers on a clean post-invalidation snapshot.
    e.be->markRead(d1, model::parseTs("1800000000.000350"));
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.unread   = 3;
        c.mentions = 1;
    });
    const auto preOverflowCtx = e.be->captureReadDispatchForTest();
    set(R"({"client.counts":{"ok":true,"channels":[],
        "ims":[{"id":"D1","last_read":"1800000000.002000","latest":"1800000000.002000",
                "has_unreads":false,"mention_count":0}],"mpims":[]}})");
    for (int i = 1; i <= 257; ++i) {
        const Ts ts = model::parseTs("1800000000.001000") + i;
        e.be->recordUnreadArrivalForTest(d1, ts, 1, 0);
    }
    slack::readstate::Observation zeroObs;
    zeroObs.source            = slack::readstate::Source::Counts;
    zeroObs.lastRead          = model::parseTs("1800000000.000350");
    zeroObs.latest            = model::parseTs("1800000000.000400");
    zeroObs.display.unread    = {0, true};
    zeroObs.display.mentions  = {0, true};
    zeroObs.coverage.scope    = slack::readstate::Scope::MainConversation;
    zeroObs.coverage.through  = zeroObs.latest;
    zeroObs.coverage.complete = true;
    e.be->applyReadObservationForTest(d1, zeroObs, preOverflowCtx);
    CHECK(e.store.conversation(d1).unread == 3);
    REQUIRE(pumpUntil([&] { return e.store.conversation(d1).unread == 0; }, 4000));

    // 5. Destroy backend during delayed response: no callback touches freed state.
    bool destroyedCallbackRan = false;
    script("conversations.info", R"([{"__delay":0.15,"ok":true,"channel":{"id":"D1"}}])");
    e.be->readCallObservedForTest(
        "conversations.info",
        "channel=D1&destroy=1",
        [&](const json::Document &, const std::string &, const slack::readstate::DispatchContext &) {
            destroyedCallbackRan = true;
        }
    );
    e.be.reset();
    fakeslack::pumpFor(200);
    CHECK(!destroyedCallbackRan);
}

TEST("dm zero sources: roster reload, two-page context isolation, coherent Meta, and cross-entry observer") {
    if (!haveServer())
        return;
    Env e(kWorkspace, /*session=*/true);
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 1; }));
    const ConvRef d1 = e.conv("D1");
    const ConvRef d2 = e.conv("D2");
    REQUIRE(d1 != kNoConv);
    REQUIRE(d2 != kNoConv);

    const Ts ts200 = model::parseTs("1800000000.000200");
    const Ts ts300 = model::parseTs("1800000000.000300");

    // 1. Roster reload: explicit zero clears D1 despite latest=300; missing count preserves D2=5.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts200 - 1;
        c.latest   = ts300;
        c.unread   = 1;
        c.mentions = 1;
    });
    e.store.updateConversation(d2, [&](model::Conversation &c) {
        c.lastRead = ts200;
        c.latest   = ts300;
        c.unread   = 5;
        c.mentions = 2;
    });

    // Track coherent Meta emissions: each changed conversation emits at most one Meta update.
    int metaD1 = 0;
    e.store.observe(d1, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Meta)
            ++metaD1;
    });

    set(R"({"users.conversations":{"ok":true,"channels":[
        {"id":"C1","name":"general","is_channel":true,"is_member":true},
        {"id":"D1","is_im":true,"user":"UMIRA","last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"},"unread_count":0,"unread_count_display":0,"mention_count":0,"mention_count_display":0}],
        "response_metadata":{"next_cursor":"p2"}},
        "users.conversations?cursor=p2":{"ok":true,"channels":[
        {"id":"D2","is_im":true,"user":"UEXT","last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"}},
        {"id":"G1","is_mpim":true,"name":"mpdm-me--mira--jonas-1"},
        {"id":"C2","name":"random","is_channel":true,"is_member":true,"is_muted":true}],
        "response_metadata":{"next_cursor":""}}})");

    const int ucBefore = Log().count("users.conversations");
    e.be->reloadConversationsForTest();
    REQUIRE(pumpUntil([&] { return Log().count("users.conversations") >= ucBefore + 2; }));
    REQUIRE(pumpUntil([&] { return e.store.conversation(d1).lastRead == ts200; }));
    // Roster is UnknownActivity: preserves existing unread when latest > lastRead, advances lastRead.
    CHECK(e.store.conversation(d1).unread == 1);
    CHECK(metaD1 == 1);
    CHECK(e.store.conversation(d2).unread == 5);
    CHECK(e.store.conversation(d2).mentions == 2);

    // 2. Two roster pages with different dispatch contexts + cross-entry synchronous observer:
    // Page 1 has D1; Page 2 has D2 (delayed). While Page 1 is in flight (or via observer on D1),
    // D2 is marked unread -> Page 1's older context cannot clear D1 if marked, and Page 2 cannot
    // lend its freshness to Page 1.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts200 - 1;
        c.latest   = ts300;
        c.unread   = 1;
        c.mentions = 1;
    });
    e.store.updateConversation(d2, [&](model::Conversation &c) {
        c.lastRead = ts300;
        c.latest   = ts300;
        c.unread   = 0;
        c.mentions = 0;
    });
    // Add a message at ts300 on D2 so markUnread(d2, ts300) sets desired = ts300 - 1.
    model::Message mD2;
    mD2.ts   = ts300;
    mD2.user = e.store.findUser("UEXT");
    mD2.text = "d2 msg";
    e.store.addMessage(d2, std::move(mD2));

    bool markedD2InObserver = false;
    e.store.observe(d1, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Meta && !markedD2InObserver) {
            markedD2InObserver = true;
            e.be->markUnread(d2, ts300);
        }
    });

    script("users.conversations", R"([
        {"ok":true,"channels":[
            {"id":"C1","name":"general","is_channel":true,"is_member":true},
            {"id":"D1","is_im":true,"user":"UMIRA","last_read":"1800000000.000200","latest":{"ts":"1800000000.000300"},"unread_count":0,"unread_count_display":0,"mention_count":0,"mention_count_display":0}],
         "response_metadata":{"next_cursor":"p2"}},
        {"ok":true,"channels":[
            {"id":"D2","is_im":true,"user":"UEXT","last_read":"1800000000.000300","latest":{"ts":"1800000000.000300"},"unread_count":0,"unread_count_display":0,"mention_count":0,"mention_count_display":0},
            {"id":"G1","is_mpim":true,"name":"mpdm-me--mira--jonas-1"},
            {"id":"C2","name":"random","is_channel":true,"is_member":true,"is_muted":true}],
         "response_metadata":{"next_cursor":""}}])");
    const int uc2 = Log().count("users.conversations");
    e.be->reloadConversationsForTest();
    REQUIRE(pumpUntil([&] { return Log().count("users.conversations") >= uc2 + 2; }));
    REQUIRE(pumpUntil([&] { return markedD2InObserver; }));
    // D2 was marked unread by D1's observer before D2's roster merge ran: D2's markUnread must survive!
    CHECK(e.store.conversation(d2).lastRead == ts300 - 1);
    CHECK(e.store.conversation(d2).unread >= 1);

    // 3. Empty client.counts snapshot must not silently clear every conversation.
    script("client.counts", R"([{"ok":true,"channels":[],"ims":[],"mpims":[]}])");
    e.be->pollUnreadCountsForTest();
    fakeslack::pumpFor(100);
    CHECK(e.store.conversation(d2).unread >= 1);
}

TEST("dm zero trace: opt-in slack-read trace logs decisions without leaking names, IDs, text, or tokens") {
    if (!haveServer())
        return;
    Env e(kWorkspace, /*session=*/true);
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 1; }));
    const ConvRef d1 = e.conv("D1");
    REQUIRE(d1 != kNoConv);

    // Seed sentinel name/text that must NEVER appear in slack-read trace lines.
    e.store.updateConversation(d1, [](model::Conversation &c) {
        c.localName = "SENTINEL_DM_NAME";
    });
    model::Message sentinelMsg;
    sentinelMsg.ts   = model::parseTs("1800000000.000200");
    sentinelMsg.user = e.store.findUser("UMIRA");
    sentinelMsg.text = "SENTINEL_SECRET_BODY";
    e.store.addMessage(d1, std::move(sentinelMsg));

    const std::string logPath =
        (std::filesystem::temp_directory_path() / "msga-slack-read-trace.log").string();
    std::remove(logPath.c_str());
    const base::LogLevel prevLevel = base::logLevel();
    base::setLogLevel(base::LogLevel::Info);
    REQUIRE(base::setLogFile(logPath.c_str()));

    auto readLog = [&] {
        std::ifstream in(logPath);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    // 1. Disabled mode emits no slack-read records.
    base::test::unsetEnv("MSGA_SLACK_READ_TRACE");
    slack::readstate::Observation obs;
    obs.source            = slack::readstate::Source::Counts;
    obs.lastRead          = model::parseTs("1800000000.000200");
    obs.latest            = model::parseTs("1800000000.000200");
    obs.display.unread    = {0, true};
    obs.display.mentions  = {0, true};
    obs.coverage.scope    = slack::readstate::Scope::MainConversation;
    obs.coverage.through  = obs.latest;
    obs.coverage.complete = true;
    const auto staleCtx  = e.be->captureReadDispatchForTest();
    e.be->applyReadObservationForTest(d1, obs, e.be->captureReadDispatchForTest());
    CHECK(readLog().find("slack-read") == std::string::npos);

    // 2. Enabled mode logs accept, reject, and defer decisions with context, and no sentinels.
    base::test::setEnv("MSGA_SLACK_READ_TRACE", "1");
    e.be->applyReadObservationForTest(d1, obs, e.be->captureReadDispatchForTest()); // accept
    e.be->applyReadObservationForTest(d1, obs, staleCtx);                         // reject

    // Defer via ambiguous marked event.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.latest = model::parseTs("1800000000.000300");
    });
    slack::readstate::Observation markedObs;
    markedObs.source           = slack::readstate::Source::Marked;
    markedObs.lastRead         = model::parseTs("1800000000.000200");
    markedObs.display.unread   = {0, true};
    markedObs.display.mentions = {0, true};
    e.be->applyMarkedEventForTest("D1", markedObs);

    base::setLogFile("");
    base::setLogLevel(prevLevel);
    base::test::unsetEnv("MSGA_SLACK_READ_TRACE");

    const std::string logged = readLog();
    std::remove(logPath.c_str());
    CHECK(logged.find("slack-read:") != std::string::npos);
    CHECK(logged.find("scope=") != std::string::npos);
    CHECK(logged.find("raw=(") != std::string::npos);
    CHECK(logged.find("disp=(") != std::string::npos);
    CHECK(logged.find("hu=") != std::string::npos);
    CHECK(logged.find("cov=(") != std::string::npos);
    CHECK(logged.find("obs_latest=") != std::string::npos);
    CHECK(logged.find("act_latest=") != std::string::npos);
    CHECK(logged.find("arr=(ret=") != std::string::npos);
    CHECK(logged.find("decision=accept") != std::string::npos);
    CHECK(logged.find("decision=superseded-observation") != std::string::npos);
    CHECK(logged.find("decision=defer-ambiguous") != std::string::npos);
    CHECK(logged.find("ctx=(") != std::string::npos);
    CHECK(logged.find("SENTINEL_DM_NAME") == std::string::npos);
    CHECK(logged.find("SENTINEL_SECRET_BODY") == std::string::npos);
    CHECK(logged.find("xoxc-test") == std::string::npos);
    CHECK(logged.find("UMIRA") == std::string::npos);
    CHECK(logged.find("D1") == std::string::npos);
}

TEST("dm scope accounting: missing counts preserve totals and old zero snapshots preserve newer arrivals") {
    if (!haveServer())
        return;
    Env e(kWorkspace, /*session=*/true);
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 1; }));
    const ConvRef d1 = e.conv("D1");
    REQUIRE(d1 != kNoConv);

    const Ts ts100 = model::parseTs("1800000000.000100");
    const Ts ts300 = model::parseTs("1800000000.000300");
    const Ts ts400 = model::parseTs("1800000000.000400");

    // 1. Missing-count observation must not double-count an arrival already in current totals.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts100;
        c.latest   = ts400;
        c.unread   = 1;
        c.mentions = 1;
    });
    const auto ctxBeforeArrival = e.be->captureReadDispatchForTest();
    e.be->recordUnreadArrivalForTest(d1, ts400, 1, 1);

    slack::readstate::Observation missingCounts;
    missingCounts.source   = slack::readstate::Source::Roster;
    missingCounts.lastRead = ts100;
    missingCounts.latest   = ts300;

    e.be->applyReadObservationForTest(d1, missingCounts, ctxBeforeArrival);
    CHECK(e.store.conversation(d1).unread == 1);
    CHECK(e.store.conversation(d1).mentions == 1);

    // Repeating the missing-count observation must still preserve 1/1.
    e.be->applyReadObservationForTest(d1, missingCounts, e.be->captureReadDispatchForTest());
    CHECK(e.store.conversation(d1).unread == 1);
    CHECK(e.store.conversation(d1).mentions == 1);

    // 2. Two consecutive old zero snapshots must both preserve an arrival at 400
    // even when the arrival was known before dispatch.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts100;
        c.latest   = ts400;
        c.unread   = 1;
        c.mentions = 0;
    });
    const auto ctxAfterArrival = e.be->captureReadDispatchForTest();

    slack::readstate::Observation oldZero;
    oldZero.source            = slack::readstate::Source::Counts;
    oldZero.lastRead          = ts100;
    oldZero.latest            = ts300;
    oldZero.hasUnreads        = false;
    oldZero.display.unread    = {0, true};
    oldZero.display.mentions  = {0, true};
    oldZero.coverage.scope    = slack::readstate::Scope::MainConversation;
    oldZero.coverage.through  = ts300;
    oldZero.coverage.complete = true;

    e.be->applyReadObservationForTest(d1, oldZero, ctxAfterArrival);
    CHECK(e.store.conversation(d1).unread == 1);

    e.be->applyReadObservationForTest(d1, oldZero, e.be->captureReadDispatchForTest());
    CHECK(e.store.conversation(d1).unread == 1);

    // A later comparable baseline covering 400 absorbs it exactly once.
    slack::readstate::Observation coveringBaseline;
    coveringBaseline.source            = slack::readstate::Source::Counts;
    coveringBaseline.lastRead          = ts100;
    coveringBaseline.latest            = ts400;
    coveringBaseline.display.unread    = {1, true};
    coveringBaseline.display.mentions  = {0, true};
    coveringBaseline.coverage.scope    = slack::readstate::Scope::MainConversation;
    coveringBaseline.coverage.through  = ts400;
    coveringBaseline.coverage.complete = true;

    e.be->applyReadObservationForTest(d1, coveringBaseline, e.be->captureReadDispatchForTest());
    CHECK(e.store.conversation(d1).unread == 1);
    e.be->applyReadObservationForTest(d1, coveringBaseline, e.be->captureReadDispatchForTest());
    CHECK(e.store.conversation(d1).unread == 1);

    // Scripted HTTP variant: client.counts omitting counts and then returning old zero.
    const Ts ts500 = model::parseTs("1800000000.000500");
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts100;
        c.latest   = ts500;
        c.unread   = 1;
        c.mentions = 1;
    });
    e.be->recordUnreadArrivalForTest(d1, ts500, 1, 1);

    set(R"({"client.counts":{"ok":true,"channels":[],
        "ims":[{"id":"D1","latest":"1800000000.000300","last_read":"1800000000.000100"}],
        "mpims":[]}})");
    e.be->pollUnreadCountsForTest();
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 2; }));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).unread == 1);
    CHECK(e.store.conversation(d1).mentions == 1);

    set(R"({"client.counts":{"ok":true,"channels":[],
        "ims":[{"id":"D1","latest":"1800000000.000300","last_read":"1800000000.000100",
                "has_unreads":false,"dm_count":0}],"mpims":[]}})");
    e.be->pollUnreadCountsForTest();
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 3; }));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).unread == 1);
    e.be->pollUnreadCountsForTest();
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 4; }));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).unread == 1);

    // 3. Independent unread/mention omissions, exact 5->2 + 1 disjoint, and missing-count
    // must not clear overflow invalidation.
    const Ts ts600 = model::parseTs("1800000000.000600");
    const Ts ts700 = model::parseTs("1800000000.000700");
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = ts100;
        c.latest   = ts600;
        c.unread   = 5;
        c.mentions = 2;
    });
    const auto ctxBeforeExact = e.be->captureReadDispatchForTest();
    e.be->recordUnreadArrivalForTest(d1, ts700, 1, 0);
    slack::readstate::Observation exactTwo;
    exactTwo.source            = slack::readstate::Source::Counts;
    exactTwo.lastRead          = ts100;
    exactTwo.latest            = ts600;
    exactTwo.display.unread    = {2, true};
    exactTwo.display.mentions  = {1, true};
    exactTwo.coverage.scope    = slack::readstate::Scope::MainConversation;
    exactTwo.coverage.through  = ts600;
    exactTwo.coverage.complete = true;
    e.be->applyReadObservationForTest(d1, exactTwo, ctxBeforeExact);
    CHECK(e.store.conversation(d1).unread == 3);
    CHECK(e.store.conversation(d1).mentions == 1);

    // Overflow invalidation: missing-count response must not clear overflow invalidation.
    for (int i = 1; i <= 260; ++i) {
        const Ts ts = ts700 + i;
        e.be->recordUnreadArrivalForTest(d1, ts, 1, 0);
    }
    const auto ctxAfterOverflow = e.be->captureReadDispatchForTest();
    slack::readstate::Observation noCountsObs;
    noCountsObs.source   = slack::readstate::Source::Roster;
    noCountsObs.lastRead = ts100;
    noCountsObs.latest   = ts700 + 260;
    e.be->applyReadObservationForTest(d1, noCountsObs, ctxAfterOverflow);
    // Overflow must still be active: a subsequent missing-count observation cannot clear it,
    // and an exact baseline clears it once caught up.
    slack::readstate::Observation exactCatchup;
    exactCatchup.source            = slack::readstate::Source::Counts;
    exactCatchup.lastRead          = ts100;
    exactCatchup.latest            = ts700 + 260;
    exactCatchup.display.unread    = {4, true};
    exactCatchup.display.mentions  = {0, true};
    exactCatchup.coverage.scope    = slack::readstate::Scope::MainConversation;
    exactCatchup.coverage.through  = ts700 + 260;
    exactCatchup.coverage.complete = true;
    e.be->applyReadObservationForTest(d1, exactCatchup, e.be->captureReadDispatchForTest());
    CHECK(e.store.conversation(d1).unread == 4);
}

TEST("dm scope sources: main-DM read stays zero across contradictory info and roster refreshes") {
    if (!haveServer())
        return;
    Env e(kWorkspace, /*session=*/true);
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 1; }));
    const ConvRef d1 = e.conv("D1");
    REQUIRE(d1 != kNoConv);

    const Ts mainRead = model::parseTs("1794909941.811299");
    const Ts reply    = model::parseTs("1794910537.557059");
    const Ts newMain  = model::parseTs("1794910538.000000");

    // Seed initial false badge 1 on D1 with mainRead < reply.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = mainRead;
        c.latest   = reply;
        c.unread   = 1;
        c.mentions = 1;
    });
    uint32_t maxObservedUnreadDuringZero = 0;
    bool     trackTransitions            = false;
    e.store.observe(d1, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Meta && trackTransitions)
            maxObservedUnreadDuringZero =
                std::max(maxObservedUnreadDuringZero, e.store.conversation(d1).unread);
    });

    // 1. Accept main read via client.counts (has_unreads=false, dm_count absent).
    set(R"({"client.counts":{"ok":true,"channels":[],
        "ims":[{"id":"D1","latest":"1794909869.919749","last_read":"1794909941.811299",
                "has_unreads":false,"mention_count":0}],
        "mpims":[]}})");
    e.be->pollUnreadCountsForTest();
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 2; }));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(e.store.conversation(d1).mentions == 0);

    // 2. Contradictory conversations.info (raw/display=1, latest=reply) must NOT flap badge to 1.
    trackTransitions = true;
    script("conversations.info", R"([{"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA",
        "last_read":"1794909941.811299",
        "latest":{"ts":"1794910537.557059","thread_ts":"1794909869.919749"},
        "unread_count":1,"unread_count_display":1,"mention_count":0,"mention_count_display":0}}])");
    const int infoCountBefore = Log().count("conversations.info");
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
    REQUIRE(pumpUntil([&] { return Log().count("conversations.info") >= infoCountBefore + 1; }));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(maxObservedUnreadDuringZero == 0);

    // 3. Roster refresh with info=1 also must not restore badge.
    script("users.conversations", R"([{"ok":true,"channels":[
        {"id":"D1","is_im":true,"user":"UMIRA","last_read":"1794909941.811299",
         "latest":{"ts":"1794910537.557059","thread_ts":"1794909869.919749"},
         "unread_count":1,"unread_count_display":1}],
        "response_metadata":{"next_cursor":""}}])");
    const int uc = Log().count("users.conversations");
    e.be->reloadConversationsForTest();
    REQUIRE(pumpUntil([&] { return Log().count("users.conversations") >= uc + 1; }));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).unread == 0);
    CHECK(maxObservedUnreadDuringZero == 0);

    // 4. Genuinely new top-level message becomes unread and survives an older zero snapshot.
    trackTransitions = false;
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.latest   = newMain;
        c.unread   = 1;
        c.mentions = 1;
    });
    e.be->recordUnreadArrivalForTest(d1, newMain, 1, 1);
    set(R"({"client.counts":{"ok":true,"channels":[],
        "ims":[{"id":"D1","latest":"1794909869.919749","last_read":"1794909941.811299",
                "has_unreads":false,"mention_count":0}],
        "mpims":[]}})");
    e.be->pollUnreadCountsForTest();
    REQUIRE(pumpUntil([&] { return Log().count("client.counts") >= 3; }));
    fakeslack::pumpFor(50);
    CHECK(e.store.conversation(d1).unread == 1);

    // 5. History fallback when client.counts is unavailable: two-page scan retains page-1
    // context across an arrival between pages, and missing cursor stays unknown.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.lastRead = mainRead;
        c.latest   = reply;
        c.unread   = 1;
        c.mentions = 1;
    });
    script("client.counts", R"([{"ok":false,"error":"unknown_method"}])");
    script("conversations.info", R"([{"ok":true,"channel":{"id":"D1","is_im":true,"user":"UMIRA",
        "last_read":"1794909941.811299",
        "latest":{"ts":"1794910537.557059","thread_ts":"1794909869.919749"},
        "unread_count":1,"unread_count_display":1}}])");
    script("conversations.history", R"([
        {"ok":true,"has_more":true,"response_metadata":{"next_cursor":"h2"},
         "messages":[{"type":"message","ts":"1794910537.557059","thread_ts":"1794909869.919749","user":"UMIRA","text":"reply"}]},
        {"__delay":0.15,"ok":true,"has_more":false,"messages":[]}])");
    const int histBefore = Log().count("conversations.history");
    e.be->reconcileConversationReadStateForTest("D1");
    REQUIRE(pumpUntil([&] { return Log().count("conversations.history") >= histBefore + 2; }));
    // Deliver a genuinely new main message while page 2 is in flight.
    e.store.updateConversation(d1, [&](model::Conversation &c) {
        c.latest   = newMain;
        c.unread   = 1;
        c.mentions = 1;
    });
    e.be->recordUnreadArrivalForTest(d1, newMain, 1, 1);
    fakeslack::pumpFor(200);
    // Page-1 context was retained, so newMain arrived after page 1 and MUST NOT be cleared!
    CHECK(e.store.conversation(d1).unread == 1);
}

#endif // !_WIN32
