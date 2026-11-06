#include "screens/shell/quick_switcher.h"

#include "base/str.h"
#include "base/utf8.h"
#include "screens/shell/sidebar.h"

#include <algorithm>

using model::ConvRef;

namespace shell {

namespace {

// Seconds of a message timestamp (epoch micros).
int64_t secs(model::Ts ts) {
    return ts / 1000000;
}

} // namespace

std::vector<ConvRef> quickSwitchOrder(
    const model::Store &store, const std::unordered_map<std::string, int64_t> &visited
) {
    struct Entry {
        ConvRef     c;
        int64_t     activity;
        std::string key; // the name, case-folded
    };
    std::vector<Entry> list;
    for (ConvRef c = 0; c < store.conversationCount(); ++c) {
        const model::Conversation &cv = store.conversation(c);
        if (!cv.member)
            continue;
        if (deadDm(store, cv)) // a DM whose peer is gone: nothing to jump to
            continue;
        const auto v = visited.find(cv.id);
        list.push_back(
            {c,
             std::max(
                 {v == visited.end() ? int64_t(0) : v->second, secs(cv.latest), secs(cv.lastRead)}
             ),
             utf8::foldCase(store.displayName(c))}
        );
    }
    std::sort(list.begin(), list.end(), [](const Entry &a, const Entry &b) {
        if (a.activity != b.activity)
            return a.activity > b.activity; // most recent first
        return a.key < b.key;
    });
    std::vector<ConvRef> out;
    out.reserve(list.size());
    for (const Entry &e : list)
        out.push_back(e.c);
    return out;
}

// Issue #60: fuzzy, not substring ("xdg" lands on #xd-general).
std::optional<double> quickSwitchScore(const QuickSwitchName &n, const std::vector<uint32_t> &q) {
    if (n.text.folded.empty())
        return std::nullopt;
    std::optional<double> s = fuzzyScore(q, n.text);
    if (!n.alt.folded.empty())
        if (const std::optional<double> a = fuzzyScore(q, n.alt); a && (!s || *a > *s))
            s = a;
    if (!s)
        return std::nullopt;
    // Group DMs are named after their members, so a person matches every
    // group they are in as well as their DM — and the groups, often more
    // recent, buried it (issue #61): half a consecutive match.
    return *s + (n.group ? -0.5 : 0.0);
}

std::vector<QuickSwitchName>
quickSwitchNames(const model::Store &store, const std::vector<ConvRef> &order) {
    std::vector<QuickSwitchName> out;
    out.reserve(order.size());
    for (ConvRef c : order) {
        const model::Conversation &cv = store.conversation(c);
        const std::string          nm = store.displayName(c);
        // A DM also by its peer's other names: the full or display name
        // that doesn't show (Settings → Names), and the handle.
        std::string                alt;
        if (cv.kind == model::ConvKind::Dm && cv.localName.empty() && cv.dmUser != model::kNoUser) {
            const model::User &u = store.user(cv.dmUser);
            for (const std::string *n : {&u.realName, &u.profileName, &u.name})
                if (!n->empty() && *n != nm)
                    alt = alt.empty() ? *n : str::concat({alt, " ", *n});
        }
        out.push_back(
            {FuzzyText(nm),
             cv.kind == model::ConvKind::Group,
             alt.empty() ? FuzzyText() : FuzzyText(alt)}
        );
    }
    return out;
}

namespace {

using Scored = std::vector<std::pair<double, ConvRef>>;

// What matches, with its score, in `order`.
Scored scoreAll(
    const std::vector<uint32_t>        &q,
    const std::vector<ConvRef>         &order,
    const std::vector<QuickSwitchName> &names
) {
    Scored scored;
    for (size_t i = 0; i < order.size() && i < names.size(); ++i)
        if (const std::optional<double> sc = quickSwitchScore(names[i], q))
            scored.emplace_back(*sc, order[i]);
    return scored;
}

// Best first; equal scores keep `order`.
std::vector<ConvRef> ranked(Scored scored) {
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });
    std::vector<ConvRef> out;
    out.reserve(scored.size());
    for (const auto &s : scored)
        out.push_back(s.second);
    return out;
}

} // namespace

std::vector<ConvRef> quickSwitchFilter(
    std::string_view                    query,
    const std::vector<ConvRef>         &order,
    const std::vector<QuickSwitchName> &names
) {
    return ranked(scoreAll(fuzzyQuery(str::trim(query)), order, names));
}

std::vector<ConvRef> quickSwitchFilter(
    const model::Store &store, std::string_view query, const std::vector<ConvRef> &order
) {
    return quickSwitchFilter(query, order, quickSwitchNames(store, order));
}

} // namespace shell
