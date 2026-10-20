#pragma once

#include <format>
#include <optional>

#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace corners {

    static constexpr double REACH = 1.5, RELEASE = 10.0;

    struct SCorner {
        const char* id;
        bool        left, top;
    };

    static constexpr SCorner       CORNERS[] = {{"tl", true, true}, {"tr", false, true}, {"bl", true, false}, {"br", false, false}};

    static std::optional<Vector2D> g_armedAt;
    static Vector2D                g_lastSeen{-1, -1};
    static bool                    g_checkQueued = false;

    static PHLMONITOR              monitorAt(const Vector2D& p) {
        return State::monitorState()->query().vec(p).run();
    }

    static bool onAnyMonitor(const Vector2D& p) {
        const auto mon = monitorAt(p);
        return mon && mon->logicalBox().containsPoint(p);
    }

    static Vector2D pointOf(const CBox& box, const SCorner& c) {
        return {c.left ? box.x : box.x + box.w, c.top ? box.y : box.y + box.h};
    }

    static bool isOuterCorner(const Vector2D& at, const SCorner& c) {
        const double out_x = c.left ? -2 : 2, out_y = c.top ? -2 : 2;
        const double in_x = c.left ? 2 : -2, in_y = c.top ? 2 : -2;
        return !onAnyMonitor({at.x + out_x, at.y + in_y}) && !onAnyMonitor({at.x + in_x, at.y + out_y}) && !onAnyMonitor({at.x + out_x, at.y + out_y});
    }

    static bool busy() {
        if (g_pInputManager->hasHeldButtons())
            return true;
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
            return true;
        const auto& dc = g_layoutManager->dragController();
        return dc && dc->target();
    }

    static void check() {
        const Vector2D p = g_pInputManager->getMouseCoordsInternal();
        if (p == g_lastSeen)
            return;
        g_lastSeen = p;
        if (g_armedAt) {
            if (p.distance(*g_armedAt) <= RELEASE)
                return;
            g_armedAt.reset();
        }
        const auto mon = monitorAt(p);
        if (!mon || busy())
            return;
        const CBox box = mon->logicalBox();
        for (const auto& c : CORNERS) {
            const Vector2D at = pointOf(box, c);
            if (std::abs(p.x - at.x) > REACH || std::abs(p.y - at.y) > REACH || !isOuterCorner(at, c))
                continue;
            g_armedAt = at;
            g_pEventManager->postEvent(SHyprIPCEvent{.event = "varunacorner", .data = std::format("{},{},{}", mon->m_name, c.id, g_pInputManager->getModsFromAllKBs())});
            return;
        }
    }

    static void checkSoon() {
        if (g_checkQueued)
            return;
        g_checkQueued = true;
        g_pEventLoopManager->doLater([] {
            g_checkQueued = false;
            check();
        });
    }

    static void init() {
        static auto move  = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) { checkSoon(); });
        static auto frame = Event::bus()->m_events.render.pre.listen([](PHLMONITOR) { checkSoon(); });
    }

} // namespace corners
