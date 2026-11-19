#include <algorithm>
#include <cstdint>
#include <cstdio>
namespace model { using Ts = int64_t; enum class NotifyLevel {Default,Nothing}; struct Conversation { Ts lastRead=0,latest=0; uint32_t unread=0,mentions=0; bool muted=false; NotifyLevel notify=NotifyLevel::Default; bool isDirect() const { return true; } }; }
using model::Ts;
struct SlackBackend { void reconcileCounts(const model::Conversation&, Ts, Ts, uint32_t, uint32_t, bool, bool, Ts&, Ts&, uint32_t&, uint32_t&) const; };
void SlackBackend::reconcileCounts(
    const model::Conversation &cur,
    Ts                         obsLastRead,
    Ts                         obsLatest,
    uint32_t                   obsUnread,
    uint32_t                   obsMentions,
    bool                       seedCounts,
    bool                       fromCounts,
    Ts                        &outLastRead,
    Ts                        &outLatest,
    uint32_t                  &outUnread,
    uint32_t                  &outMentions
) const {
    outLastRead = std::max(cur.lastRead, obsLastRead);
    outLatest   = std::max(cur.latest, obsLatest);
    outUnread   = cur.unread;
    outMentions = cur.mentions;
    if (outLatest <= outLastRead) {
        outUnread   = 0;
        outMentions = 0;
        return;
    }
    if (fromCounts && obsUnread == 0 && obsLastRead >= obsLatest && cur.latest <= obsLatest) {
        outUnread   = 0;
        outMentions = 0;
        return;
    }
    if (obsUnread > 0 && obsLatest > cur.lastRead &&
        (seedCounts || (!fromCounts && obsLastRead >= cur.lastRead))) {
        const bool     muted = cur.muted || cur.notify == model::NotifyLevel::Nothing;
        const uint32_t m     = cur.isDirect() ? std::max(obsUnread, obsMentions) : obsMentions;
        outUnread            = std::max(outUnread, muted ? m : std::max(obsUnread, m));
        outMentions          = std::max(outMentions, m);
    }
}
int main() {
 model::Conversation c; c.lastRead=1794909941811299LL;c.latest=1794910537557059LL;c.unread=1;c.mentions=1;
 SlackBackend backend;Ts read=0,latest=0;uint32_t unread=0,mentions=0;
 backend.reconcileCounts(c,1794909941811299LL,1794909869919749LL,0,0,false,true,read,latest,unread,mentions);
 std::printf("1. Current badge + live client.counts(read): 1 -> %u\n",unread);
 c.unread=0;c.mentions=0;
 backend.reconcileCounts(c,1794909941811299LL,1794910537557059LL,1,0,false,false,read,latest,unread,mentions);
 std::printf("2. Locally cleared badge + live conversations.info: 0 -> %u\n",unread);
 c.lastRead=read;c.latest=latest;c.unread=unread;c.mentions=mentions;
 backend.reconcileCounts(c,1794909941811299LL,1794909869919749LL,0,0,false,true,read,latest,unread,mentions);
 std::printf("3. Next live client.counts(read): 1 -> %u (mentions=%u)\n",unread,mentions);
 std::printf("Blocking comparison: thread-inclusive latest <= top-level latest is %s\n",c.latest<=1794909869919749LL ? "true":"false");
 return unread==0 && mentions==0 ? 0:1;
}
