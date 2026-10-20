#pragma once

#include "../desktop/DesktopTypes.hpp"

namespace Varuna {
    void init();
    void stop();
    bool onFullscreenRequest(const PHLWINDOW& window, bool on);
    bool onMaximizeRequest(const PHLWINDOW& window, bool on);
}
