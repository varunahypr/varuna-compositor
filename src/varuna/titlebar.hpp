#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

#include <cairo/cairo.h>
#include <linux/input-event-codes.h>

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/XWaylandManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#define private public
#include <hyprland/src/protocols/XDGDecoration.hpp>
#undef private

#include "requests.hpp"

namespace titlebar {

using namespace Render;

enum eFinish : uint8_t {
    FLAT,
    EDGE_SHADED,
    GLASS,
};

struct SButtonLook {
    std::array<CHyprColor, 3> fill;
    CHyprColor                symbol;
    eFinish                   finish = FLAT;
    bool                      ring   = false;
    bool                      hollow = false;

    bool operator==(const SButtonLook&) const = default;
};

struct SHeader {
    CHyprColor active{0.18, 0.18, 0.196, 1}, backdrop{0.133, 0.133, 0.149, 1}, fg{1, 1, 1, 1};
    float      shade = 0.36f;

    bool operator==(const SHeader&) const = default;
};

struct SLook {
    bool        on     = true;
    bool        right  = false;
    SHeader     header;
    float       fontPt = 11.f;
    float       iconPx = 8.f;
    SButtonLook rest, lit;

    bool operator==(const SLook&) const = default;
};
static SLook g_look;

enum eLight : uint8_t {
    CLOSE,
    MINIMIZE,
    ZOOM,
};

static constexpr double HEADER = 46.0, SHADE = 1.0, HEIGHT = HEADER + SHADE;
static constexpr double LIGHT = 14.0, LIGHT_Y = 22.5, FIRST = 19.0, STEP = 22.0;
static constexpr double LIGHTS_ROOM = FIRST + 2 * STEP + LIGHT;
static constexpr double PT_TO_PX = 96.0 / 72.0;
static constexpr int    TITLE_WEIGHT = 700;
static constexpr float  BACKDROP_OPACITY = 0.5f;
static constexpr auto   DOUBLE_TIME = std::chrono::milliseconds(400);
static constexpr double DOUBLE_NEAR = 6.0;
static constexpr uint32_t BORDER_PRIORITY = 10000;
static constexpr double SHADE_R = 0, SHADE_G = 0, SHADE_B = 6.0 / 255.0;

class CTitleBar;
class CSquareTopCorners;
static std::vector<CTitleBar*>         g_bars;
static std::vector<CSquareTopCorners*> g_corners;
static bool                    g_dragStartedOnBar = false;

static bool drawsNoTitleBar(const PHLWINDOW& w) {
    if (!w)
        return false;
    if (w->m_isX11) {
        const auto x = w->m_xwaylandSurface.lock();
        return x && !x->m_overrideRedirect && !w->m_X11DoesntWantBorders && !w->m_X11ShouldntFocus;
    }
    const auto surface  = w->m_xdgSurface.lock();
    const auto toplevel = surface ? surface->m_toplevel.lock() : nullptr;
    if (!toplevel || !toplevel->m_resource || !PROTO::xdgDecoration)
        return false;
    const auto& decorations = PROTO::xdgDecoration->m_decorations;
    const auto  it          = decorations.find(toplevel->m_resource->resource());
    return it != decorations.end() && it->second && it->second->mostRecentlyRequested != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
}

static void circle(cairo_t* cr, double cx, double cy, double r) {
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
}

static void paintRadial(cairo_t* cr, double cx, double cy, double inner, double outer, double r, double g, double b, double a0, double a1) {
    const auto pattern = cairo_pattern_create_radial(cx, cy, inner, cx, cy, outer);
    cairo_pattern_add_color_stop_rgba(pattern, 0, r, g, b, a0);
    cairo_pattern_add_color_stop_rgba(pattern, 1, r, g, b, a1);
    cairo_set_source(cr, pattern);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(pattern);
}

static void dropShadow(cairo_t* cr, double cx, double cy, double r) {
    for (const auto& [grow, alpha] : {std::pair{1.2, 0.05}, {0.8, 0.08}, {0.4, 0.12}}) {
        circle(cr, cx, cy + 0.5, r + grow);
        cairo_set_source_rgba(cr, 0, 0, 0, alpha);
        cairo_fill(cr);
    }
}

static void glassFinish(cairo_t* cr, double cx, double cy, double r, eFinish finish) {
    const double size = 2 * r, cornerReach = r * M_SQRT2;
    circle(cr, cx, cy, r);
    if (finish == GLASS) {
        const double hx = cx, hy = cy - r + 0.3 * size, highlightReach = size * std::sqrt(0.25 + 0.49);
        paintRadial(cr, hx, hy, 0, 0.5 * highlightReach, 1, 1, 1, 0.5, 0);
    }
    const double edge = finish == GLASS ? 0.22 : 0.12;
    paintRadial(cr, cx, cy, 0.58 * cornerReach, cornerReach, 0, 0, 0, 0, edge);
    cairo_new_path(cr);
}

static void symbolPath(cairo_t* cr, eLight which) {
    switch (which) {
        case CLOSE:
            cairo_move_to(cr, 4.6, 4.6);
            cairo_line_to(cr, 11.4, 11.4);
            cairo_move_to(cr, 11.4, 4.6);
            cairo_line_to(cr, 4.6, 11.4);
            break;
        case MINIMIZE: cairo_rectangle(cr, 4, 10.0078, 8, 1.9883); break;
        case ZOOM:
            cairo_rectangle(cr, 3.9883, 3.9922, 8.0117, 8.0117);
            cairo_rectangle(cr, 5.9883, 5.9922, 4.0117, 4.0117);
            break;
    }
}

static void drawSymbol(cairo_t* cr, eLight which, double cx, double cy, double px, const CHyprColor& c) {
    cairo_save(cr);
    cairo_translate(cr, cx - px / 2, cy - px / 2);
    cairo_scale(cr, px / 16.0, px / 16.0);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    symbolPath(cr, which);
    if (which == CLOSE) {
        cairo_set_line_width(cr, 1.45);
        cairo_stroke(cr);
    } else {
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

static constexpr double LIGHT_PAD = 2.0;

static SP<ITexture> lightTexture(eLight which, const SButtonLook& look, double scale) {
    const int  px      = std::max(1, (int)std::round((LIGHT + 2 * LIGHT_PAD) * scale));
    const auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    const auto cr      = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    const double c = LIGHT / 2 + LIGHT_PAD, r = LIGHT / 2;
    const auto&  fill = look.fill[which];
    if (look.finish == GLASS && !look.hollow)
        dropShadow(cr, c, c, r);
    if (!look.hollow) {
        circle(cr, c, c, r);
        cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, fill.a);
        cairo_fill(cr);
        if (look.finish != FLAT)
            glassFinish(cr, c, c, r, look.finish);
    }
    if (look.ring) {
        circle(cr, c, c, r - 0.75);
        cairo_set_line_width(cr, 1.5);
        cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, fill.a);
        cairo_stroke(cr);
    }
    if (look.symbol.a > 0)
        drawSymbol(cr, which, c, c, g_look.iconPx, look.symbol);
    cairo_surface_flush(surface);
    auto tex = g_pHyprRenderer->createTexture(surface);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return tex;
}

static std::map<std::tuple<int, bool, int>, SP<ITexture>> g_lights;

static SP<ITexture> light(eLight which, bool lit, double scale) {
    auto& tex = g_lights[std::make_tuple((int)which, lit, (int)std::round(scale * 100))];
    if (!tex)
        tex = lightTexture(which, lit ? g_look.lit : g_look.rest, scale);
    return tex;
}

static uint64_t g_lookGeneration = 0;

class CTitleBar : public IHyprWindowDecoration {
  public:
    explicit CTitleBar(PHLWINDOW w) : IHyprWindowDecoration(w), m_window(w) {
        g_bars.push_back(this);
    }
    ~CTitleBar() override {
        std::erase(g_bars, this);
    }

