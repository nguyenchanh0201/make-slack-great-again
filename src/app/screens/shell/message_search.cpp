#include "screens/shell/message_search.h"

#include "app/screens/common/message_text.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/utf8.h"

#include <algorithm>

using i18n::tr;
using model::ConvRef;

namespace shell {

namespace {

constexpr size_t kPreviewChars = 120;

} // namespace

std::string searchConvLabel(const model::Store &store, ConvRef conv) {
    const std::string name = conv < store.conversationCount() ? store.displayName(conv) : "";
    if (name.empty())
        return tr("Unknown channel");
    return store.conversation(conv).isDirect() ? name : str::concat({"#", name});
}

std::string searchPreview(const model::Store &store, std::string_view text) {
    // The rendered text with mentions resolved (the
    // parser's label is kept for anyone unknown).
    std::string out = screens::plainText(store, text);
    // The first 120 characters, newlines as spaces.
    out.resize(utf8::prefixBytes(out, kPreviewChars));
    std::replace(out.begin(), out.end(), '\n', ' ');
    return out;
}

} // namespace shell
