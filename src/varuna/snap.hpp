// Snapping floating windows (when tiling is off), like macOS and Windows.
//
// Dragging: carry a window to the edge of the screen and where it would go
// lights up (a translucent rounded box): the left or right edge, that half;
// a corner, that quarter; the top, the whole screen (the bar and Dock keep
// their space; the top is reached at the bar: the pointer touching it, or
// just under it). Let go there and the window snaps to it; move away and
// the suggestion goes.
//
// The green button: hover over it for three seconds and the shell shows a
// grid of layouts to pick from. Apps draw their own title-bar buttons, so
// the compositor only knows where the button is: in Varuna's title bars
// (GTK apps, styled by varuna's gtk.css) it sits at the same place in
// every window. Only windows of GTK apps are watched (other apps draw other
// title bars), seen from the GTK library their process has loaded. When it
// fires, the plugin posts
//
//   varunasnapgrid>>ADDR,X,Y   (the button's middle, layout coordinates)
//
// and the shell's grid snaps the window with `hyprctl varunaglass snap ADDR
// ZONE`, through the same layouts as dragging.
//
// Settings (glass.conf): snap (on when tiling is off and snapping is),
// snap_gap (around the screen's edge), snap_gap_in (between two windows),
// buttons_right (title-bar buttons on the right).
#pragma once

#include <chrono>
#include <fstream>
#include <format>
#include <string>
#include <unordered_map>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>