    SDecorationPositioningInfo getPositioningInfo() override {
        SDecorationPositioningInfo info;
        info.policy         = DECORATION_POSITION_STICKY;
        info.edges          = DECORATION_EDGE_TOP;
        info.priority       = BORDER_PRIORITY + 1;
        info.reserved       = true;
        info.desiredExtents = {Vector2D{0.0, visible() ? HEIGHT : 0.0}, Vector2D{}};
        return info;
    }

    void onPositioningReply(const SDecorationPositioningReply& reply) override {
        m_assigned = reply.assignedGeometry;
    }

    eDecorationType getDecorationType() override {
        return DECORATION_CUSTOM;
    }

    void updateWindow(PHLWINDOW) override {
        damageEntire();
    }

    void damageEntire() override {
        g_pHyprRenderer->damageBox(box().expand(2));
    }

    eDecorationLayer getDecorationLayer() override {
        return DECORATION_LAYER_UNDER;
    }

    uint64_t getDecorationFlags() override {
        return DECORATION_ALLOWS_MOUSE_INPUT | DECORATION_PART_OF_MAIN_WINDOW;
    }

    std::string getDisplayName() override {
        return "VarunaTitleBar";
    }

    PHLWINDOW window() const {
        return m_window.lock();
    }

    bool visible() {
        const auto w = m_window.lock();
        return g_look.on && w && !Fullscreen::controller()->isFullscreen(w) && w->m_ruleApplicator->decorate().valueOrDefault();
    }

