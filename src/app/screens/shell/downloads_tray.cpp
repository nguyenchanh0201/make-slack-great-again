#include "screens/shell/downloads_tray.h"

#include "app/model/downloads.h"
#include "base/i18n.h"
#include "base/str.h"
#include "gfx/icons_generated.h"
#include "plat/plat.h"
#include "ui/controls.h"

#include <algorithm>
#include <cmath>

using i18n::arg;
using i18n::tr;
using ui::Align;
using ui::C;

namespace shell {

namespace {

constexpr float  kCardW    = 340;
constexpr size_t kMaxCards = 4; // the latest; older ones wait behind them

// A thin bar: the share received, or a sliding segment while the size is
// unknown.
class ProgressBar final : public ui::View {
public:
    explicit ProgressBar(float fraction) : _f(fraction) { style().height(4).flex(0); }
    void paint(gfx::Painter &p) override {
        const ui::RectF b = bounds();
        p.fillRoundRect(b, 2, ui::color(C::FormHighlightStrong));
        if (_f > 0)
            p.fillRoundRect(
                {b.x, b.y, std::max(4.f, b.w * std::min(1.f, _f)), b.h}, 2, ui::color(C::Accent)
            );
    }

private:
    float _f;
};

std::string sizeText(int64_t n) {
    const std::string s = str::byteSize(n, str::ByteSize::File);
    return s.empty() ? "0 B" : s;
}

} // namespace

DownloadsTray::DownloadsTray(screens::Context &ctx) : _ctx(ctx) {
    setModal(false);
    // Bottom right: a far-off point below-right of the window, which the
    // placement clamps into its corner (and keeps there on a resize).
    setAnchor({1e6f, 1e6f, 0, 0}, Place::Cursor);
    style().column().padding(12, 12, 12, 12).spacing(10).width(kCardW);
    rebuild();
}

void DownloadsTray::rebuild() {
    clearChildren();
    const auto  &all  = model::downloads().items();
    const size_t from = all.size() > kMaxCards ? all.size() - kMaxCards : 0;
    for (size_t i = all.size(); i-- > from;) { // the latest on top
        const model::Downloads::Item &d = all[i];
        using S                         = model::Downloads::State;
        auto *card                      = add<ui::View>();
        card->style().column().spacing(6);
        auto *top = card->add<ui::View>();
        top->style().row().spacing(8).items(Align::Center);
        auto *mark = top->add<ui::IconView>(
            d.state == S::Done     ? gfx::Icon::Check
            : d.state == S::Failed ? gfx::Icon::X
                                   : gfx::Icon::Download,
            16,
            d.state == S::Failed ? C::Danger : C::TextMuted
        );
        mark->style().noShrink();
        auto *name = ui::styledLabel(
            top, d.name, ui::pxFont(14, text::Weight::Bold, ui::themed(C::Text)), 1
        );
        name->style().flex(1).shrink = 1;
        const int id                 = d.id;
        auto     *x                  = top->add<ui::IconButton>(gfx::Icon::X, tr("Dismiss"));
        x->style().size(24, 24).noShrink();
        x->setIconSize(12);
        x->onClick = [id] { model::downloads().remove(id); };
        if (d.state == S::Running) {
            const float f = d.total > 0 ? float(d.received) / float(d.total) : 0.f;
            card->add<ProgressBar>(f);
            ui::styledLabel(
                card,
                d.total > 0      ? arg(tr("%1 of %2"), sizeText(d.received), sizeText(d.total))
                : d.received > 0 ? sizeText(d.received)
                                 : std::string(tr("Downloading\xE2\x80\xA6")),
                ui::pxFont(12, text::Weight::Regular, ui::themed(C::TextMuted)),
                1
            );
            continue;
        }
        auto *bottom = card->add<ui::View>();
        bottom->style().row().spacing(8).items(Align::Center);
        auto *status = ui::styledLabel(
            bottom,
            d.state == S::Done ? (d.total > 0 ? sizeText(d.total) : std::string(tr("Downloaded")))
                               : arg(tr("Couldn't download: %1"), d.error),
            ui::pxFont(12, text::Weight::Regular, ui::themed(C::TextMuted)),
            1
        );
        status->style().flex(1).shrink = 1;
        if (d.state == S::Done) {
#ifdef __APPLE__
            const char *label = N_("Show in Finder");
#else
            const char *label = N_("Show in folder");
#endif
            auto *show = bottom->add<ui::Button>(tr(label), ui::Button::Kind::Secondary);
            show->style().noShrink();
            const std::string path = d.path;
            show->onClick          = [this, path] { _ctx.app.platform().revealFile(path); };
        }
    }
    invalidateLayout();
    update();
}

} // namespace shell
