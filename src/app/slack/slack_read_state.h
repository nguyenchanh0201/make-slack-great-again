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

} // namespace slack::readstate
