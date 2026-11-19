#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <optional>
#include <unordered_map>
#include <vector>
namespace model { using Ts=int64_t; using ConvRef=uint32_t; enum class NotifyLevel{Default,Nothing}; struct Conversation{std::string id="D";Ts lastRead=0,latest=0;uint32_t unread=0,mentions=0;bool muted=false;NotifyLevel notify=NotifyLevel::Default;bool isDirect()const{return true;}}; }
using model::Ts;using model::ConvRef;
namespace slack::readstate {

enum class Source { ConversationInfo, Roster, Counts, Marked, Reconcile };

struct Count {
    std::optional<uint32_t> value;
    bool                    exact = true;
};

struct CountPair {
    Count unread;
    Count mentions;
};

struct Observation {
    std::optional<model::Ts> lastRead;
    std::optional<model::Ts> latest;
    CountPair                raw;
    CountPair                display;
    std::optional<bool>      hasUnreads;
    Source                   source = Source::ConversationInfo;
};

struct DispatchContext {
    uint64_t dispatchId       = 0;
    uint64_t readEpoch        = 0;
    uint64_t arrivalEpoch     = 0;
    uint64_t observationEpoch = 0;
};

struct Arrival {
    model::Ts ts       = 0;
    uint64_t  sequence = 0;
    uint32_t  unread   = 0;
    uint32_t  mentions = 0;
};

struct ReadDecision {
    model::Ts   lastRead = 0;
    model::Ts   latest   = 0;
    uint32_t    unread   = 0;
    uint32_t    mentions = 0;
    bool        accept   = false;
    bool        refresh  = false;
    const char *reason   = "";
};

struct ConversationObservation {
    model::Conversation info;
    Observation         read;
    DispatchContext     context;
};

} // namespace slack::readstate

