// Minimize like macOS's scale effect: the window shrinks smoothly into its
// Dock icon, and grows back out of it.
//
// Resizing the real window to get there (as the shell used to) makes the
// app lay itself out and redraw at every size, stalls at its minimum size
// and stretches while its buffers lag behind: clunky. Instead the window
// is photographed (the snapshot Hyprland's own close animation uses), the
// real window goes away at once, and the photo is drawn moving and
// shrinking, every frame, until it's in the icon. Restoring draws the same
// photo growing back to where the window lands, with the window itself
// held invisible until the photo gets there.
//
//   hyprctl varunaglass minimize ADDR x y w h   photograph, then shrink into
//                                               the box (layout px)
//   hyprctl varunaglass restore ADDR x y w h    grow from the box to the
//                                               window's place
//   hyprctl varunaglass snapinfo ADDR           geometry of a snapshot (debug)
//   hyprctl varunaglass where ADDR              where the window is drawn now (debug)
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <sstream>
#include <vector>

#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

namespace minimize {

using Clock = std::chrono::steady_clock;

constexpr float SHRINK_MS = 380.F;
constexpr float GROW_MS   = 340.F;

// A window's photo, and the box (layout px) the photo's framebuffer covers.
struct SPhoto {
    SP<Render::IFramebuffer> fb;
    CBox                     covers;
    // Where the window was in it, for scaling the photo around the window.
    CBox window;
};

struct SAnim {
    PHLWINDOWREF      window;
    SPhoto            photo;
    CBox              icon;
    bool              restore = false;
    Clock::time_point start;
};

static std::vector<SAnim>                                 g_anims;
static std::map<const Desktop::View::CWindow*, SPhoto>    g_kept;

static PHLWINDOW find(const std::string& addr) {
    uintptr_t want = 0;
    try {
        want = std::stoull(addr, nullptr, 16);
    } catch (...) { return nullptr; }
    for (const auto& w : Desktop::windowState()->windows())
        if (w && rc<uintptr_t>(w.get()) == want)
            return w;
    return nullptr;
}

// Where the window is drawn now, or where it's headed.
static CBox windowBox(const PHLWINDOW& w, bool goal) {
    return w->geometricBox(goal ? Desktop::View::IGeometric::GEOMETRIC_GOAL : Desktop::View::IGeometric::GEOMETRIC_CURRENT);
}

// The framebuffer Hyprland makes holds the window where it is on its
// monitor, monitor-sized (logical size = pixels / scale); a window-sized
// one is taken as covering just the window.
static std::optional<SPhoto> photograph(const PHLWINDOW& w) {
    const auto mon = w->m_monitor.lock();
    if (!mon || !w->m_isMapped)
        return std::nullopt;
    auto fb = g_pHyprRenderer->makeSnapshotFB(w);
    if (!fb || !fb->isAllocated() || !fb->getTexture())
        return std::nullopt;
    const CBox win = windowBox(w, false);
    const auto fbLogical = fb->m_size / mon->m_scale;
    const bool monitorSized = std::abs(fbLogical.x - mon->m_size.x) < 2 && std::abs(fbLogical.y - mon->m_size.y) < 2;
    const CBox covers = monitorSized ? CBox{mon->m_position.x, mon->m_position.y, mon->m_size.x, mon->m_size.y} : win;
    return SPhoto{fb, covers, win};
}

static float easeInOut(float t) {
    t = std::clamp(t, 0.F, 1.F);
    return t < 0.5F ? 4.F * t * t * t : 1.F - std::pow(-2.F * t + 2.F, 3.F) / 2.F;
}

static CBox lerp(const CBox& a, const CBox& b, float k) {
    return {a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.w + (b.w - a.w) * k, a.h + (b.h - a.h) * k};
}

// Where the window is this frame and how opaque, or nothing once done.
struct SFrame {
    CBox  box;
    float alpha;
    bool  done;
};

static SFrame frameOf(const SAnim& a, const PHLWINDOW& w) {
    const float ms = std::chrono::duration<float, std::milli>(Clock::now() - a.start).count();
    const float t  = std::clamp(ms / (a.restore ? GROW_MS : SHRINK_MS), 0.F, 1.F);
    const float k  = easeInOut(t);
    if (a.restore) {
        // Toward wherever the window is headed (it may still be tiling in).
        const CBox place = w ? windowBox(w, true) : a.photo.window;
        return {lerp(a.icon, place, k), std::clamp(t / 0.35F, 0.F, 1.F), t >= 1.F};
    }
    return {lerp(a.photo.window, a.icon, k), 1.F - std::clamp((t - 0.6F) / 0.4F, 0.F, 1.F), t >= 1.F};
}

// The photo drawn so its window lands on `box`: the whole framebuffer goes
// through the same scale-and-move that takes the window to `box`.
static void draw(const PHLMONITOR& mon, const SPhoto& photo, const CBox& box, float alpha) {
    if (photo.window.w <= 0 || photo.window.h <= 0 || alpha <= 0.01F)
        return;
    const double sx = box.w / photo.window.w, sy = box.h / photo.window.h;
    CBox target{box.x + (photo.covers.x - photo.window.x) * sx, box.y + (photo.covers.y - photo.window.y) * sy, photo.covers.w * sx,
                photo.covers.h * sy};
    target.translate(-mon->m_position).scale(mon->m_scale).round();
    CTexPassElement::SRenderData data;
    data.tex = photo.fb->getTexture();
    data.box = target;
    data.a   = alpha;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(data)));
}

