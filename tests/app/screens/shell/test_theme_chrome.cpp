// Rendered regressions for pale custom themes: no dark-theme-only white controls.
#include "app/fake/fake_backend.h"
#include "app/screens/messages/image_cache.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "plat/testing.h"
#include "screens/shell/nav_chrome.h"
#include "screens/shell/shell.h"
#include "support/test.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {
ui::App &app() {
    static auto a = ui::App::create();
    return *a;
}

void pump() {
    for (int i = 0; i < 20; ++i)
        app().pump(2);
}

struct Theme {
    ui::CustomPalette old   = ui::customPalette();
    ui::Palette       light = ui::palette(false), dark = ui::palette(true);
    Theme() {
        ui::CustomPalette t;
        t.primary         = 0xfff4daff;
        t.highlight1      = 0xff9e0b2e;
        t.highlight2      = 0xff83388a;
        t.important       = 0xffc01343;
        t.brightness      = 6;
        t.sidebarInverted = true;
        ui::setCustomPalette(t);
        ui::setPalette(false, ui::Palette::Custom);
        ui::setPalette(true, ui::Palette::Custom);
        app().setThemeMode(ui::ThemeMode::Light);
        app().restyle();
    }
    ~Theme() {
        ui::setCustomPalette(old);
        ui::setPalette(false, light);
        ui::setPalette(true, dark);
        app().setThemeMode(ui::ThemeMode::System);
        app().restyle();
    }
};

int channel(gfx::Color c, int shift) {
    return int(c >> shift & 255);
}

gfx::Color pixel(ui::Window &w, float x, float y) {
    uint32_t p = 0;
    CHECK(
        app().platform().testHooks()->readPixel(
            w.native(), int(x * w.scale()), int(y * w.scale()), &p
        )
    );
    return p;
}

class Nav : public ui::View {
public:
    void paint(gfx::Painter &p) override { shell::paintNavGradient(*this, p, ui::C::Rail); }
};

ui::View *searchLabel(ui::View &v) {
    if (v.accessibleName().starts_with("Search "))
        return &v;
    for (size_t i = 0; i < v.childCount(); ++i)
        if (auto *found = searchLabel(*v.child(i)))
            return found;
    return nullptr;
}
} // namespace

TEST(
    "theme chrome: a pale custom sidebar remains nearly white instead of darkening down the window"
) {
    Theme      theme;
    const auto c = ui::paletteColors(ui::Palette::Custom, false);
    CHECK(channel(c.sidebar, 16) >= 245);
    CHECK(channel(c.sidebar, 8) >= 245);
    CHECK(channel(c.sidebar, 0) >= 245);
    CHECK(c.pill == 0xff9e0b2e);
    CHECK(c.online == 0xff83388a);
    CHECK(c.badge == 0xffc01343);
}

TEST(
    "theme chrome: the outer gradient picks up the selection hue without washing pale colors to "
    "white"
) {
    Theme      theme;
    ui::Window w({.size = {400, 300}});
    w.root().add<Nav>();
    pump();
    const auto a = pixel(w, 20, 20), b = pixel(w, 20, 280), c = pixel(w, 380, 20);
    CHECK(channel(b, 16) > channel(b, 0)); // pink, not a darkened lilac
    CHECK(channel(c, 16) > channel(c, 0));
    CHECK(channel(a, 8) < 245); // preserve the lilac origin rather than clipping to white
    CHECK(channel(b, 16) > 230);
    CHECK(channel(b, 8) < 200);
}

TEST("theme chrome: ghost buttons use dark ink on pale navigation backgrounds") {
    Theme      theme;
    ui::Window w({.size = {100, 100}});
    auto      *nav = w.root().add<Nav>();
    nav->style().row().items(ui::Align::Start);
    nav->add<shell::GhostButton>(gfx::Icon::Plus, "Add", true);
    pump();
    const auto ink = pixel(w, 20, 20);
    CHECK(channel(ink, 16) < 120);
    CHECK(channel(ink, 8) < 120);
    CHECK(channel(ink, 0) < 120);
}

TEST("theme chrome: pinning white title-bar text does not wash out pale-rail controls") {
    Theme theme;
    auto  t        = ui::customPalette();
    t.titleBarBg   = 0xff1a1020;
    t.titleBarText = 0xffffffff;
    ui::setCustomPalette(t);
    ui::Window w({.size = {100, 100}});
    auto      *nav = w.root().add<Nav>();
    nav->style().row().items(ui::Align::Start);
    nav->add<shell::GhostButton>(gfx::Icon::Plus, "Add", true);
    pump();
    const auto ink = pixel(w, 20, 20);
    CHECK(std::max({channel(ink, 16), channel(ink, 8), channel(ink, 0)}) < 120);
}

TEST("theme chrome: the workspace search label is readable on a pale title bar") {
    Theme             theme;
    model::Store      store;
    fake::FakeBackend backend(store, app().platform());
    backend.setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
    bool loaded = false;
    backend.connect([&](bool ok, const std::string &) { loaded = ok; });
    for (int i = 0; i < 200 && !loaded; ++i)
        app().pump(5);
    REQUIRE(loaded);
    screens::ImageCache images(app().platform());
    screens::Context    ctx{app(), store, backend, images, {}, {}, {}, {}, {}};
    shell::Settings     settings;
    ui::Window          w({.size = {1200, 800}, .decorations = plat::Decorations::Custom});
    shell::Shell        sh(ctx, w, settings, "");
    sh.setWorkspaces({}, "");
    pump();
    auto *label = searchLabel(w.root());
    REQUIRE(label != nullptr);
    const auto r = label->windowRect();
    if (const char *path = std::getenv("MSGA_THEME_CAPTURE")) {
        FILE *out = std::fopen(path, "wb");
        REQUIRE(out != nullptr);
        std::fprintf(out, "P6\n1200 800\n255\n");
        for (int y = 0; y < 800; ++y)
            for (int x = 0; x < 1200; ++x) {
                const auto          c     = pixel(w, float(x), float(y));
                const unsigned char rgb[] = {
                    static_cast<unsigned char>(c >> 16),
                    static_cast<unsigned char>(c >> 8),
                    static_cast<unsigned char>(c)
                };
                std::fwrite(rgb, 1, 3, out);
            }
        std::fclose(out);
    }
    int darkPixels = 0;
    for (int y = int(r.y); y < int(r.bottom()); ++y)
        for (int x = int(r.x); x < int(r.right()); ++x) {
            const auto p = pixel(w, float(x), float(y));
            darkPixels += std::max({channel(p, 16), channel(p, 8), channel(p, 0)}) < 120;
        }
    CHECK(darkPixels >= 20);
}