struct SlackBackend{
 struct Store{model::Conversation record;size_t conversationCount()const{return 1;} const model::Conversation &conversation(ConvRef)const{return record;}} _store;
 struct ReadIntent{bool pending=false;uint64_t epoch=0,acceptedDispatchId=0,acceptedObsEpoch=0,invalidatedEpoch=0;std::vector<slack::readstate::Arrival>arrivals;};
 std::unordered_map<std::string,ReadIntent> _readIntent; uint64_t _observationEpoch=0; void queueReadReconcile(const std::string&){} bool commitReadDecision(ConvRef,const slack::readstate::ReadDecision&,const slack::readstate::DispatchContext&);
 slack::readstate::ReadDecision prepareReadObservation(ConvRef,const model::Conversation&,const slack::readstate::Observation&,const slack::readstate::DispatchContext&,bool=false)const;
};
namespace readstate=slack::readstate;
readstate::ReadDecision SlackBackend::prepareReadObservation(
    ConvRef                           c,
    const model::Conversation        &candidate,
    const readstate::Observation     &obs,
    const readstate::DispatchContext &context,
    bool                              allowBackwardCursor
) const {
    readstate::ReadDecision out{
        candidate.lastRead, candidate.latest, candidate.unread, candidate.mentions, false, false, ""
    };
    if (c >= _store.conversationCount()) {
        out.reason = "invalid-conv";
        return out;
    }
    const ReadIntent *in = nullptr;
    if (const auto it = _readIntent.find(candidate.id); it != _readIntent.end())
        in = &it->second;

    if (in) {
        if (in->pending || context.readEpoch < in->epoch) {
            out.reason = "stale-intent";
            return out;
        }
        if (context.dispatchId < in->acceptedDispatchId ||
            (context.dispatchId == 0 && context.observationEpoch < in->acceptedObsEpoch)) {
            out.refresh = false;
            out.reason  = "superseded-observation";
            return out;
        }
        if (in->invalidatedEpoch > 0) {
            const bool newerArrival = std::any_of(
                in->arrivals.begin(),
                in->arrivals.end(),
                [&](const readstate::Arrival &a) { return a.sequence > context.arrivalEpoch; }
            );
            if (context.arrivalEpoch < in->invalidatedEpoch || newerArrival) {
                out.refresh = true;
                out.reason  = "evidence-overflow";
                return out;
            }
        }
    }

    const Ts newLastRead = allowBackwardCursor && obs.lastRead
                               ? *obs.lastRead
                               : std::max(candidate.lastRead, obs.lastRead.value_or(0));
    const Ts newLatest   = std::max(candidate.latest, obs.latest.value_or(0));

    std::optional<uint32_t> exactUnread;
    std::optional<uint32_t> lowerBoundUnread;
    if (obs.display.unread.value.has_value() && obs.display.unread.exact) {
        exactUnread = *obs.display.unread.value;
    } else if (
        obs.source != readstate::Source::Reconcile &&
        obs.source != readstate::Source::Marked &&
        obs.source != readstate::Source::Counts &&
        obs.raw.unread.value.has_value() && obs.raw.unread.exact
    ) {
        exactUnread = *obs.raw.unread.value;
    }
    if (!exactUnread.has_value() && obs.hasUnreads.has_value()) {
        if (!*obs.hasUnreads)
            exactUnread = 0;
        else
            lowerBoundUnread = 1;
    }
    if (obs.source == readstate::Source::Counts && !exactUnread.has_value() &&
        obs.raw.unread.value.has_value() && *obs.raw.unread.value > 0) {
        lowerBoundUnread = std::max(lowerBoundUnread.value_or(0), *obs.raw.unread.value);
    }

    std::optional<uint32_t> exactMentions;
    if (obs.display.mentions.value.has_value() && obs.display.mentions.exact) {
        exactMentions = *obs.display.mentions.value;
    } else if (
        obs.source != readstate::Source::Reconcile &&
        obs.source != readstate::Source::Marked &&
        obs.raw.mentions.value.has_value() && obs.raw.mentions.exact
    ) {
        exactMentions = *obs.raw.mentions.value;
    }

    uint32_t disjointUnread = 0, overlapUnread = 0;
    uint32_t disjointMentions = 0, overlapMentions = 0;
    if (in) {
        for (const readstate::Arrival &a : in->arrivals) {
            if (a.ts <= newLastRead)
                continue;
            const bool postDispatch = a.sequence > context.arrivalEpoch;
            const bool afterObsLatest = !obs.latest.has_value() || a.ts > *obs.latest;
            if (afterObsLatest) {
                disjointUnread += a.unread;
                disjointMentions += a.mentions;
            } else if (postDispatch) {
                overlapUnread += a.unread;
                overlapMentions += a.mentions;
            }
        }
    }

    uint32_t newUnread   = candidate.unread;
    uint32_t newMentions = candidate.mentions;
    bool     needsRefresh = false;

    if (newLatest <= newLastRead && disjointUnread == 0 && overlapUnread == 0 &&
        disjointMentions == 0 && overlapMentions == 0) {
        newUnread   = 0;
        newMentions = 0;
    } else {
        if (exactUnread.has_value()) {
            if (*exactUnread == 0) {
                newUnread = disjointUnread + overlapUnread;
                if (overlapUnread > 0)
                    needsRefresh = true;
            } else {
                newUnread = std::max(*exactUnread, overlapUnread) + disjointUnread;
                if (overlapUnread > 0)
                    needsRefresh = true;
            }
        } else if (lowerBoundUnread.has_value()) {
            newUnread = std::max({candidate.unread, *lowerBoundUnread, disjointUnread + overlapUnread});
        } else {
            newUnread = candidate.unread + disjointUnread;
        }

        if (exactMentions.has_value()) {
            if (*exactMentions == 0) {
                newMentions = disjointMentions + overlapMentions;
                if (overlapMentions > 0)
                    needsRefresh = true;
            } else {
                newMentions = std::max(*exactMentions, overlapMentions) + disjointMentions;
                if (overlapMentions > 0)
                    needsRefresh = true;
            }
        } else if (exactUnread.has_value() && *exactUnread == 0 && disjointUnread == 0 &&
                   overlapUnread == 0) {
            newMentions = 0;
        } else if (candidate.isDirect() && exactUnread.has_value()) {
            newMentions = newUnread;
        } else if (candidate.isDirect() && obs.source == readstate::Source::Counts &&
                   lowerBoundUnread.has_value()) {
            newMentions = std::max(candidate.mentions, *lowerBoundUnread) + disjointMentions;
        } else {
            newMentions = candidate.mentions + disjointMentions;
        }

        const bool muted = candidate.muted || candidate.notify == model::NotifyLevel::Nothing;
        if (muted && !candidate.isDirect())
            newUnread = newMentions;
    }

    out.lastRead = newLastRead;
    out.latest   = newLatest;
    out.unread   = newUnread;
    out.mentions = newMentions;
    out.accept   = true;
    out.refresh  = needsRefresh;
    out.reason   = needsRefresh ? "accept-refresh" : "accept";
    return out;
}

