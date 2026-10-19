// Title-bar buttons, like macOS.
//
// Hyprland ignores an app's own minimize request (xdg_toplevel.set_minimized;
// it only reports task bars' requests), and varuna's rules suppress maximize
// requests. Pass both on as IPC events so the shell can act on them:
//
//   minimized>>ADDR,1     the same event Hyprland sends for a task bar's
//                         request; the shell minimizes into the dock
//   varunazoom>>ADDR,0|1  the green button; the shell toggles full screen
//                         or maximize, as configured
//   varunafill>>ADDR,0|1  a title-bar double-click (the same request, but
//                         right after two quick clicks in one place, not on
//                         the green button): the shell fills the screen
//                         under the bar with it, or puts it back. Also the
//                         green button of a window with a size limit: it
//                         zooms to its limit (snap.hpp's fitted)
//   varunafullscreen>>ADDR,0|1
//                         an app asking for full screen (F11); varuna's rules
//                         suppress it so the shell's full screen is used
//                         (laid out maximized, so the bar can push it down)
//
// X11 apps' maximize requests (suppressed by the rules too) become
// varunazoom / varunafill the same way.
//
// Hyprland also tells apps it can only maximize and go full screen, so GTK
// greys out the minimize button; now that minimize works, each window is
// told it can minimize too.
#pragma once

#include <chrono>
#include <format>
#include <map>

#include <linux/input-event-codes.h>

#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>

namespace requests {

struct SWatch {
    PHLWINDOWREF        window;
    CHyprSignalListener stateChanged;
};
static std::map<const Desktop::View::CWindow*, SWatch> g_watches;

// The last two left-button presses, to tell a double-click.
struct SPress {
    std::chrono::steady_clock::time_point at{};
    Vector2D                              pos;
    bool                                  green = false; // on a green button
};
static SPress g_presses[2];
/// How quick and close two clicks are to be a double-click (GTK's own
/// double-click time is 400 ms), and how soon after it the request comes.
static constexpr auto   DOUBLE_TIME = std::chrono::milliseconds(500), REQUEST_TIME = std::chrono::milliseconds(800);
static constexpr double DOUBLE_NEAR = 8.0;
/// Whether a point is on a window's green button (snap.hpp's).
static bool (*g_onGreenButton)(const Vector2D&) = nullptr;
/// Whether a window has a size limit smaller than its screen (snap.hpp's).
static bool (*g_limited)(const PHLWINDOW&) = nullptr;

static void onPress() {
    g_presses[0] = g_presses[1];
    const auto p = g_pInputManager->getMouseCoordsInternal();
    // Judged now: the first click may move the window (into full screen).
    g_presses[1] = {std::chrono::steady_clock::now(), p, g_onGreenButton && g_onGreenButton(p)};
}

/// Whether `w`'s maximize request comes from a double-click on its title
/// bar (rather than its green button, a shortcut or a menu).
static bool doubleClicked() {
    const auto& [a, b] = g_presses;
    if (std::chrono::steady_clock::now() - b.at > REQUEST_TIME || b.at - a.at > DOUBLE_TIME || a.pos.distance(b.pos) > DOUBLE_NEAR)
        return false;
    // A double-click on the green button is two clicks on it.
    return !a.green && !b.green;
}

/// A maximize request: a fill on a double-click, else the green button's
/// zoom -- except for a window with a size limit (Varuna Settings), which
/// can't go full screen: its green button zooms it to its limit, as a
/// double-click does.
static const char* maximizeEvent(const PHLWINDOW& w) {
    return doubleClicked() || (g_limited && g_limited(w)) ? "varunafill" : "varunazoom";
}

static void post(const char* event, const PHLWINDOW& w, bool on) {
    g_pEventManager->postEvent(SHyprIPCEvent{.event = event, .data = std::format("{:x},{}", rc<uintptr_t>(w.get()), on ? 1 : 0)});
}

// wm_capabilities: fullscreen and maximize (as Hyprland sends) plus minimize,
// applied by the app with the next configure.
static void announceMinimize(const SP<CXDGToplevelResource>& toplevel) {
    if (!toplevel->m_resource || toplevel->m_resource->version() < 5)
        return;
    wl_array caps;
    wl_array_init(&caps);
    for (const auto cap : {XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN, XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE, XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE})
        *sc<uint32_t*>(wl_array_add(&caps, sizeof(uint32_t))) = cap;
    toplevel->m_resource->sendWmCapabilities(&caps);
    wl_array_release(&caps);
    if (const auto surface = toplevel->m_owner.lock())
        surface->scheduleConfigure();
}

static void watch(const PHLWINDOW& w) {
    if (!w || g_watches.contains(w.get()))
        return;
    PHLWINDOWREF ref = w;
    // X11 apps (their own title bars, mostly): the maximize button, or a
    // double-click on the title bar. Their full screen stays Hyprland's.
    if (const auto x = w->m_xwaylandSurface.lock()) {
        WP<CXWaylandSurface> xs       = x;
        auto                 listener = x->m_events.stateChanged.listen([ref, xs] {
            const auto s   = xs.lock();
            const auto win = ref.lock();
            if (s && win && s->m_state.requestsMaximize.has_value())
                post(maximizeEvent(win), win, *s->m_state.requestsMaximize);
        });
        g_watches[w.get()] = SWatch{ref, std::move(listener)};
        return;
    }
    const auto surface = w->m_xdgSurface.lock();
    const auto toplevel = surface ? surface->m_toplevel.lock() : nullptr;
    if (!toplevel)
        return;
    announceMinimize(toplevel);
    WP<CXDGToplevelResource> top = toplevel;
    // The request flags are only set while stateChanged is being emitted.
    auto listener = toplevel->m_events.stateChanged.listen([ref, top] {
        const auto t   = top.lock();
        const auto win = ref.lock();
        if (!t || !win)
            return;
        if (t->m_state.requestsMinimize.value_or(false))
            post("minimized", win, true);
        if (t->m_state.requestsMaximize.has_value())
            post(maximizeEvent(win), win, *t->m_state.requestsMaximize);
        if (t->m_state.requestsFullscreen.has_value())
            post("varunafullscreen", win, *t->m_state.requestsFullscreen);
    });
    g_watches[w.get()] = SWatch{ref, std::move(listener)};
}

// Forget windows that are gone.
static void prune() {
    std::erase_if(g_watches, [](const auto& kv) { return kv.second.window.expired(); });
}

static void init() {
    static auto onOpen    = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) {
        prune();
        watch(w);
    });
    static auto onButton  = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
        if (e.button == BTN_LEFT && e.state == WL_POINTER_BUTTON_STATE_PRESSED)
            onPress();
    });
    static auto onDestroy = Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF) { prune(); });
    // Windows that were already open when the plugin loaded.
    for (const auto& w : Desktop::windowState()->windows())
        watch(w);
}

static void stop() {
    g_watches.clear();
}

} // namespace requests