    void refresh() {
        const bool shown = visible();
        if (m_shown != shown) {
            m_shown = shown;
            g_pDecorationPositioner->repositionDeco(this);
            if (const auto w = m_window.lock(); w && w->layoutTarget())
                w->layoutTarget()->recalc();
        }
        damageEntire();
    }

    CBox box() {
        const auto w = m_window.lock();
        if (!w)
            return {};
        CBox b = m_assigned;
        b.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, w));
        if (w->m_workspace && !w->m_pinned)
            b.translate(w->m_workspace->m_renderOffset->value());
        b.translate(w->m_floatingOffset);
        return b;
    }

    Vector2D lightAt(eLight which) {
        const CBox b = box();
        const double x = FIRST + STEP * static_cast<int>(which);
        return {g_look.right ? b.x + b.w - x : b.x + x, b.y + LIGHT_Y};
    }

    std::optional<eLight> lightUnder(const Vector2D& p) {
        for (const auto which : {CLOSE, MINIMIZE, ZOOM}) {
            const auto c = lightAt(which);
            if (std::abs(p.x - c.x) <= STEP / 2.0 && std::abs(p.y - c.y) <= HEIGHT / 2.0)
                return which;
        }
        return std::nullopt;
    }

    void hover(const Vector2D& p) {
        const bool on = visible() && lightUnder(p).has_value();
        if (on != m_hovered) {
            m_hovered = on;
            damageEntire();
        }
    }

    void draw(PHLMONITOR monitor, float const& a) override {
        if (m_shown != visible())
            refresh();
        if (!m_shown)
            return;
        const auto w = m_window.lock();
        if (!w)
            return;
        const double scale  = monitor->m_scale;
        const bool   active = Desktop::focusState()->window() == w;
        CBox         bar    = box();
        bar.translate(-monitor->m_position);

        const double round = w->rounding();
        CRectPassElement::SRectData rect;
        rect.color = active ? g_look.header.active : g_look.header.backdrop;
        rect.color.a *= a;
        rect.box           = CBox{bar.x, bar.y, bar.w, bar.h + 2 * round}.scale(scale).round();
        rect.clipBox       = CBox{bar.x, bar.y, bar.w, bar.h + round}.scale(scale).round();
        rect.round         = (int)std::round(round * scale);
        rect.roundingPower = w->roundingPower();
        g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(rect));

        CRectPassElement::SRectData shade;
        shade.color = CHyprColor{SHADE_R, SHADE_G, SHADE_B, g_look.header.shade * a};
        shade.box   = CBox{bar.x, bar.y + HEADER, bar.w, SHADE}.scale(scale).round();
        g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(shade));

        const float contents = a * (active ? 1.f : BACKDROP_OPACITY);
        for (const auto which : {CLOSE, MINIMIZE, ZOOM}) {
            const auto tex = light(which, m_hovered, scale);
            if (!tex)
                continue;
            const auto   c    = lightAt(which) - monitor->m_position;
            const double span = LIGHT + 2 * LIGHT_PAD;
            CTexPassElement::SRenderData data;
            data.tex = tex;
            data.box = CBox{c.x - span / 2.0, c.y - span / 2.0, span, span}.scale(scale).round();
            data.a   = contents;
            g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(std::move(data)));
        }

        const auto title = titleTexture(w->m_title, scale, bar.w);
        if (title && title->m_size.x > 0) {
            CBox where{bar.x * scale + (bar.w * scale - title->m_size.x) / 2.0, bar.y * scale + (HEADER * scale - title->m_size.y) / 2.0, title->m_size.x,
                       title->m_size.y};
            CTexPassElement::SRenderData data;
            data.tex = title;
            data.box = where.round();
            data.a   = contents;
            g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(std::move(data)));
        }
    }

    bool onInputOnDeco(const eInputType type, const Vector2D& p, std::any data) override {
        if (!visible())
            return false;
        if (type == INPUT_TYPE_DRAG_START)
            return true;
        if (type != INPUT_TYPE_BUTTON)
            return false;
        const auto e = std::any_cast<const IPointer::SButtonEvent&>(data);
        const auto w = m_window.lock();
        if (!w || e.button != BTN_LEFT)
            return true;
        const auto on = lightUnder(p);
        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (on && m_pressed == on)
                press(*on, w);
            m_pressed.reset();
            return true;
        }
        if (!Desktop::focusState()->isWindowActive(w))
            Desktop::focusState()->rawWindowFocus(w, Desktop::FOCUS_REASON_CLICK);
        if (w->m_isFloating)
            Desktop::windowState()->raise(w);
        if (on) {
            m_pressed = on;
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastPress < DOUBLE_TIME && m_lastPressAt.distance(p) < DOUBLE_NEAR) {
            m_lastPress = {};
            requests::post("varunafill", w, true);
            return true;
        }
        m_lastPress   = now;
        m_lastPressAt = p;
        if (w->layoutTarget()) {
            g_layoutManager->beginDragTarget(w->layoutTarget(), MBIND_MOVE);
            g_dragStartedOnBar = true;
        }
        return true;
    }

  private:
    PHLWINDOWREF                          m_window;
    CBox                                  m_assigned;
    bool                                  m_shown   = false;
    bool                                  m_hovered = false;
    std::optional<eLight>                 m_pressed;
    std::chrono::steady_clock::time_point m_lastPress{};
    Vector2D                              m_lastPressAt;

    struct STitle {
        std::string  text;
        int          scale = 0, width = 0;
        uint64_t     lookGeneration = 0;
        SP<ITexture> tex;
    } m_title;

    SP<ITexture> titleTexture(const std::string& text, double scale, double barWidth) {
        const int width = std::max(0, (int)std::round((barWidth - 2 * LIGHTS_ROOM) * scale));
        const int key   = (int)std::round(scale * 100);
        if (m_title.tex && m_title.text == text && m_title.scale == key && m_title.width == width && m_title.lookGeneration == g_lookGeneration)
            return m_title.tex;
        static auto fontFamily = CConfigValue<std::string>("misc:font_family");
        const int   px         = (int)std::round(g_look.fontPt * PT_TO_PX * scale);
        m_title = {text, key, width, g_lookGeneration,
                   width > 0 && !text.empty() ? g_pHyprRenderer->renderText(text, g_look.header.fg, px, false, *fontFamily, width, TITLE_WEIGHT) : nullptr};
        return m_title.tex;
    }

    static void press(eLight which, const PHLWINDOW& w) {
        switch (which) {
            case CLOSE: g_pXWaylandManager->sendCloseWindow(w); break;
            case MINIMIZE: requests::post("minimized", w, true); break;
            case ZOOM: requests::post(requests::g_limited && requests::g_limited(w) ? "varunafill" : "varunazoom", w, true); break;
        }
    }
};

