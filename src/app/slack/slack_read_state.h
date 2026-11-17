// Slack-private read-state observation types and dispatch context.
// Kept out of public model/Store headers so transient Slack response metadata
// does not leak into persistence or Backend APIs.
#pragma once

#include "app/model/types.h"

#include <cstdint>
#include <optional>

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