bool SlackBackend::commitReadDecision(
    ConvRef                           c,
    const readstate::ReadDecision    &decision,
    const readstate::DispatchContext &context
) {
    if (c >= _store.conversationCount())
        return false;
    const std::string channel = _store.conversation(c).id;
    if (channel.empty())
        return false;

    if (!decision.accept) {
        if (decision.refresh)
            queueReadReconcile(channel);
        return false;
    }

    ReadIntent &in = _readIntent[channel];
    if (in.pending || context.readEpoch < in.epoch)
        return false;
    if (context.dispatchId < in.acceptedDispatchId ||
        (context.dispatchId == 0 && context.observationEpoch < in.acceptedObsEpoch))
        return false;
    if (in.invalidatedEpoch > 0) {
        const bool newerArrival = std::any_of(
            in.arrivals.begin(),
            in.arrivals.end(),
            [&](const readstate::Arrival &a) { return a.sequence > context.arrivalEpoch; }
        );
        if (context.arrivalEpoch < in.invalidatedEpoch || newerArrival) {
            queueReadReconcile(channel);
            return false;
        }
        in.invalidatedEpoch = 0;
    }

    in.acceptedObsEpoch   = ++_observationEpoch;
    in.acceptedDispatchId = context.dispatchId;
    // Retire arrivals covered by the accepted read cursor or absorbed by this baseline.
    in.arrivals.erase(
        std::remove_if(
            in.arrivals.begin(),
            in.arrivals.end(),
            [&](const readstate::Arrival &a) {
                return a.ts <= decision.lastRead ||
                       (a.sequence <= context.arrivalEpoch && a.ts <= decision.latest);
            }
        ),
        in.arrivals.end()
    );
    if (decision.refresh)
        queueReadReconcile(channel);
    return true;
}

int main(){
 SlackBackend b;model::Conversation c;
 c.lastRead=1794909941811299LL;c.latest=1794910537557059LL;c.unread=0;c.mentions=0;
 readstate::Observation info;info.source=readstate::Source::Reconcile;info.lastRead=c.lastRead;info.latest=c.latest;info.raw.unread.value=1;info.display.unread.value=1;
 auto d=b.prepareReadObservation(0,c,info,{1,0,0,0});
 std::printf("current merge: cleared live DM + actual info response -> unread=%u\n",d.unread);
 c.unread=d.unread;c.mentions=d.mentions;
 readstate::Observation counts;counts.source=readstate::Source::Counts;counts.lastRead=c.lastRead;counts.latest=1794909869919749LL;counts.hasUnreads=false;counts.raw.mentions.value=0;
 d=b.prepareReadObservation(0,c,counts,{2,0,0,0});
 std::printf("current merge: subsequent actual client.counts response -> unread=%u\n",d.unread);
 // One real incoming arrival is already reflected in current count.
 c.lastRead=100;c.latest=400;c.unread=1;c.mentions=1;
 b._readIntent[c.id].arrivals.push_back({400,1,1,1});
 readstate::Observation missing;missing.source=readstate::Source::Roster;missing.lastRead=100;missing.latest=300;
 d=b.prepareReadObservation(0,c,missing,{3,0,0,0});
 std::printf("current merge: one counted arrival + missing-count roster -> unread=%u mentions=%u (expected 1,1)\n",d.unread,d.mentions);

 c.lastRead=100;c.latest=400;c.unread=1;c.mentions=1;
 b._readIntent[c.id].arrivals={{400,1,1,1}}; b._store.record=c;
 counts.lastRead=100;counts.latest=300;counts.hasUnreads=false;
 readstate::DispatchContext context{4,0,1,0};
 d=b.prepareReadObservation(0,c,counts,context);
 std::printf("first zero response older than known arrival -> unread=%u\n",d.unread);
 bool accepted=b.commitReadDecision(0,d,context);
 std::printf("commit accepted=%d, retained arrival entries=%zu (expected 1)\n",accepted,b._readIntent[c.id].arrivals.size());
 c.lastRead=d.lastRead;c.latest=d.latest;c.unread=d.unread;c.mentions=d.mentions;b._store.record=c;
 d=b.prepareReadObservation(0,c,counts,{5,0,1,1});
 std::printf("second identical old zero response -> unread=%u (expected 1)\n",d.unread);
 return d.unread==1?0:1;

}