class CSquareTopCorners : public IHyprWindowDecoration {
  public:
    explicit CSquareTopCorners(PHLWINDOW w) : IHyprWindowDecoration(w), m_window(w) {
        g_corners.push_back(this);
    }
    ~CSquareTopCorners() override {
        std::erase(g_corners, this);
    }

    SDecorationPositioningInfo getPositioningInfo() override {
        return {};
    }

    void onPositioningReply(const SDecorationPositioningReply&) override {}

    eDecorationType getDecorationType() override {
        return DECORATION_CUSTOM;
    }

    void updateWindow(PHLWINDOW) override {}

    void damageEntire() override {}

    eDecorationLayer getDecorationLayer() override {
        return DECORATION_LAYER_OVER;
    }

    uint64_t getDecorationFlags() override {
        return 0;
    }

    std::string getDisplayName() override {
        return "VarunaSquareTopCorners";
    }

    PHLWINDOW window() const {
        return m_window.lock();
    }

    void draw(PHLMONITOR monitor, float const& a) override {
        const auto w = m_window.lock();
        if (!w || !barShown(w))
            return;
        const double round = w->rounding();
        const auto   surface = w->resource();
        const auto   tex     = surface ? surface->m_current.texture : nullptr;
        if (round <= 0 || !tex || surface->m_current.transform != WL_OUTPUT_TRANSFORM_NORMAL || surface->m_current.viewport.hasSource)
            return;
        CBox window = w->getWindowMainSurfaceBox();
        if (w->m_workspace && !w->m_pinned)
            window.translate(w->m_workspace->m_renderOffset->value());
        window.translate(w->m_floatingOffset - monitor->m_position);
        const double scale = monitor->m_scale;
        for (const double x : {window.x, window.x + window.w - round}) {
            CTexPassElement::SRenderData data;
            data.tex     = tex;
            data.box     = CBox{window}.scale(scale).round();
            data.clipBox = CBox{x, window.y, round, round}.scale(scale).round();
            data.a       = a;
            g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(std::move(data)));
        }
    }

  private:
    PHLWINDOWREF m_window;

    static bool barShown(const PHLWINDOW& w);
};

