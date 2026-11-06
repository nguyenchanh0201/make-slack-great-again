// The conversations the unified search jumps to: their order, names and
// fuzzy filter, and a workspace's tab.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "screens/shell/fuzzy_match.h"
#include "ui/ui.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shell {

// Every conversation you are a member of except
// DMs whose peer is deactivated or still unresolved (a raw user id), most
// recent first by max(visited here, latest message, last read), then by
// name. `visited`: the sidebar's visit stamps (conv id → epoch secs).
std::vector<model::ConvRef> quickSwitchOrder(
    const model::Store &store, const std::unordered_map<std::string, int64_t> &visited
);

// Conversations whose name (a DM's: also its peer's other name and
// handle) fuzzy-matches `query` (fuzzy_match.h), best
// first, group DMs a little behind; ties keep `order`. Empty query = order
// (minus the ones without a name yet).
std::vector<model::ConvRef> quickSwitchFilter(
    const model::Store &store, std::string_view query, const std::vector<model::ConvRef> &order
);

// What quickSwitchFilter scores, prepared once per Store change rather than
// per keystroke: each name of `order`, folded (empty: no name yet).
struct QuickSwitchName {
    FuzzyText text;
    bool      group = false; // a group DM, ranked a little behind
    FuzzyText alt;           // a DM peer's other names and handle (empty: none)
};
std::vector<QuickSwitchName>
quickSwitchNames(const model::Store &store, const std::vector<model::ConvRef> &order);
// One name's score for a folded query (fuzzyQuery): group DMs half a match
// behind; none when it doesn't match or has no name yet.
std::optional<double> quickSwitchScore(const QuickSwitchName &n, const std::vector<uint32_t> &q);
// quickSwitchFilter over prepared names (parallel to `order`): the same result.
std::vector<model::ConvRef> quickSwitchFilter(
    std::string_view                    query,
    const std::vector<model::ConvRef>  &order,
    const std::vector<QuickSwitchName> &names
);

// One workspace's tab.
struct QuickSwitchTab {
    std::string                  key, name, icon; // icon: a local picture ("" = the letter)
    const model::Store          *store = nullptr;
    std::vector<model::ConvRef>  order; // quickSwitchOrder
    // quickSwitchNames(order), and the store's metaRevision they were made at.
    std::vector<QuickSwitchName> names;
    uint64_t                     namesRev = UINT64_MAX;
    model::ConvRef               open     = model::kNoConv; // open there: not a Recent place
};

} // namespace shell
