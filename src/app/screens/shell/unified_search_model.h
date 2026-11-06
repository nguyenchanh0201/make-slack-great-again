// The unified search bar's matches over people, channels and recent places.
#pragma once

#include "screens/shell/quick_switcher.h"
#include "screens/shell/sidebar.h"

#include <string_view>
#include <vector>

namespace shell {

struct PersonHit {
    model::UserRef user;
    model::ConvRef dm = model::kNoConv; // an existing DM with them, else kNoConv
};

// A conversation, or (conv == kNoConv) one of the sidebar's pages.
struct RecentPlace {
    model::ConvRef conv = model::kNoConv;
    Sidebar::Nav   page = Sidebar::Nav::None;
};

// Threads, Saved, Activity, Scheduled, then `order`'s conversations; <= max.
std::vector<RecentPlace>
recentPlaces(const model::Store &store, const std::vector<model::ConvRef> &order, size_t max = 8);

// Active humans (not me) whose display name, real name or handle fuzzy-matches
// `query`, best first. Empty query: none.
std::vector<PersonHit>
searchPeople(const model::Store &store, std::string_view query, size_t max = 5);

// quickSwitchFilter over `order`/`names` minus DMs and group DMs; <= max.
std::vector<model::ConvRef> searchChannels(
    const model::Store                 &store,
    const std::vector<model::ConvRef>  &order,
    const std::vector<QuickSwitchName> &names,
    std::string_view                    query,
    size_t                              max = 5
);

} // namespace shell
