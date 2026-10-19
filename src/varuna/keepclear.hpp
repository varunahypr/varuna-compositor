// No window goes under the top bar.
//
// Tiled windows already stay inside the monitor's reserved area (the bar's
// exclusive zone), but floating ones don't: a dialog taller than the room
// left is centred on the whole monitor, apps can place their windows where
// they like, and a drag could carry one up behind the bar. Like macOS's menu
// bar, the bar is off limits: each frame, before it is drawn, a floating
// window whose top (border included) is above the reserved area's is moved
// down (at once, not by animation, so it is never seen under the bar), and
// one too tall for the room below the bar is shortened to fit.
//
// Fullscreen windows are left alone: varuna's full screen hides the bar and
// pushes the window down itself when the bar slides in.
#pragma once

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

namespace keepclear {

// Heights we asked windows to shrink from. An app with a minimum size may
// keep its height anyway; then don't ask again every frame.
static std::unordered_map<const Desktop::View::CWindow*, double> g_refused;

static void clamp(const PHLMONITOR& mon) {
    const CBox area = mon->logicalBoxMinusReserved();
    for (const auto& w : Desktop::windowState()->windows()) {
        if (!w || !w->m_isMapped || !w->m_isFloating || w->m_monitor.lock() != mon || w->isX11OverrideRedirect())
            continue;
        if (Fullscreen::controller()->isFullscreen(w))
            continue;
        const auto target = w->layoutTarget();
        if (!target)
            continue;
        const auto deco = w->getFullWindowReservedArea();
        CBox       box  = target->position();
        const double top    = area.y + deco.topLeft.y;
        const double bottom = mon->logicalBox().y + mon->logicalBox().h - deco.bottomRight.y;
        const double room   = bottom - top;
        if (box.y >= top - 0.5 && box.h <= room + 0.5)
            continue;
        if (box.h > room + 0.5) {
            const auto it = g_refused.find(w.get());
            if (it == g_refused.end() || std::abs(it->second - box.h) > 0.5) {
                g_refused[w.get()] = box.h;
                box.h              = room;
            }
        }
        box.y = std::max(box.y, top);
        if (box == target->position())
            continue;
        target->setPositionGlobal(box);
        // At once: moved by animation, a window would be seen under the bar
        // on its way out (a drag carrying it up there, every frame).
        target->warpPositionSize();
    }
}

static void init() {
    static auto destroy = Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF w) { g_refused.erase(w.get()); });
}

} // namespace keepclear
