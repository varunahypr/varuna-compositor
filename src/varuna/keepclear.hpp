// No window goes under the top bar.
//
// Tiled windows already stay inside the monitor's reserved area (the bar's
// exclusive zone), but floating ones don't: a dialog taller than the room
// left is centred on the whole monitor, apps can place their windows where
// they like, and a drag could carry one up behind the bar. Like macOS's menu
// bar, the bar is off limits: wherever a floating window is put
// (CWindowTarget::updatePos calls keepClear), one whose top (border
// included) would be above the reserved area's goes down to it, and one too
// tall for the room below the bar is shortened to fit. When the reserved
// area itself changes (the bar comes or goes), the floating windows there
// are put again.
//
// Fullscreen windows are left alone: varuna's full screen hides the bar and
// pushes the window down itself when the bar slides in.
#pragma once

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

namespace keepclear {

// Heights we asked windows to shrink from. An app with a minimum size may
// keep its height anyway; then don't ask again.
static std::unordered_map<const Desktop::View::CWindow*, double> g_refused;
static std::unordered_map<MONITORID, CBox>                       g_areas;

static bool keepClear(const PHLWINDOW& w, CBox& box) {
    if (!w || w->isX11OverrideRedirect())
        return false;
    const auto mon = w->m_monitor.lock();
    if (!mon)
        return false;
    const CBox   area   = mon->logicalBoxMinusReserved();
    const auto   deco   = w->getFullWindowReservedArea();
    const double top    = area.y + deco.topLeft.y;
    const double bottom = mon->logicalBox().y + mon->logicalBox().h - deco.bottomRight.y;
    const double room   = bottom - top;
    const CBox   was    = box;
    if (box.h > room + 0.5) {
        const auto it = g_refused.find(w.get());
        if (it == g_refused.end() || std::abs(it->second - box.h) > 0.5) {
            g_refused[w.get()] = box.h;
            box.h              = room;
        }
    }
    box.y = std::max(box.y, top);
    return box != was;
}

static void onRenderPre(const PHLMONITOR& mon) {
    const CBox area = mon->logicalBoxMinusReserved();
    const auto it   = g_areas.find(mon->m_id);
    if (it != g_areas.end() && it->second == area)
        return;
    g_areas[mon->m_id] = area;
    for (const auto& w : Desktop::windowState()->windows()) {
        if (!w || !w->m_isMapped || !w->m_isFloating || w->m_monitor.lock() != mon || Fullscreen::controller()->isFullscreen(w))
            continue;
        if (const auto target = w->layoutTarget())
            target->setPositionGlobal(target->position());
    }
}

static void init() {
    static auto destroy = Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF w) { g_refused.erase(w.get()); });
    static auto gone    = Event::bus()->m_events.monitor.destroyMon.listen([](PHLMONITOR m) { g_areas.erase(m->m_id); });
}

} // namespace keepclear
