// The unified search bar's matches over people, channels and recent places.
#pragma once

#include "screens/shell/quick_switcher.h"
#include "screens/shell/sidebar.h"

#include <string_view>
#include <utility>
#include <vector>

namespace shell {

struct PersonHit {
    model::UserRef user;
    model::ConvRef dm    = model::kNoConv; // an existing DM with them, else kNoConv
    double         score = 0;              // the fuzzy match's, comparable with a channel's
};

// A conversation, or (conv == kNoConv) one of the sidebar's pages.
struct RecentPlace {
    model::ConvRef conv = model::kNoConv;
    Sidebar::Nav   page = Sidebar::Nav::None;
};

// `order`'s conversations but `open`, then Threads, Saved, Activity,
// Scheduled; <= max, the pages always in.
std::vector<RecentPlace> recentPlaces(
    const std::vector<model::ConvRef> &order, model::ConvRef open = model::kNoConv, size_t max = 12
);

// Active humans (not me) whose display name, real name or handle fuzzy-matches
// `query`, best first. Empty query: none.
std::vector<PersonHit>
searchPeople(const model::Store &store, std::string_view query, size_t max = 5);

// quickSwitchFilter over `order`/`names` minus DMs and group DMs, with each
// one's score; <= max.
std::vector<std::pair<double, model::ConvRef>> searchChannels(
    const model::Store                 &store,
    const std::vector<model::ConvRef>  &order,
    const std::vector<QuickSwitchName> &names,
    std::string_view                    query,
    size_t                              max = 5
);

} // namespace shell
