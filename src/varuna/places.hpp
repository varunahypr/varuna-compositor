#pragma once

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace places {

    static constexpr double MIN_W = 220, MIN_H = 160;
    static constexpr const char* SIZES_ITSELF_CLASS = "dev.varuna.Files";
    static constexpr const char* SIZES_ITSELF_TITLE = "Quick Look: ";

    static std::map<std::string, CBox> g_places;

    static std::string filePath() {
        const char* state = std::getenv("XDG_STATE_HOME");
        const char* home  = std::getenv("HOME");
        std::string base  = state && *state ? state : std::string(home ? home : "/tmp") + "/.local/state";
        return base + "/varuna/window-places.conf";
    }

    static void load() {
        g_places.clear();
        std::ifstream in(filePath());
        for (std::string line; std::getline(in, line);) {
            std::istringstream fields(line);
            double             x, y, w, h;
            std::string        cls;
            if (fields >> x >> y >> w >> h && std::getline(fields >> std::ws, cls) && !cls.empty())
                g_places[cls] = CBox{x, y, w, h};
        }
    }

    static bool sizesItself(const PHLWINDOW& w) {
        return w->m_class == SIZES_ITSELF_CLASS && w->m_title.starts_with(SIZES_ITSELF_TITLE);
    }

    static bool keepable(const PHLWINDOW& w) {
        return w && w->m_isMapped && w->m_isFloating && !sizesItself(w) && w->m_workspace && !w->m_workspace->m_isSpecialWorkspace &&
            !Fullscreen::controller()->isFullscreen(w) && !w->m_minimized;
    }

    static bool firstOfItsApp(const PHLWINDOW& w) {
        return std::ranges::none_of(Desktop::windowState()->windows(), [&](const PHLWINDOW& o) { return o && o != w && o->m_isMapped && o->m_class == w->m_class; });
    }

    static CBox workArea(const PHLMONITOR& mon) {
        return mon->logicalBoxMinusReserved();
    }

    static CBox target(const CBox& remembered, const CBox& area) {
        const double w = std::clamp(remembered.w, MIN_W, std::max(area.w, MIN_W));
        const double h = std::clamp(remembered.h, MIN_H, std::max(area.h, MIN_H));
        const CBox   there{remembered.x, remembered.y, w, h};
        const bool   onScreen = std::ranges::any_of(State::monitorState()->monitors(), [&](const PHLMONITOR& m) {
            if (!m)
                return false;
            const CBox a = workArea(m);
            return there.x >= a.x && there.y >= a.y && there.x + w <= a.x + a.w && there.y + h <= a.y + a.h;
        });
        if (onScreen)
            return there;
        return CBox{area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h};
    }

    static void onOpen(const PHLWINDOW& w) {
        if (!keepable(w) || !firstOfItsApp(w))
            return;
        const auto it = g_places.find(w->m_class);
        const auto mon = w->m_monitor.lock();
        const auto target_ = w->layoutTarget();
        if (it == g_places.end() || !mon || !target_)
            return;
        target_->setPositionGlobal(target(it->second, workArea(mon)));
    }

    static void onClose(const PHLWINDOW& w) {
        if (!keepable(w))
            return;
        const auto box = w->layoutTarget() ? w->layoutTarget()->position() : CBox{};
        if (box.w < MIN_W || box.h < MIN_H)
            return;
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "varunaclosedplace",
                                                 .data  = std::format("{:x},{},{},{},{},{}", reinterpret_cast<uintptr_t>(w.get()), (int)std::round(box.x),
                                                                      (int)std::round(box.y), (int)std::round(box.w), (int)std::round(box.h), w->m_class)});
    }

    static std::string command(std::istream& in) {
        std::string what;
        in >> what;
        if (what != "reload")
            return "usage: varunaglass places reload";
        load();
        return "ok";
    }

    static void init() {
        load();
        static auto open  = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { onOpen(w); });
        static auto close = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) { onClose(w); });
    }
}
