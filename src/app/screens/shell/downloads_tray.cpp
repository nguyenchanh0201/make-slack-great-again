#include "screens/shell/downloads_tray.h"

#include "app/model/downloads.h"
#include "screens/common/downloads.h"
#include "base/file.h"
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

constexpr float  kCardW    = 380;
constexpr size_t kMaxCards = 4; // the latest; older ones wait behind them

// A thin bar: the share received, or a sliding segment while the size is
// unknown.
class ProgressBar final : public ui::View {
public:
    explicit ProgressBar(float fraction) : _f(fraction) { style().height(6); }
    void paint(gfx::Painter &p) override {
        const ui::RectF b = bounds();
        p.fillRoundRect(b, 3, ui::color(C::OnDarkDim) & 0x55ffffffU);
        if (_f > 0)
            p.fillRoundRect(
                {b.x, b.y, std::max(6.f, b.w * std::min(1.f, _f)), b.h}, 3, 0xff1d9bd1U
            );
    }

private:
    float _f;
};

// The file's type as a coloured tile with its letter (Slack's): by extension.
class TypeTile final : public ui::View {
public:
    explicit TypeTile(std::string_view name) {
        style().size(40, 40).noShrink();
        std::string ext = str::asciiLower(file::extension(name));
        const auto  in  = [&](std::initializer_list<const char *> l) {
            for (const char *e : l)
                if (ext == e)
                    return true;
            return false;
        };
        _color  = in({"pdf"})                                         ? 0xffe44d4dU
                  : in({"png", "jpg", "jpeg", "gif", "webp", "heic"}) ? 0xff2bac76U
                  : in({"mp4", "mov", "webm", "mkv"})                 ? 0xff7b2d8bU
                  : in({"mp3", "m4a", "wav", "ogg"})                  ? 0xffe8912dU
                  : in({"zip", "gz", "tgz", "rar", "7z", "tar"})      ? 0xff616061U
                  : in({"doc", "docx"})                               ? 0xff2b579aU
                  : in({"xls", "xlsx", "csv"})                        ? 0xff217346U
                  : in({"ppt", "pptx", "key"})                        ? 0xffd24726U
                                                                      : 0xff1d9bd1U;
        _letter = ext.empty() ? "F" : str::asciiUpper(ext.substr(0, 1));
        auto *l =
            ui::styledLabel(this, _letter, ui::pxFont(17, text::Weight::Bold, 0xffffffffU), 1);
        l->setHitTransparent(true);
        style().items(Align::Center).justifyContent(ui::Justify::Center);
    }
    void paint(gfx::Painter &p) override {
        p.fillRoundRect(bounds(), 8, _color);
        View::paint(p);
    }

private:
    gfx::Color  _color = 0;
    std::string _letter;
};

// A text action on the card ("Open", "Show in Finder"), as Slack's
// "Pause"/"Cancel": plain text, a wash under the pointer.
ui::Clickable *textAction(ui::View *parent, std::string label) {
    auto *b = parent->add<ui::Clickable>();
    b->setLook({C::None, C::None, C::None, C::None, 4});
    b->setRole(ui::Role::Button);
    b->style().padding(0, 2, 0, 2).noShrink();
    auto *l = ui::styledLabel(
        b, std::move(label), ui::pxFont(14, text::Weight::Semibold, ui::themed(C::TooltipText)), 1
    );
    l->setHitTransparent(true);
    return b;
}

std::string sizeText(int64_t n) {
    const std::string s = str::byteSize(n, str::ByteSize::File);
    return s.empty() ? "0 B" : s;
}

} // namespace

