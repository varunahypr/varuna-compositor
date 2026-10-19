// The pointer comes back to what's under it when a menu closes.
//
// Hyprland moves the pointer onto a menu (an xdg popup) while it's open,
// and when the menu goes it tells the app the pointer left, but doesn't say
// it's back over the surface underneath until the mouse moves. On varuna's
// desktop (a layer surface) that meant a right-click on the spot where the
// last menu closed went nowhere: no menu, until you nudged the mouse. So
// once a popup is gone, look again at what's under the pointer, as if it
// had moved.
#pragma once

#include <hyprland/src/desktop/view/View.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

namespace popups {

static void init() {
    static auto destroy = Event::bus()->m_events.view.destroy.listen([](Event::SViewDestroyEvent e) {
        if (e.type != Desktop::View::VIEW_TYPE_POPUP)
            return;
        // After the popup is fully gone: refocusing from inside its
        // teardown would find it still there.
        g_pEventLoopManager->doLater([] { g_pInputManager->simulateMouseMovement(); });
    });
}

} // namespace popups
