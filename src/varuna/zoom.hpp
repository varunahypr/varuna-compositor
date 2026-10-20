#pragma once

#include <map>

#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

namespace zoom {

static std::map<const Desktop::View::CWindow*, CBox> g_normal, g_full;

static CBox goalBox(const PHLWINDOW& w) {
    return w->geometricBox(Desktop::View::IGeometric::GEOMETRIC_GOAL);
}

static bool settled(const PHLWINDOW& w) {
    return w->m_isMapped && !w->isHidden() && !w->m_minimized && !w->positionAnimation()->isBeingAnimated() && !w->sizeAnimation()->isBeingAnimated();
}

static void remember() {
    for (const auto& w : Desktop::windowState()->windows()) {
        if (!w || !settled(w))
            continue;
        (Fullscreen::controller()->isFullscreen(w) ? g_full : g_normal)[w.get()] = goalBox(w);
    }
}

static void growFrom(const PHLWINDOWREF& ref, const CBox& from) {
    const auto w = ref.lock();
    if (!w || !w->m_isMapped || w->isHidden() || w->m_minimized)
        return;
    const CBox to = goalBox(w);
    if (to.w <= 0 || to.h <= 0 || (to.pos() == from.pos() && to.size() == from.size()))
        return;
    w->positionAnimation()->setValue(from.pos());
    w->sizeAnimation()->setValue(from.size());
}

static void onFullscreen(const PHLWINDOW& w) {
    if (!w)
        return;
    const auto& was  = Fullscreen::controller()->isFullscreen(w) ? g_normal : g_full;
    const auto  from = was.find(w.get());
    if (from == was.end())
        return;
    g_pEventLoopManager->doLater([ref = PHLWINDOWREF{w}, box = from->second] { growFrom(ref, box); });
}

static void forget(const Desktop::View::CWindow* w) {
    g_normal.erase(w);
    g_full.erase(w);
}

static void init() {
    static auto frame = Event::bus()->m_events.render.pre.listen([](PHLMONITOR) { remember(); });
    static auto full  = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW w) { onFullscreen(w); });
}

static void stop() {
    g_normal.clear();
    g_full.clear();
}

} // namespace zoom
