// A message search hit's two lines, as the unified search shows them.
#pragma once

#include "app/model/store.h"

#include <string>
#include <string_view>

namespace shell {

// A result's two lines: "#channel" (a DM's name bare, "Unknown channel"
// without one), and the message text, mentions named, its first 120
// characters on one line.
std::string searchConvLabel(const model::Store &store, model::ConvRef conv);
std::string searchPreview(const model::Store &store, std::string_view mrkdwn);

} // namespace shell
