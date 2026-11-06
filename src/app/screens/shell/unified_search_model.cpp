#include "screens/shell/unified_search_model.h"

#include "base/str.h"
#include "base/utf8.h"

#include <algorithm>
#include <unordered_map>

namespace shell {

std::vector<RecentPlace>
recentPlaces(const model::Store &, const std::vector<model::ConvRef> &order, size_t max) {
    std::vector<RecentPlace> out;
    for (Sidebar::Nav p :
         {Sidebar::Nav::Threads,
          Sidebar::Nav::Saved,
          Sidebar::Nav::Activity,
          Sidebar::Nav::Scheduled})
        if (out.size() < max)
            out.push_back({model::kNoConv, p});
    for (model::ConvRef c : order)
        if (out.size() < max)
            out.push_back({c, Sidebar::Nav::None});
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
        out.push_back({s.user, d == dms.end() ? model::kNoConv : d->second});
    }
    return out;
}

std::vector<model::ConvRef> searchChannels(
    const model::Store                 &store,
    const std::vector<model::ConvRef>  &order,
    const std::vector<QuickSwitchName> &names,
    std::string_view                    query,
    size_t                              max
) {
    std::vector<model::ConvRef> out;
    if (str::trim(query).empty())
        return out;
    for (model::ConvRef c : quickSwitchFilter(query, order, names)) {
        if (store.conversation(c).isDirect())
            continue;
        out.push_back(c);
        if (out.size() == max)
            break;
    }
    return out;
}

} // namespace shell
