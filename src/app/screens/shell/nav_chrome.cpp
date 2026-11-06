#include "screens/shell/nav_chrome.h"

#include "gfx/icons_generated.h"

#include <algorithm>

using namespace ui;

namespace shell {

namespace {

Color scaled(Color c, float f) {
    const auto ch = [&](int s) {
        return uint32_t(std::clamp(float((c >> s) & 0xff) * f, 0.f, 255.f)) << s;
    };
    return (c & 0xff000000u) | ch(16) | ch(8) | ch(0);
}

Color mix(Color a, Color b, float amount) {
    Color out = 0xff000000;
    for (int shift : {0, 8, 16})
        out |=
            uint32_t(
                float((a >> shift) & 255) * (1 - amount) + float((b >> shift) & 255) * amount + 0.5f
            )
            << shift;
    return out;
}

} // namespace

void paintNavGradient(const View &v, gfx::Painter &p, C token) {
    const Color base = color(token);
    const bool  flat =
        palette(app() && app()->dark()) == Palette::Custom && !customPalette().gradient;
    if (flat || !v.window()) {
        p.fillRect(v.bounds(), base);
        return;
    }
    const auto nav = paletteColors(palette(app() && app()->dark()), app() && app()->dark());
    const int  r = int(nav.rail >> 16 & 255), g = int(nav.rail >> 8 & 255), b = int(nav.rail & 255);
    if (palette(app() && app()->dark()) == Palette::Custom &&
        std::max({r, g, b}) + std::min({r, g, b}) > 255) {
        const float w      = std::max(1.f, v.window()->size().w),
                    h      = std::max(1.f, v.window()->size().h);
        const auto  origin = v.mapToWindow({0, 0});
        Color       end    = nav.gradientEnd;
        if (token == C::Sidebar)
            end = mix(end, 0xffffffff, app()->dark() ? 0.03f : 0.90f);
        // Screenshot-tuned approximation: opposite corners share a hue, in window coordinates.
        uint32_t corners[] = {base, end, end, base};
        p.save();
        p.clipRect(v.bounds());
        // Expand by half a window to align the two texel centres with the window's edges.
        p.drawBitmap(
            {corners, 2, 2, 2},
            {-origin.x - w / 2, -origin.y - h / 2, w * 2, h * 2},
            gfx::Sampling::Bilinear
        );
        p.restore();
        return;
    }
    // Local coordinates of the window's top and bottom edges.
    const float top = -v.mapToWindow({0, 0}).y, h = std::max(1.f, v.window()->size().h);
    p.fillRectGradient(
        v.bounds(), {0, top}, scaled(base, 1.12f), {0, top + h}, scaled(base, 0.90f)
    );
}

GhostButton::GhostButton(gfx::Icon icon, std::string tooltip, bool plus)
    : _icon(uint16_t(icon)), _plus(plus) {
    style().size(40, 40).noShrink();
    setLook({C::None, C::None, C::None, C::None, 10});
    setTooltip(std::move(tooltip));
    setRole(Role::Button);
    setHoverRepaint(true);
}

void GhostButton::setIcon(gfx::Icon icon) {
    _icon = uint16_t(icon);
    update();
}

void GhostButton::paint(gfx::Painter &p) {
    paintChrome(p);
    paintGlyph(p, gfx::Icon(_icon));
}

void GhostButton::paintChrome(gfx::Painter &p) {
    const bool  hov = hovered();
    const RectF r{0.75f, 0.75f, width() - 1.5f, height() - 1.5f};
    const Color ink = color(C::SidebarText);
    p.fillRoundRect(r, 10, gfx::withAlpha(ink, hov ? 0.16f : 0.08f));
    p.strokeRoundRect(r, 10, 1.5f, gfx::withAlpha(ink, hov ? 0.35f : 0.18f));
}

void GhostButton::paintGlyph(gfx::Painter &p, gfx::Icon icon) {
    const Color ink = color(C::SidebarText);
    if (_plus) { // two opaque strokes: no alpha seam where they cross
        const float cx = width() / 2, cy = height() / 2, arm = 5;
        p.drawLine({cx - arm, cy}, {cx + arm, cy}, 2.5f, ink);
        p.drawLine({cx, cy - arm}, {cx, cy + arm}, 2.5f, ink);
        return;
    }
    const float d = width() * 0.52f;
    gfx::drawIcon(p, icon, {snapPx((width() - d) / 2), snapPx((height() - d) / 2), d, d}, ink);
}

void GhostButton::paintGlyphTurned(gfx::Painter &p, gfx::Icon icon, float degrees) {
    const float d = width() * 0.52f;
    gfx::drawIconRotated(
        p, icon, {(width() - d) / 2, (height() - d) / 2, d, d}, color(C::SidebarText), degrees
    );
}

GlyphButton::GlyphButton(gfx::Icon icon, float box, float iconPx, C tint, std::string tooltip)
    : _icon(uint16_t(icon)), _px(iconPx), _tint(tint) {
    style().size(box, box).noShrink();
    setLook({C::None, C::None, C::None, C::None, 0});
    setTooltip(std::move(tooltip));
    setRole(Role::Button);
}

void GlyphButton::setIcon(gfx::Icon icon) {
    _icon = uint16_t(icon);
    update();
}

void GlyphButton::setTint(C tint) {
    _tint = tint;
    update();
}

void GlyphButton::paint(gfx::Painter &p) {
    Clickable::paint(p);
    gfx::drawIcon(
        p,
        gfx::Icon(_icon),
        {snapPx((width() - _px) / 2), snapPx((height() - _px) / 2), _px, _px},
        color(enabled() ? _tint : C::DropArrow)
    );
}

} // namespace shell
