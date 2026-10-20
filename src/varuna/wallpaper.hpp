#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <hyprland/src/helpers/MainLoopExecutor.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/AsyncResourceGatherer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace wallpaper {

    using Clock = std::chrono::steady_clock;

    static constexpr float       FADE_MS     = 450.F;
    static constexpr const char* ANY_DISPLAY = "*";

    struct SPaper {
        std::string          path;
        SP<Render::ITexture> shown;
        SP<Render::ITexture> leaving;
        Clock::time_point    since;
    };

    struct SLoad {
        std::string                       path;
        ASP<Hyprgraphics::CImageResource> image;
    };

    static std::map<std::string, std::string> g_chosen;
    static std::map<std::string, SPaper>      g_papers;
    static std::map<std::string, SLoad>       g_loading;

    static std::string                        listPath() {
        const char* data = std::getenv("XDG_DATA_HOME");
        const char* home = std::getenv("HOME");
        std::string base = data && *data ? data : std::string(home ? home : "/tmp") + "/.local/share";
        return base + "/varuna/wallpapers.conf";
    }

    static std::map<std::string, std::string> readList() {
        std::map<std::string, std::string> chosen;
        std::ifstream                      in(listPath());
        for (std::string line; std::getline(in, line);) {
            if (line.empty() || line.front() == '#')
                continue;
            std::istringstream fields(line);
            std::string        display, path;
            fields >> display;
            std::getline(fields >> std::ws, path);
            if (!display.empty() && !path.empty())
                chosen[display] = path;
        }
        return chosen;
    }

    static std::string chosenFor(const std::string& display) {
        if (const auto it = g_chosen.find(display); it != g_chosen.end())
            return it->second;
        if (const auto it = g_chosen.find(ANY_DISPLAY); it != g_chosen.end())
            return it->second;
        return "";
    }

    static void damage(const std::string& display) {
        for (const auto& m : State::monitorState()->monitors())
            if (m && m->m_name == display)
                g_pHyprRenderer->damageMonitor(m);
    }

    static void show(const std::string& display, SP<Render::ITexture> texture) {
        auto& paper   = g_papers[display];
        paper.leaving = paper.shown;
        paper.shown   = std::move(texture);
        paper.since   = Clock::now();
        damage(display);
    }

    static void arrived(const std::string& display, const std::string& path) {
        const auto it = g_loading.find(display);
        if (it == g_loading.end() || it->second.path != path)
            return;
        const auto image = it->second.image;
        g_pEventLoopManager->doLater([display, image] {
            if (const auto it = g_loading.find(display); it != g_loading.end() && it->second.image == image)
                g_loading.erase(it);
        });
        if (g_papers[display].path != path)
            return;
        const auto& surface = image->m_asset.cairoSurface;
        if (!surface || !surface->cairo()) {
            Log::logger->log(Log::ERR, "varuna wallpaper: can't read {}", path);
            return;
        }
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        auto texture = g_pHyprRenderer->createTexture(surface->cairo());
        if (!texture || !texture->ok()) {
            Log::logger->log(Log::ERR, "varuna wallpaper: no texture for {}", path);
            return;
        }
        Log::logger->log(Log::DEBUG, "varuna wallpaper: {} on {}", path, display);
        show(display, std::move(texture));
    }

    static void load(const std::string& display, const std::string& path) {
        auto image = makeAtomicShared<Hyprgraphics::CImageResource>(path);
        auto ready = makeShared<CMainLoopExecutor>([display, path] { arrived(display, path); });
        image->m_events.finished.listenStatic([ready] { ready->signal(); });
        g_loading[display] = SLoad{path, image};
        g_pAsyncResourceGatherer->enqueue(image);
    }

    static void follow(const std::string& display) {
        const auto want  = chosenFor(display);
        auto&      paper = g_papers[display];
        if (paper.path == want)
            return;
        paper.path = want;
        if (want.empty())
            show(display, nullptr);
        else
            load(display, want);
    }

    static CBox cover(const PHLMONITOR& monitor, const SP<Render::ITexture>& texture) {
        const auto   screen = monitor->m_transformedSize;
        const auto   size   = texture->m_size;
        const double scale  = std::max(screen.x / size.x, screen.y / size.y);
        const auto   drawn  = size * scale;
        return CBox{(screen - drawn) / 2.0, drawn};
    }

    static void paint(const PHLMONITOR& monitor, const SP<Render::ITexture>& texture, float alpha) {
        if (!texture || alpha <= 0.F)
            return;
        CTexPassElement::SRenderData data;
        data.tex = texture;
        data.box = cover(monitor, texture);
        data.a   = alpha;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(data)));
    }

    static bool draw(const PHLMONITOR& monitor) {
        follow(monitor->m_name);
        auto& paper = g_papers[monitor->m_name];
        if (!paper.shown && !paper.leaving)
            return false;
        const float ms       = std::chrono::duration<float, std::milli>(Clock::now() - paper.since).count();
        const float progress = std::clamp(ms / FADE_MS, 0.F, 1.F);
        if (progress >= 1.F)
            paper.leaving = nullptr;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CClearPassElement>(CClearPassElement::SClearData{CHyprColor(0, 0, 0, 1)}));
        paint(monitor, paper.leaving, paper.shown ? 1.F : 1.F - progress);
        paint(monitor, paper.shown, paper.leaving ? progress : 1.F);
        if (paper.leaving)
            g_pEventLoopManager->doLater([weak = PHLMONITORREF(monitor)] {
                if (const auto m = weak.lock())
                    g_pHyprRenderer->damageMonitor(m);
            });
        return true;
    }

    static void reload() {
        g_chosen = readList();
        for (auto& [display, paper] : g_papers)
            paper.path.clear();
        for (const auto& m : State::monitorState()->monitors())
            if (m)
                g_pHyprRenderer->damageMonitor(m);
    }

    static std::string command(std::istream& in) {
        std::string what;
        in >> what;
        if (what != "reload")
            return "usage: varunaglass wallpaper reload";
        reload();
        return "ok";
    }

    static void init() {
        g_chosen = readList();
    }

    static void stop() {
        g_loading.clear();
        g_papers.clear();
    }
}
