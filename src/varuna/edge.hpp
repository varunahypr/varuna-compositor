// Where two monitors meet, the pointer holds a moment before it crosses,
// like macOS: a short push keeps it on the screen it's on (aiming at
// something on the screen's edge), pushing on carries it over.
//
// While a window is being dragged it holds longer: that edge is then a
// screen edge to snap the window to (snap.hpp shows where it would go),
// and pushing through still takes it to the other monitor.
//
// Each pointer move that would land on another monitor is checked: until
// the pointer has pushed far enough past the edge (in logical px, counted
// from the moves that were held back), it's put back on the edge and the
// move is dropped. Leaving the edge, or resting a moment, starts over.
#pragma once

#include <chrono>

#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace edge {

    /// How far to push past the edge to cross: the pointer, a dragged window.
    static constexpr double PUSH = 28.0, PUSH_DRAGGING = 140.0;
    /// A rest this long at the edge starts the push over.
    static constexpr auto REST = std::chrono::milliseconds(500);

    /// Told after the pointer is held at an edge (the move that put it there
    /// was dropped, so other listeners don't see it): snapping's zone.
    static void (*g_held)() = nullptr;

    static Vector2D                              g_last{-1, -1};
    static double                                g_pushed = 0;
    static std::chrono::steady_clock::time_point g_lastPush{};

    static PHLMONITOR                            monitorAt(const Vector2D& p) {
        return State::monitorState()->query().vec(p).run();
    }

    /// `p` brought back inside `box` (by less than a pixel at its edge).
    static Vector2D inside(const Vector2D& p, const CBox& box) {
        return {std::clamp(p.x, box.x, box.x + box.w - 0.5), std::clamp(p.y, box.y, box.y + box.h - 0.5)};
    }

    static void onMove(Event::SCallbackInfo& info) {
        const auto& ptr = Pointer::mgr();
        if (!ptr)
            return;
        const Vector2D now = ptr->position();
        const Vector2D was = g_last;
        g_last             = now;
        if (was.x < 0)
            return;
        const auto from = monitorAt(was), to = monitorAt(now);
        if (!from || !to)
            return;
        if (from == to) {
            // Back away from the edge: the next crossing starts over.
            if (from->logicalBox().copy().expand(-3).containsPoint(now))
                g_pushed = 0;
            return;
        }
        const auto clock = std::chrono::steady_clock::now();
        if (clock - g_lastPush > REST)
            g_pushed = 0;
        g_lastPush          = clock;
        const auto&  dc     = g_layoutManager->dragController();
        const bool   drag   = dc && dc->target() && dc->mode() == MBIND_MOVE;
        const double needed = drag ? PUSH_DRAGGING : PUSH;
        g_pushed += now.distance(was);
        if (g_pushed >= needed) {
            g_pushed = 0;
            return;
        }
        // Held: back on the edge of the screen it was on.
        const Vector2D back = inside(now, from->logicalBox());
        ptr->warpTo(back);
        g_last         = back;
        info.cancelled = true;
        if (g_held)
            g_held();
    }

    static void init() {
        static auto move = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo& info) { onMove(info); });
    }

} // namespace edge
