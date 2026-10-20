#pragma once

#include "../desktop/DesktopTypes.hpp"

namespace Layout {
    class ITarget;
}

namespace Varuna {
    void init();
    void stop();
    bool onFullscreenRequest(const PHLWINDOW& window, bool on);
    bool onMaximizeRequest(const PHLWINDOW& window, bool on);
    void onDraggedOutOfTiling(const SP<Layout::ITarget>& target, const CBox& tiled);
}