static void setHidden(const PHLWINDOW& w, bool hidden) {
    if (w)
        w->alpha(Desktop::View::WINDOW_ALPHA_FADE)->setValueAndWarp(hidden ? 0.F : 1.F);
}

// ------------------------------------------------------------------ hooks --

// Before each frame: keep frames coming while anything moves, and finish
// what's done.
static void onRenderPre(const PHLMONITOR& mon) {
    for (auto it = g_anims.begin(); it != g_anims.end();) {
        const auto w = it->window.lock();
        const auto f = frameOf(*it, w);
        if (f.done) {
            if (it->restore) {
                setHidden(w, false);
                if (w)
                    g_kept.erase(w.get());
            }
            g_pHyprRenderer->damageMonitor(mon);
            it = g_anims.erase(it);
            continue;
        }
        g_pHyprRenderer->damageMonitor(mon);
        ++it;
    }
}

static void onRenderStage(const PHLMONITOR& mon) {
    const CBox screen{mon->m_position.x, mon->m_position.y, mon->m_size.x, mon->m_size.y};
    for (const auto& a : g_anims) {
        const auto w = a.window.lock();
        const auto f = frameOf(a, w);
        if (!f.done && !f.box.copy().intersection(screen).empty())
            draw(mon, a.photo, f.box, f.alpha);
    }
}

static void forget(const Desktop::View::CWindow* w) {
    g_kept.erase(w);
    std::erase_if(g_anims, [w](const SAnim& a) { return a.window.expired() || a.window.lock().get() == w; });
}

// ---------------------------------------------------------------- command --

static std::string command(const std::string& sub, std::istringstream& in) {
    std::string addr;
    in >> addr;
    const auto w = find(addr);
    if (!w)
        return "no such window";
    // Where the window is drawn right now, mid-animation (debug).
    if (sub == "where") {
        const CBox b = windowBox(w, false);
        return std::format("{} {} {} {}", b.x, b.y, b.w, b.h);
    }
    if (sub == "snapinfo") {
        const auto p = photograph(w);
        if (!p)
            return "no snapshot";
        const auto mon = w->m_monitor.lock();
        return std::format("fb {}x{} px, covers {} {} {}x{}, window {} {} {}x{}, monitor scale {}", p->fb->m_size.x, p->fb->m_size.y, p->covers.x,
                           p->covers.y, p->covers.w, p->covers.h, p->window.x, p->window.y, p->window.w, p->window.h, mon ? mon->m_scale : 0.F);
    }
    double x = 0, y = 0, bw = 0, bh = 0;
    if (!(in >> x >> y >> bw >> bh) || bw <= 0 || bh <= 0)
        return "usage: varunaglass minimize|restore ADDR x y w h";
    const CBox icon{x, y, bw, bh};
    // Anything already running for this window gives way.
    std::erase_if(g_anims, [&](const SAnim& a) { return a.window.lock() == w; });
    if (sub == "minimize") {
        const auto p = photograph(w);
        if (!p)
            return "no snapshot";
        g_kept[w.get()] = *p;
        g_anims.push_back({w, *p, icon, false, Clock::now()});
    } else {
        const auto kept = g_kept.find(w.get());
        if (kept == g_kept.end())
            return "no snapshot";
        setHidden(w, true);
        g_anims.push_back({w, kept->second, icon, true, Clock::now()});
    }
    if (const auto mon = w->m_monitor.lock())
        g_pHyprRenderer->damageMonitor(mon);
    return "ok";
}

static void stop() {
    for (const auto& a : g_anims)
        if (a.restore)
            setHidden(a.window.lock(), false);
    g_anims.clear();
    g_kept.clear();
}

} // namespace minimize
