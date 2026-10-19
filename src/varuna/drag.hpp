// Dragging a window keeps the spot you grabbed under the pointer.
//
// When a drag starts on a tiled window, Hyprland floats it, shrinks it, and
// centres it on the pointer, so the window jumps and the pointer ends up in
// its middle, wherever you grabbed it (the title bar, usually). Like macOS,
// keep the grab point instead: the same fraction across the window, and the
// same distance down from its top (clamped to the new size).
//
// Hyprland re-centres the window and then floats it, and floating emits
// window.floating right away, inside the same call, before it notes where
// the drag starts. So moving the window from that event is enough: the drag
// then carries on from the corrected place. (Function hooks would be the
// obvious tool, but Hyprland only supports them on x86-64.) Where the window
// was before is remembered each frame, while it's still tiled.
#pragma once

#include <algorithm>
#include <unordered_map>

#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>

namespace drag {

// Tiled windows' boxes, as of the last frame, and as of the last button
// press with where the pointer was: apps ask to be moved only once the
// pointer has travelled a little (GTK's drag threshold), and the grab point
// is where the button went down.
static std::unordered_map<const Desktop::View::CWindow*, CBox> g_tiled, g_pressed;
static Vector2D                                                g_press;

static void remember() {
    g_tiled.clear();
    for (const auto& w : Desktop::windowState()->windows()) {
        // The window itself (the layout's box also holds the gaps).
        if (w && w->m_isMapped && !w->m_isFloating)
            if (const auto box = w->logicalBox())
                g_tiled[w.get()] = *box;
    }
}

static void onFloating(const PHLWINDOW& w) {
    const auto& dc     = g_layoutManager->dragController();
    const auto  target = dc ? dc->target() : nullptr;
    if (!w || !w->m_isFloating || !target || target->window() != w || dc->mode() != MBIND_MOVE)
        return;
    const auto pointer = g_pInputManager->getMouseCoordsInternal();
    // Grabbed at the press, if it was on this window; else where it is now.
    auto       it      = g_pressed.find(w.get());
    Vector2D   grab    = g_press;
    if (it == g_pressed.end() || !it->second.containsPoint(grab)) {
        it   = g_tiled.find(w.get());
        grab = pointer;
        if (it == g_tiled.end())
            return;
    }
    const CBox was = it->second;
    if (was.w <= 0 || was.h <= 0)
        return;
    const CBox   now = target->position();
    const double fx  = std::clamp((grab.x - was.x) / was.w, 0.0, 1.0);
    const double dy  = std::clamp(grab.y - was.y, 0.0, std::max(0.0, now.h - 1.0));
    target->setPositionGlobal(CBox{pointer.x - fx * now.w, pointer.y - dy, now.w, now.h});
}

static void init() {
    static auto floating = Event::bus()->m_events.window.floating.listen([](PHLWINDOW w) { onFloating(w); });
    static auto press    = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
        if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            g_press   = g_pInputManager->getMouseCoordsInternal();
            g_pressed = g_tiled;
        }
    });
}

} // namespace drag
