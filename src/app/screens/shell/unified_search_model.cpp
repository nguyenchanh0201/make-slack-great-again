#include "screens/shell/unified_search_model.h"

#include "base/str.h"
#include "base/utf8.h"

#include <algorithm>
#include <iterator>
#include <unordered_map>

namespace shell {

std::vector<RecentPlace>
recentPlaces(const std::vector<model::ConvRef> &order, model::ConvRef open, size_t max) {
    constexpr Sidebar::Nav pages[] = {
        Sidebar::Nav::Threads, Sidebar::Nav::Saved, Sidebar::Nav::Activity, Sidebar::Nav::Scheduled
    };
    const size_t             convs = max > std::size(pages) ? max - std::size(pages) : 0;
    std::vector<RecentPlace> out;
    for (model::ConvRef c : order)
        if (c != open && out.size() < convs)
            out.push_back({c, Sidebar::Nav::None});
    for (Sidebar::Nav p : pages)
        if (out.size() < max)
            out.push_back({model::kNoConv, p});
    return out;
}

std::vector<PersonHit> searchPeople(const model::Store &store, std::string_view query, size_t max) {
    const std::string_view trimmed = str::trim(query);
    if (trimmed.empty())
        return {};
    const std::vector<uint32_t> q = fuzzyQuery(trimmed);
    struct Scored {
        double         score;
        model::UserRef user;
        std::string    key; // display name, case-folded, for ties
    };
    std::vector<Scored> found;
    for (model::UserRef u = 0; u < store.userCount(); ++u) {
        const model::User &us = store.user(u);
        if (u == store.me || us.bot || us.deleted || us.stranger || us.placeholder)
            continue;
        // Best of the three names, as a DM's alternative names are in quickSwitchNames.
        std::optional<double> best;
        for (const std::string *n : {&us.displayName, &us.realName, &us.name})
            if (!n->empty())
                if (const auto s = fuzzyScore(q, FuzzyText(*n)); s && (!best || *s > *best))
                    best = s;
        if (best)
            found.push_back({*best, u, utf8::foldCase(std::string(us.label()))});
    }
    std::sort(found.begin(), found.end(), [](const Scored &a, const Scored &b) {
        return a.score != b.score ? a.score > b.score : a.key < b.key;
    });
    if (found.size() > max)
        found.resize(max);
    // The existing 1:1 DM of each hit.
    std::unordered_map<model::UserRef, model::ConvRef> dms;
    for (model::ConvRef c = 0; c < store.conversationCount(); ++c) {
        const model::Conversation &cv = store.conversation(c);
        if (cv.kind == model::ConvKind::Dm && cv.dmUser != model::kNoUser)
            dms.emplace(cv.dmUser, c);
    }
    std::vector<PersonHit> out;
    for (const Scored &s : found) {
        const auto d = dms.find(s.user);
        out.push_back({s.user, d == dms.end() ? model::kNoConv : d->second, s.score});
    }
    return out;
}

std::vector<std::pair<double, model::ConvRef>> searchChannels(
    const model::Store                 &store,
    const std::vector<model::ConvRef>  &order,
    const std::vector<QuickSwitchName> &names,
    std::string_view                    query,
    size_t                              max
) {
    std::vector<std::pair<double, model::ConvRef>> out;
    const std::string_view                         trimmed = str::trim(query);
    if (trimmed.empty())
        return out;
    // quickSwitchFilter's ranking, keeping the scores: best first, ties in `order`.
    const std::vector<uint32_t> q = fuzzyQuery(trimmed);
    for (size_t i = 0; i < order.size() && i < names.size(); ++i)
        if (!store.conversation(order[i]).isDirect())
            if (const std::optional<double> s = quickSwitchScore(names[i], q))
                out.emplace_back(*s, order[i]);
    std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });
    if (out.size() > max)
        out.resize(max);
    return out;
}

} // namespace shell
