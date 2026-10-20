#pragma once

#include "../desktop/DesktopTypes.hpp"

namespace Layout {
    class ITarget;
}

namespace Desktop::View {
    class CPopup;
}

namespace Varuna {
    void init();
    void stop();
    bool onFullscreenRequest(const PHLWINDOW& window, bool on);
    bool onMaximizeRequest(const PHLWINDOW& window, bool on);
    void onDraggedOutOfTiling(const SP<Layout::ITarget>& target, const CBox& tiled);
    bool keepClear(const PHLWINDOW& window, CBox& box);
    bool onFloatingDragMoved(const SP<Layout::ITarget>& target);
    void beforePopup(const SP<Desktop::View::CPopup>& popup, const PHLMONITOR& monitor);
}
