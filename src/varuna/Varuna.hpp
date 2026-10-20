#pragma once

#include <string>

#include "../desktop/DesktopTypes.hpp"

namespace Layout {
    class ITarget;
}

namespace Desktop::View {
    class CPopup;
}

namespace Varuna {
    void        init();
    void        stop();
    std::string features();
    bool        drawWallpaper(const PHLMONITOR& monitor);
    void        beforeLayer(const PHLLS& layer, const PHLMONITOR& monitor);
    bool        onFullscreenRequest(const PHLWINDOW& window, bool on);
    bool        onMaximizeRequest(const PHLWINDOW& window, bool on);
    void        onDraggedOutOfTiling(const SP<Layout::ITarget>& target, const CBox& tiled);
    bool        keepClear(const PHLWINDOW& window, CBox& box);
    bool        onFloatingDragMoved(const SP<Layout::ITarget>& target);
    void        beforePopup(const SP<Desktop::View::CPopup>& popup, const PHLMONITOR& monitor);
}
