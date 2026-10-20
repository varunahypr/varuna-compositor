#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <istream>
#include <string>
#include <vector>

#include <linux/input-event-codes.h>

#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace showdesktop {

    using Clock = std::chrono::steady_clock;

    static constexpr double SLIVER   = 15.0;
    static constexpr float  SLIDE_MS = 300.F;

    struct SAside {
        PHLWINDOWREF window;
        Vector2D     by;
    };

    static std::vector<SAside> g_aside;
    static bool                g_out = false;
    static Clock::time_point   g_since;

    static bool overlaps(const CBox& a, const CBox& b) {
        return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
    }

    static Vector2D asideOf(const CBox& window, const CBox& screen, const std::vector<CBox>& others) {
        const double   x = std::clamp(window.x, screen.x, std::max(screen.x, screen.x + screen.w - 2 * SLIVER));
        const double   y = std::clamp(window.y, screen.y, std::max(screen.y, screen.y + screen.h - 2 * SLIVER));
        const Vector2D left{screen.x + SLIVER - window.w, y};
        const Vector2D right{screen.x + screen.w - SLIVER, y};
        const Vector2D down{x, screen.y + screen.h - SLIVER};
        const bool     leftIsNearer = window.x + window.w / 2 < screen.x + screen.w / 2;
        const Vector2D near         = leftIsNearer ? left : right;
        const Vector2D far          = leftIsNearer ? right : left;
        for (const auto& to : {near, far, down})
            if (std::ranges::none_of(others, [&](const CBox& o) { return overlaps(CBox{to, window.size()}, o); }))
                return to;
        return near;
    }

    static float progress() {
        const float ms = std::chrono::duration<float, std::milli>(Clock::now() - g_since).count();
        const float t  = std::clamp(ms / SLIDE_MS, 0.F, 1.F);
        return 1.F - std::pow(1.F - t, 3.F);
    }

    static void announce(const char* what) {
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "varunadesktop", .data = what});
    }

    static void damageAll() {
        for (const auto& m : State::monitorState()->monitors())
            if (m)
                g_pHyprRenderer->damageMonitor(m);
    }

    static void show() {
        if (g_out)
            return;
        g_aside.clear();
        std::vector<CBox> screens;
        for (const auto& m : State::monitorState()->monitors())
            if (m)
                screens.push_back(m->logicalBox());
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w || !w->m_isMapped || w->isHidden() || w->m_minimized || w->m_pinned || !w->m_workspace || w->m_workspace->m_isSpecialWorkspace)
                continue;
            const auto mon = w->m_monitor.lock();
            if (!mon || mon->m_activeWorkspace != w->m_workspace)
                continue;
            const CBox        box    = w->geometricBox(Desktop::View::IGeometric::GEOMETRIC_GOAL);
            const CBox        screen = mon->logicalBox();
            std::vector<CBox> others;
            std::ranges::copy_if(screens, std::back_inserter(others), [&](const CBox& s) { return s != screen; });
            g_aside.push_back({w, asideOf(box, screen, others) - box.pos()});
            w->setInputBlocked(Desktop::View::INPUT_BLOCK_SET_ASIDE, true);
        }
        if (g_aside.empty())
            return;
        g_out   = true;
        g_since = Clock::now();
        announce("shown");
        damageAll();
    }

    static void hide() {
        if (!g_out)
            return;
        g_out   = false;
        g_since = Clock::now();
        for (const auto& a : g_aside)
            if (const auto w = a.window.lock())
                w->setInputBlocked(Desktop::View::INPUT_BLOCK_SET_ASIDE, false);
        announce("back");
        damageAll();
    }

    static void onRenderPre(const PHLMONITOR& mon) {
        if (g_aside.empty())
            return;
        const float p     = progress();
        const float share = g_out ? p : 1.F - p;
        for (const auto& a : g_aside)
            if (const auto w = a.window.lock())
                w->m_floatingOffset = a.by * share;
        g_pHyprRenderer->damageMonitor(mon);
        if (p >= 1.F && !g_out) {
            for (const auto& a : g_aside)
                if (const auto w = a.window.lock())
                    w->m_floatingOffset = {};
            g_aside.clear();
        }
    }

    static bool onSliver(const Vector2D& at) {
        return std::ranges::any_of(g_aside, [&](const SAside& a) {
            const auto w = a.window.lock();
            return w && w->geometricBox(Desktop::View::IGeometric::GEOMETRIC_GOAL).translate(a.by).containsPoint(at);
        });
    }

    static void onButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        if (!g_out || e.state != WL_POINTER_BUTTON_STATE_PRESSED || !onSliver(g_pInputManager->getMouseCoordsInternal()))
            return;
        info.cancelled = true;
        hide();
    }

    static std::string command(std::istream& in) {
        std::string what;
        in >> what;
        if (what == "show")
            show();
        else if (what == "hide")
            hide();
        else if (what == "toggle")
            g_out ? hide() : show();
        else if (what != "state")
            return "usage: varunaglass desktop show|hide|toggle|state";
        return g_out ? "shown" : "back";
    }

    static void init() {
        static auto button = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onButton(e, info); });
        static auto closed = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) {
            std::erase_if(g_aside, [&](const SAside& a) { return a.window.expired() || a.window.lock() == w; });
            if (w)
                w->m_floatingOffset = {};
        });
    }

    static void stop() {
        for (const auto& a : g_aside)
            if (const auto w = a.window.lock()) {
                w->setInputBlocked(Desktop::View::INPUT_BLOCK_SET_ASIDE, false);
                w->m_floatingOffset = {};
            }
        g_aside.clear();
        g_out = false;
    }
}