DownloadsTray::DownloadsTray(screens::Context &ctx) : _ctx(ctx) {
    setModal(false);
    setCard(false); // each download is its own card
    // Bottom right: a far-off point below-right of the window, which the
    // placement clamps into its corner (and keeps there on a resize). The
    // padding is the gap to the window's edges.
    setAnchor({1e6f, 1e6f, 0, 0}, Place::Cursor);
    style().column().padding(0, 0, 12, 12).spacing(8).width(kCardW + 12);
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
        card->setBackground(C::TooltipBg, 10);
        card->style().row().padding(16, 14, 12, 14).spacing(14).items(Align::Start);
        card->add<TypeTile>(d.name);
        auto *col = card->add<ui::View>();
        col->style().column().spacing(6).flex(1);
        col->style().shrink = 1;
        // The name, and the × at its end.
        auto *top           = col->add<ui::View>();
        top->style().row().spacing(8).items(Align::Center);
        auto *name = ui::styledLabel(
            top, d.name, ui::pxFont(15, text::Weight::Bold, ui::themed(C::TooltipText)), 1
        );
        name->style().flex(1).shrink = 1;
        const int id                 = d.id;
        auto     *x                  = top->add<ui::IconButton>(gfx::Icon::X, tr("Dismiss"));
        x->style().size(24, 24).noShrink();
        x->setIconSize(14);
        // × on a running one stops it too.
        x->onClick            = [this, id] { screens::cancelDownload(_ctx.backend, id); };
        // What it is, and how far / how it ended.
        const text::Style dim = ui::pxFont(13, text::Weight::Regular, ui::themed(C::OnDarkDim));
        std::string       sub;
        const bool        moving = d.state == S::Running || d.state == S::Paused;
        if (moving) {
            sub = d.total > 0      ? arg(tr("%1 of %2"), sizeText(d.received), sizeText(d.total))
                  : d.received > 0 ? sizeText(d.received)
                  : d.state == S::Running ? std::string(tr("Downloading\xE2\x80\xA6"))
                                          : std::string();
            if (d.state == S::Paused)
                sub = sub.empty() ? std::string(tr("Paused"))
                                  : str::concat({tr("Paused"), " \xC2\xB7 ", sub});
        } else if (d.state == S::Done)
            sub = d.total > 0 ? sizeText(d.total) : std::string(tr("Downloaded"));
        else
            sub = arg(tr("Couldn't download: %1"), d.error);
        if (!d.kind.empty() && d.state != S::Failed)
            sub = str::concat({d.kind, " \xC2\xB7 ", sub});
        ui::styledLabel(col, sub, dim, 1);
        const auto actions = [&] {
            auto *a = col->add<ui::View>();
            a->style().row().spacing(18).margins(0, 4, 0, 0);
            return a;
        };
        if (moving) {
            col->add<ProgressBar>(d.total > 0 ? float(d.received) / float(d.total) : 0.f)
                ->style()
                .margins(0, 4, 0, 2);
            ui::View *row = actions();
            if (d.state == S::Running)
                textAction(row, tr("Pause"))->onClick = [this, id] {
                    screens::pauseDownload(_ctx.backend, id);
                };
            else
                textAction(row, tr("Resume"))->onClick = [this, id] {
                    screens::resumeDownload(_ctx.app.platform(), _ctx.backend, id);
                };
            textAction(row, tr("Cancel"))->onClick = [this, id] {
                screens::cancelDownload(_ctx.backend, id);
            };
            continue;
        }
        if (d.state == S::Failed) {
            if (!d.source.empty())
                textAction(actions(), tr("Retry"))->onClick = [this, id] {
                    screens::resumeDownload(_ctx.app.platform(), _ctx.backend, id);
                };
            continue;
        }
        ui::View         *done                = actions();
        const std::string path                = d.path;
        textAction(done, tr("Open"))->onClick = [this, path] {
            _ctx.app.platform().openUrl(file::toFileUrl(path));
        };
#ifdef __APPLE__
        const char *reveal = N_("Show in Finder");
#else
        const char *reveal = N_("Show in folder");
#endif
        textAction(done, tr(reveal))->onClick = [this, path] {
            _ctx.app.platform().revealFile(path);
        };
    }
    invalidateLayout();
    update();
}

} // namespace shell