namespace snap {

enum class Zone { None, Left, Right, Top, Bottom, Fill, TopLeft, TopRight, BottomLeft, BottomRight };

static bool  g_on     = false;
static float g_gap    = 8.f;
static float g_gapIn  = 4.f;
static bool  g_right  = false;

/// How close to the screen's edge the pointer must be, and how far along an
/// edge from a corner still counts as the corner.
static constexpr double EDGE   = 6.0;
static constexpr double CORNER = 90.0;
/// The green button's middle in Varuna's title bars, from the window's
/// top-left (or top-right, buttons on the right), as measured in GTK 3 and
/// 4 apps (Files, Settings, Calculator, Ghostty): the button is ~14 px
/// across. The pointer counts as on it within NEAR_X / NEAR_Y of the
/// middle (the button and a little round it: a hand resting on it drifts),
/// and, once resting, until it strays past KEEP (a wobble doesn't start
/// the wait over).
static constexpr double BUTTON_X = 63.0, BUTTON_X_RIGHT = 41.0, BUTTON_Y = 22.5;
static constexpr double NEAR_X = 9.0, NEAR_Y = 11.0, KEEP = 15.0;
static constexpr auto   HOVER    = std::chrono::seconds(3);

static void configure(bool on, float gap, float gapIn, bool right) {
    g_on    = on;
    g_gap   = gap;
    g_gapIn = gapIn;
    g_right = right;
}

static Zone parseZone(const std::string& s) {
    static const std::pair<const char*, Zone> names[] = {
        {"left", Zone::Left},          {"right", Zone::Right},         {"top", Zone::Top},
        {"bottom", Zone::Bottom},      {"fill", Zone::Fill},           {"top-left", Zone::TopLeft},
        {"top-right", Zone::TopRight}, {"bottom-left", Zone::BottomLeft}, {"bottom-right", Zone::BottomRight},
    };
    for (const auto& [n, z] : names)
        if (s == n)
            return z;
    return Zone::None;
}

/// Where a window in `zone` goes on monitor `mon`: inside the area the bar
/// and Dock leave, with gaps like tiling's.
static CBox boxFor(Zone zone, const PHLMONITOR& mon) {
    const CBox   a  = mon->logicalBoxMinusReserved();
    const double g = g_gap, gi = g_gapIn;
    const double x = a.x + g, y = a.y + g, w = a.w - 2 * g, h = a.h - 2 * g;
    const double hw = (w - gi) / 2, hh = (h - gi) / 2;
    switch (zone) {
        case Zone::Left: return {x, y, hw, h};
        case Zone::Right: return {x + hw + gi, y, hw, h};
        case Zone::Top: return {x, y, w, hh};
        case Zone::Bottom: return {x, y + hh + gi, w, hh};
        case Zone::Fill: return {x, y, w, h};
        case Zone::TopLeft: return {x, y, hw, hh};
        case Zone::TopRight: return {x + hw + gi, y, hw, hh};
        case Zone::BottomLeft: return {x, y + hh + gi, hw, hh};
        case Zone::BottomRight: return {x + hw + gi, y + hh + gi, hw, hh};
        case Zone::None: break;
    }
    return {};
}

static CBox insideDecorations(const PHLWINDOW& w, const CBox& box) {
    const auto e = g_pDecorationPositioner->getWindowDecorationReserved(w);
    return {box.x + e.topLeft.x, box.y + e.topLeft.y, box.w - e.topLeft.x - e.bottomRight.x, box.h - e.topLeft.y - e.bottomRight.y};
}

static CBox windowBoxFor(const PHLWINDOW& w, Zone zone, const PHLMONITOR& mon) {
    return insideDecorations(w, boxFor(zone, mon));
}

/// The zone the pointer at `p` (on monitor `mon`) suggests while dragging.
///
/// The top edge is the top bar's lower edge, not the screen's: no window goes
/// under the bar (keepclear.hpp), so a drag stops there, and the pointer
/// reaching the bar (or just under it) is the pointer reaching the top.
static Zone zoneAt(const Vector2D& p, const PHLMONITOR& mon) {
    const CBox m      = mon->logicalBox();
    const double top0 = mon->logicalBoxMinusReserved().y;
    const bool left   = p.x <= m.x + EDGE, right = p.x >= m.x + m.w - 1 - EDGE;
    const bool top    = p.y <= top0 + EDGE, bottom = p.y >= m.y + m.h - 1 - EDGE;
    const bool nearL  = p.x <= m.x + CORNER, nearR = p.x >= m.x + m.w - CORNER;
    const bool nearT  = p.y <= top0 + CORNER, nearB = p.y >= m.y + m.h - CORNER;
    if ((top && nearL) || (left && nearT))
        return Zone::TopLeft;
    if ((top && nearR) || (right && nearT))
        return Zone::TopRight;
    if ((bottom && nearL) || (left && nearB))
        return Zone::BottomLeft;
    if ((bottom && nearR) || (right && nearB))
        return Zone::BottomRight;
    if (left)
        return Zone::Left;
    if (right)
        return Zone::Right;
    if (top)
        return Zone::Fill;
    return Zone::None;
}

/// Put floating `w` at `box`.
/// `box` shrunk to what `w` allows (its window rule's max_size, or the
/// app's own maximum), centred in it: a window is never placed past its
/// limit, so filling a limited window zooms it, like macOS.
static CBox fitted(const PHLWINDOW& w, const CBox& box) {
    const auto max = w->maxSize();
    if (!max)
        return box;
    const double fw = std::min(box.w, max->x), fh = std::min(box.h, max->y);
    return {box.x + (box.w - fw) / 2, box.y + (box.h - fh) / 2, fw, fh};
}

/// Whether `w` can't grow to fill its monitor (then it zooms instead of
/// going full screen).
static bool limited(const PHLWINDOW& w) {
    const auto mon = w ? w->m_monitor.lock() : nullptr;
    if (!mon)
        return false;
    const CBox fill = windowBoxFor(w, Zone::Fill, mon), fit = fitted(w, fill);
    return fit.w < fill.w - 1 || fit.h < fill.h - 1;
}

// ---- the size before snapping

/// A snapped window: before, and now.
struct SSnapped {
    PHLWINDOWREF w;
    CBox         before, at;
};
static std::unordered_map<const Desktop::View::CWindow*, SSnapped> g_snapped;

/// Where `w` is going (not mid-animation).
static CBox boxOf(const PHLWINDOW& w) {
    const Vector2D at = w->positionAnimation()->goal(), size = w->sizeAnimation()->goal();
    return {at.x, at.y, size.x, size.y};
}

static bool near(const CBox& a, const CBox& b) {
    return std::abs(a.x - b.x) < 2 && std::abs(a.y - b.y) < 2 && std::abs(a.w - b.w) < 2 && std::abs(a.h - b.h) < 2;
}

/// Note `w` snapped to `to`.
static void noteSnap(const PHLWINDOW& w, const CBox& to) {
    std::erase_if(g_snapped, [](const auto& kv) { return kv.second.w.expired(); });
    const CBox now = boxOf(w);
    const auto it  = g_snapped.find(w.get());
    // Snapped already: keep the first size.
    const CBox before = it != g_snapped.end() && near(now, it->second.at) ? it->second.before : now;
    g_snapped[w.get()] = {w, before, to};
}

/// Put floating `w` at `box`, as a snap.
static void place(const PHLWINDOW& w, const CBox& box, bool snap = true) {
    if (!w || !w->m_isFloating || box.w < 1 || box.h < 1)
        return;
    const auto target = w->layoutTarget();
    if (!target)
        return;
    const CBox to = fitted(w, box);
    if (snap)
        noteSnap(w, to);
    else
        g_snapped.erase(w.get());
    target->setPositionGlobal(to);
}

/// A snapped window dragged: its size back (the drag controller asks on
/// the drag's first move, and starts the drag over from the new box).
static bool restoreOnDrag(const SP<Layout::ITarget>& target) {
    const auto w = target ? target->window() : nullptr;
    if (!w)
        return false;
    const auto it = g_snapped.find(w.get());
    if (it == g_snapped.end())
        return false;
    const SSnapped s = it->second;
    g_snapped.erase(it);
    const CBox now = boxOf(w);
    if (!w->m_isFloating || now.w < 1 || now.h < 1)
        return false;
    // The grabbed spot stays under it.
    const Vector2D p  = g_pInputManager->getMouseCoordsInternal();
    const double   fx = std::clamp((p.x - now.x) / now.w, 0.0, 1.0);
    const double   dy = std::clamp(p.y - now.y, 0.0, std::max(0.0, s.before.h - 1.0));
    const CBox     back{p.x - fx * s.before.w, p.y - dy, s.before.w, s.before.h};
    target->setPositionGlobal(back);
    w->positionAnimation()->warp();
    w->sizeAnimation()->warp();
    return true;
}

// ---- dragging

static PHLWINDOWREF g_dragged;
static Zone         g_zone = Zone::None;
static PHLMONITORREF g_zoneMonitor;

static void damage() {
    if (const auto mon = g_zoneMonitor.lock())
        g_pHyprRenderer->damageMonitor(mon);
}

static void setZone(Zone z, const PHLMONITOR& mon) {
    if (z == g_zone && mon == g_zoneMonitor.lock())
        return;
    damage();
    g_zone        = z;
    g_zoneMonitor = mon;
    damage();
}

static void onMove() {
    const auto& dc     = g_layoutManager->dragController();
    const auto  target = dc ? dc->target() : nullptr;
    const auto  w      = target ? target->window() : nullptr;
    if (w && dc->mode() == MBIND_RESIZE)
        g_snapped.erase(w.get());
    if (!g_on || !w || !w->m_isFloating || dc->mode() != MBIND_MOVE) {
        if (g_zone != Zone::None)
            setZone(Zone::None, nullptr);
        g_dragged.reset();
        return;
    }
    const auto p   = g_pInputManager->getMouseCoordsInternal();
    const auto mon = State::monitorState()->query().vec(p).run();
    if (!mon)
        return;
    g_dragged = w;
    setZone(zoneAt(p, mon), mon);
}

static void onRelease() {
    const auto w   = g_dragged.lock();
    const auto mon = g_zoneMonitor.lock();
    const Zone z   = g_zone;
    setZone(Zone::None, nullptr);
    g_dragged.reset();
    if (!w || !mon || z == Zone::None)
        return;
    // After Hyprland ends the drag (it puts the window where it was let go).
    const CBox box = windowBoxFor(w, z, mon);
    PHLWINDOWREF ref = w;
    g_pEventLoopManager->doLater([ref, box] { place(ref.lock(), box); });
}

/// The suggestion, drawn over the windows of `mon` while dragging.
static void render(const PHLMONITOR& mon) {
    if (g_zone == Zone::None || g_zoneMonitor.lock() != mon)
        return;
    CBox box = boxFor(g_zone, mon);
    box.translate(-mon->m_position).scale(mon->m_scale).round();
    CRectPassElement::SRectData data;
    data.box   = box;
    data.color = CHyprColor{1.0, 1.0, 1.0, 0.18};
    data.round = (int)std::round(14 * mon->m_scale);
    data.blur  = true;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
}

// ---- the green button

static std::unordered_map<pid_t, bool> g_gtk;

/// Whether process `pid` draws GTK's (so Varuna's) title bar: it has GTK
/// loaded, and isn't a browser or Electron app, which load GTK too but
/// draw title bars of their own (Firefox's buttons are elsewhere).
static bool isGtk(pid_t pid) {
    if (pid <= 0)
        return false;
    if (const auto it = g_gtk.find(pid); it != g_gtk.end())
        return it->second;
    std::ifstream maps(std::format("/proc/{}/maps", pid));
    bool          gtk = false, own = false;
    for (std::string line; std::getline(maps, line);) {
        if (line.find("libgtk-4.so") != std::string::npos || line.find("libgtk-3.so") != std::string::npos)
            gtk = true;
        // Firefox (and Thunderbird), Chromium and Electron.
        if (line.find("libxul.so") != std::string::npos || line.find("libffmpeg.so") != std::string::npos ||
            line.find("libcef.so") != std::string::npos)
            own = true;
    }
    g_gtk[pid] = gtk && !own;
    return g_gtk[pid];
}

static PHLWINDOWREF          g_hovered;
/// The window whose layouts were just shown: not again until the pointer
/// leaves its button.
static PHLWINDOWREF          g_shown;
static bool                  g_checkQueued = false;
static SP<CEventLoopTimer>   g_timer;
static Vector2D              g_button;

static void cancelHover() {
    g_hovered.reset();
    g_shown.reset();
    if (g_timer) {
        g_pEventLoopManager->removeTimer(g_timer);
        g_timer.reset();
    }
}

static void onHoverTimer() {
    const auto w = g_hovered.lock();
    if (!w || !g_on)
        return;
    // Still there (the pointer may have left without a move being seen).
    const auto p = g_pInputManager->getMouseCoordsInternal();
    if (std::abs(p.x - g_button.x) > KEEP || std::abs(p.y - g_button.y) > KEEP) {
        cancelHover();
        return;
    }
    g_pEventManager->postEvent(SHyprIPCEvent{
        .event = "varunasnapgrid", .data = std::format("{:x},{},{}", rc<uintptr_t>(w.get()), (int)g_button.x, (int)g_button.y)});
    // Once per stay: leaving the button and coming back shows it again.
    g_hovered.reset();
    g_shown = w;
}

/// The middle of `w`'s green button (from the window itself: logicalBox()
/// has room around it for the border).
static Vector2D buttonOf(const PHLWINDOW& w) {
    const Vector2D at = w->positionAnimation()->value(), size = w->sizeAnimation()->value();
    return {g_right ? at.x + size.x - BUTTON_X_RIGHT : at.x + BUTTON_X, at.y + BUTTON_Y};
}

/// Whether `p` is on `w`'s green button (GTK title bars only: others draw
/// their buttons where they like).
static bool onGreenButton(const PHLWINDOW& w, const Vector2D& p) {
    if (!w || !isGtk(w->getPID()))
        return false;
    const Vector2D b = buttonOf(w);
    return std::abs(p.x - b.x) <= NEAR_X && std::abs(p.y - b.y) <= NEAR_Y;
}

/// Whether `p` is on the green button of the window there.
static bool onGreenButtonAt(const Vector2D& p) {
    return onGreenButton(Desktop::viewState()->hitTest().windowAt(
                             p, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING),
                         p);
}

static void onPointer() {
    const auto& dc = g_layoutManager->dragController();
    if (!g_on || (dc && dc->target())) {
        cancelHover();
        return;
    }
    const auto p = g_pInputManager->getMouseCoordsInternal();
    const auto w = Desktop::viewState()->hitTest().windowAt(
        p, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
    // Full screen there's nothing to snap it to (and its title bar, if it
    // shows one, is the app's own).
    if (!w || !w->m_isFloating || !isGtk(w->getPID()) || Fullscreen::controller()->isFullscreen(w)) {
        cancelHover();
        return;
    }
    const Vector2D button = buttonOf(w);
    const double   dx = std::abs(p.x - button.x), dy = std::abs(p.y - button.y);
    // Already resting here (or just shown): only straying well off it
    // starts over.
    if (g_hovered.lock() == w || g_shown.lock() == w) {
        if (dx > KEEP || dy > KEEP)
            cancelHover();
        return;
    }
    if (dx > NEAR_X || dy > NEAR_Y) {
        cancelHover();
        return;
    }
    g_hovered = w;
    g_button  = button;
    // A fresh timer, already set: the event loop arms a timer when it's
    // added (re-arming one it has wouldn't be seen until something else
    // nudges it).
    if (g_timer)
        g_pEventLoopManager->removeTimer(g_timer);
    g_timer = makeShared<CEventLoopTimer>(HOVER, [](SP<CEventLoopTimer>, void*) { onHoverTimer(); }, nullptr);
    g_pEventLoopManager->addTimer(g_timer);
}

// ---- filling the screen (a title-bar double-click)

/// Fill the screen under the bar with floating `w`, or, filled already, put
/// it back where it was (or, not knowing, a smaller window in the middle).
static void toggleFill(const PHLWINDOW& w) {
    const auto mon = w->m_monitor.lock();
    if (!mon)
        return;
    const CBox fill = fitted(w, windowBoxFor(w, Zone::Fill, mon));
    if (near(boxOf(w), fill)) {
        CBox back{fill.x + fill.w / 6, fill.y + fill.h / 6, fill.w * 2 / 3, fill.h * 2 / 3};
        if (const auto it = g_snapped.find(w.get()); it != g_snapped.end())
            back = it->second.before;
        place(w, back, false);
        return;
    }
    place(w, fill);
}

// ---- hyprctl varunaglass snap ADDR ZONE, varunaglass fill ADDR

static std::string command(std::istringstream& in, bool fill = false) {
    std::string addr, zoneName;
    in >> addr;
    if (!fill)
        in >> zoneName;
    const Zone zone = fill ? Zone::Fill : parseZone(zoneName);
    if (addr.empty() || zone == Zone::None)
        return "usage: varunaglass snap <address> left|right|top|bottom|fill|top-left|top-right|bottom-left|bottom-right";
    const auto want = std::stoull(addr.starts_with("0x") ? addr.substr(2) : addr, nullptr, 16);
    for (const auto& w : Desktop::windowState()->windows()) {
        if (w && rc<uintptr_t>(w.get()) == want) {
            const auto mon = w->m_monitor.lock();
            if (!mon)
                return "no monitor";
            if (!w->m_isFloating)
                return "not floating";
            if (fill)
                toggleFill(w);
            else
                place(w, windowBoxFor(w, zone, mon));
            return "ok";
        }
    }
    return "no such window";
}

static void init() {
    static auto move = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) {
        onMove();
        // Where the pointer ends up: during the event it may not have
        // moved yet, and a hand coming to rest sends its last move here.
        if (!g_checkQueued) {
            g_checkQueued = true;
            g_pEventLoopManager->doLater([] {
                g_checkQueued = false;
                onPointer();
            });
        }
    });
    static auto button = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED)
            onRelease();
        else
            cancelHover();
    });
}

static void stop() {
    cancelHover();
    g_snapped.clear();
}

} // namespace snap
