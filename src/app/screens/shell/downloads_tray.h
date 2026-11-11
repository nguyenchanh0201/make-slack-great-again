// The downloads tray: a card per download in model::downloads(), stacked at
// the window's bottom right — the name, a progress bar and "2.1 MB of 9 MB"
// while it runs, then "Show in Finder" (plat::App::revealFile), or why it
// failed; × dismisses a card. Non-modal: the window stays usable under it.
// The shell keeps one per window while the list has something to show, and
// rebuilds it after each change (Shell::syncDownloads).
#pragma once

#include "screens/common/context.h"
#include "ui/widgets.h"

namespace shell {

class DownloadsTray final : public ui::Popup {
public:
    explicit DownloadsTray(screens::Context &ctx);
    // The cards again from the list.
    void rebuild();

private:
    screens::Context &_ctx;
};

} // namespace shell
