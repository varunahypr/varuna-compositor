// Dragging a window keeps the spot you grabbed under the pointer.
//
// When a drag starts on a tiled window, Hyprland floats it, shrinks it, and
// centres it on the pointer, so the window jumps and the pointer ends up in
// its middle, wherever you grabbed it (the title bar, usually). Like macOS,
// keep the grab point instead: the same fraction across the window, and the
// same distance down from its top (clamped to the new size). The drag
// controller calls in once the window floats (onDraggedOutOfTiling).
#pragma once

#include <algorithm>

#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

namespace drag {

// Apps ask to be moved only once the pointer has travelled a little (GTK's
// drag threshold): the grab point is where the button went down.
static Vector2D g_press;

static void onDraggedOutOfTiling(const SP<Layout::ITarget>& target, const CBox& was) {
    if (!target || was.w <= 0 || was.h <= 0)
        return;
    const auto     pointer = g_pInputManager->getMouseCoordsInternal();
    const Vector2D grab    = was.containsPoint(g_press) ? g_press : pointer;
    const CBox     now     = target->position();
    const double   fx      = std::clamp((grab.x - was.x) / was.w, 0.0, 1.0);
    const double   dy      = std::clamp(grab.y - was.y, 0.0, std::max(0.0, now.h - 1.0));
    target->setPositionGlobal(CBox{pointer.x - fx * now.w, pointer.y - dy, now.w, now.h});
}

static void init() {
    static auto press = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
        if (e.state == WL_POINTER_BUTTON_STATE_PRESSED)
            g_press = g_pInputManager->getMouseCoordsInternal();
    });
}

} // namespace drag
