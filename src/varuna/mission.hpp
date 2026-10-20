#pragma once

#include <cstdio>
#include <format>
#include <istream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include "wallpaper.hpp"

namespace mission {

    static constexpr const char* WALL = "wall:";

    struct SPreview {
        std::string address;
        CBox        box;
        float       alpha  = 1.F;
        std::string wallOf;
        float       radius = 0.F;
    };

    struct SScene {
        float                 fade = 0.F;
        CHyprColor            scrim;
        std::vector<SPreview> previews;
    };

    static std::map<std::string, SScene> g_scenes;

    static void (*g_drawBackdrop)(const PHLMONITOR& mon, float opacity) = nullptr;

    static std::string addressOf(const PHLWINDOW& w) {
        return std::format("0x{:x}", reinterpret_cast<uintptr_t>(w.get()));
    }

    static PHLWINDOW windowAt(const std::string& address) {
        for (const auto& w : Desktop::windowState()->windows())
            if (w && addressOf(w) == address)
                return w;
        return nullptr;
    }

    static const SScene* sceneFor(const PHLLS& ls, const PHLMONITOR& mon) {
        if (const auto it = g_scenes.find(ls->m_namespace + "@" + mon->m_name); it != g_scenes.end())
            return &it->second;
        if (const auto it = g_scenes.find(ls->m_namespace); it != g_scenes.end())
            return &it->second;
        return nullptr;
    }

    static void damageAll() {
        for (const auto& m : State::monitorState()->monitors())
            if (m)
                g_pHyprRenderer->damageMonitor(m);
    }

    static std::vector<SPreview> parsePreviews(const std::string& list) {
        std::vector<SPreview> previews;
        std::istringstream    items(list);
        for (std::string item; std::getline(items, item, ';');) {
            char   what[64] = {};
            double x = 0, y = 0, w = 0, h = 0;
            float  alpha = 1.F, radius = 0.F;
            if (std::sscanf(item.c_str(), " %63[^,],%lf,%lf,%lf,%lf,%f,%f", what, &x, &y, &w, &h, &alpha, &radius) < 5 || w <= 1 || h <= 1)
                continue;
            const std::string name = what;
            if (name.starts_with(WALL))
                previews.push_back({"", CBox{x, y, w, h}, alpha, name.substr(std::string(WALL).size()), radius});
            else
                previews.push_back({name, CBox{x, y, w, h}, alpha});
        }
        return previews;
    }

    static std::string command(std::istream& in) {
        std::string key, fade, scrim, list;
        in >> key >> fade;
        if (key.empty())
            return "usage: varunaglass mission <namespace>[@monitor] <fade 0-1>|off [rrggbbaa] [address|wall:<monitor>,x,y,w,h,alpha[,radius];...]";
        if (fade == "off" || fade.empty()) {
            g_scenes.erase(key);
            damageAll();
            return "ok";
        }
        in >> scrim;
        std::getline(in >> std::ws, list);
        SScene scene;
        scene.fade = std::clamp(std::stof(fade), 0.F, 1.F);
        unsigned rgba = 0;
        if (std::sscanf(scrim.c_str(), "%8x", &rgba) == 1)
            scene.scrim = CHyprColor(((rgba >> 24) & 0xff) / 255.F, ((rgba >> 16) & 0xff) / 255.F, ((rgba >> 8) & 0xff) / 255.F, (rgba & 0xff) / 255.F);
        scene.previews = parsePreviews(list);
        g_scenes[key]  = std::move(scene);
        damageAll();
        return "ok";
    }

    static void drawScrim(const PHLMONITOR& mon, const SScene& scene) {
        if (scene.fade <= 0.F)
            return;
        if (g_drawBackdrop)
            g_drawBackdrop(mon, scene.fade);
        CRectPassElement::SRectData data;
        data.box   = {0, 0, mon->m_transformedSize.x, mon->m_transformedSize.y};
        data.color = scene.scrim.modifyA(scene.scrim.a * scene.fade);
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
    }

    static void withAlpha(const PHLWINDOW& w, float alpha, const std::function<void()>& draw) {
        const auto& fade = w->alpha(Desktop::View::WINDOW_ALPHA_FADE);
        if (alpha >= 1.F || fade->isBeingAnimated()) {
            draw();
            return;
        }
        const float was = fade->value();
        fade->setValueAndWarp(was * alpha);
        draw();
        fade->setValueAndWarp(was);
    }

    static void drawWall(const PHLLS& ls, const PHLMONITOR& mon, const SPreview& p) {
        const auto it = wallpaper::g_papers.find(p.wallOf);
        if (it == wallpaper::g_papers.end() || !it->second.shown)
            return;
        CTexPassElement::SRenderData data;
        data.tex   = it->second.shown;
        data.box   = CBox{(ls->m_geometry.pos() + p.box.pos() - mon->m_position) * mon->m_scale, p.box.size() * mon->m_scale};
        data.a     = p.alpha;
        data.round = sc<int>(std::round(p.radius * mon->m_scale));
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(data)));
    }

    static void drawPreview(const PHLLS& ls, const PHLMONITOR& mon, const SPreview& p) {
        if (!p.wallOf.empty()) {
            drawWall(ls, mon, p);
            return;
        }
        const auto w = windowAt(p.address);
        if (!w || !w->m_isMapped || p.alpha <= 0.01F)
            return;
        const auto size = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        if (size.x < 1 || size.y < 1)
            return;
        const Vector2D at     = (ls->m_geometry.pos() + p.box.pos() - mon->m_position) * mon->m_scale;
        const float    scale  = sc<float>(p.box.w / size.x);
        Render::SRenderModifData hints;
        hints.modifs.emplace_back(Render::SRenderModifData::eRenderModifType::RMOD_TYPE_TRANSLATE, at / scale);
        hints.modifs.emplace_back(Render::SRenderModifData::eRenderModifType::RMOD_TYPE_SCALE, scale);
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{hints}));
        const bool     onShownDesktop = w->m_workspace && w->m_workspace->isVisible() && !w->isHidden();
        const Vector2D nudged         = std::exchange(w->m_floatingOffset, Vector2D{});
        if (onShownDesktop)
            withAlpha(w, p.alpha, [&] { g_pHyprRenderer->renderWindowAtOrigin(w, mon, false); });
        else
            g_pHyprRenderer->renderWindowAtOrigin(w, mon, true);
        w->m_floatingOffset = nudged;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{Render::SRenderModifData{}}));
    }

    static void beforeLayer(const PHLLS& ls, const PHLMONITOR& mon) {
        if (!ls || !mon || g_scenes.empty())
            return;
        const auto scene = sceneFor(ls, mon);
        if (!scene)
            return;
        drawScrim(mon, *scene);
        for (const auto& p : scene->previews)
            drawPreview(ls, mon, p);
        g_pEventLoopManager->doLater([weak = PHLMONITORREF(mon)] {
            if (const auto m = weak.lock())
                g_pHyprRenderer->damageMonitor(m);
        });
    }

    static void stop() {
        g_scenes.clear();
    }
}