bool CSquareTopCorners::barShown(const PHLWINDOW& w) {
    return std::ranges::any_of(g_bars, [&](CTitleBar* b) { return b->window() == w && b->visible(); });
}

static void attach(const PHLWINDOW& w) {
    if (!drawsNoTitleBar(w) || std::ranges::any_of(g_bars, [&](CTitleBar* b) { return b->window() == w; }))
        return;
    w->addWindowDeco(makeUnique<CTitleBar>(w));
    w->addWindowDeco(makeUnique<CSquareTopCorners>(w));
}

static void refreshAll() {
    g_lights.clear();
    ++g_lookGeneration;
    for (auto* b : std::vector(g_bars))
        b->refresh();
}

static void configure(const SLook& look) {
    const bool changed = !(look == g_look);
    g_look = look;
    if (changed)
        refreshAll();
}

static void init() {
    static auto onOpen = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { attach(w); });
    static auto onFull = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW w) {
        for (auto* b : std::vector(g_bars))
            if (b->window() == w)
                b->refresh();
    });
    static auto onTitle = Event::bus()->m_events.window.title.listen([](PHLWINDOW w) {
        for (auto* b : g_bars)
            if (b->window() == w)
                b->damageEntire();
    });
    static auto onActive = Event::bus()->m_events.window.active.listen([](PHLWINDOW, Desktop::eFocusReason) {
        for (auto* b : g_bars)
            b->damageEntire();
    });
    static auto onMove = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) {
        const auto p = g_pInputManager->getMouseCoordsInternal();
        for (auto* b : g_bars)
            b->hover(p);
    });
    static auto onButton = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
        if (g_dragStartedOnBar && e.button == BTN_LEFT && e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            g_dragStartedOnBar = false;
            if (g_layoutManager->dragController()->target())
                g_layoutManager->endDragTarget();
        }
    });
    for (const auto& w : Desktop::windowState()->windows())
        if (w->m_isMapped)
            attach(w);
}

static void stop() {
    if (g_dragStartedOnBar && g_layoutManager->dragController()->target())
        g_layoutManager->endDragTarget();
    g_dragStartedOnBar = false;
    const auto takeOff = [](IHyprWindowDecoration* deco, const PHLWINDOW& w) {
        if (!w)
            return;
        g_pDecorationPositioner->uncacheDecoration(deco);
        std::erase_if(w->m_windowDecorations, [deco](const auto& d) { return d.get() == deco; });
        g_pDecorationPositioner->forceRecalcFor(w);
        if (w->layoutTarget())
            w->layoutTarget()->recalc();
    };
    for (auto* c : std::vector(g_corners))
        takeOff(c, c->window());
    for (auto* b : std::vector(g_bars))
        takeOff(b, b->window());
    g_corners.clear();
    g_bars.clear();
    g_lights.clear();
}

} // namespace titlebar
