#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <hyprland/protocols/varuna-glass-v1.hpp>
#include <hyprland/src/desktop/state/LayerState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/protocols/WaylandProtocol.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>

namespace glass_protocol {

    static constexpr size_t FIELDS_PER_PIECE = 8;
    static constexpr double SUBPIXELS        = 256.0;
    static constexpr double STRETCH_UNITS    = 1000.0;

    struct SPiece {
        double x, y, w, h;
        float  radius;
        int    tint;
        float  sx, sy;
    };

    using Apply = std::function<void(const std::string& key, uint8_t kind, bool lifted, std::vector<SPiece> pieces)>;
    static Apply               g_apply;

    static std::vector<SPiece> piecesFrom(wl_array* array) {
        std::vector<SPiece> pieces;
        if (!array)
            return pieces;
        const auto*  values = static_cast<const int32_t*>(array->data);
        const size_t count  = array->size / sizeof(int32_t) / FIELDS_PER_PIECE;
        for (size_t i = 0; i < count; i++) {
            const int32_t* v = values + i * FIELDS_PER_PIECE;
            pieces.push_back({v[0] / SUBPIXELS, v[1] / SUBPIXELS, v[2] / SUBPIXELS, v[3] / SUBPIXELS, sc<float>(v[4] / SUBPIXELS), v[5], sc<float>(v[6] / STRETCH_UNITS),
                              sc<float>(v[7] / STRETCH_UNITS)});
        }
        return pieces;
    }

    static std::string keyOf(const SP<CWLSurfaceResource>& surface) {
        for (const auto& ls : Desktop::layerState()->layers()) {
            if (!ls || !ls->wlSurface() || ls->wlSurface()->resource() != surface)
                continue;
            const auto mon = ls->m_monitor.lock();
            return mon ? ls->m_namespace + "@" + mon->m_name : ls->m_namespace;
        }
        return "";
    }

    class CGlass {
      public:
        CGlass(UP<CVarunaGlassV1>&& resource, SP<CWLSurfaceResource> surface) : m_resource(std::move(resource)), m_surface(surface) {
            if (!good())
                return;
            m_resource->setDestroy([this](CVarunaGlassV1*) { forget(); });
            m_resource->setOnDestroy([this](CVarunaGlassV1*) { forget(); });
            m_resource->setSetLook([this](CVarunaGlassV1*, uint32_t kind, uint32_t lifted) {
                m_pending.kind   = sc<uint8_t>(kind);
                m_pending.lifted = lifted != 0;
                m_dirty          = true;
            });
            m_resource->setSetPieces([this](CVarunaGlassV1*, wl_array* pieces) {
                m_pending.pieces = piecesFrom(pieces);
                m_dirty          = true;
            });
            m_committed = surface->m_events.commit.listen([this] { apply(); });
            m_gone      = surface->m_events.destroy.listen([this] { clear(); });
        }

        ~CGlass() {
            clear();
        }

        bool good() const {
            return m_resource && m_resource->resource();
        }

        bool isFor(const SP<CWLSurfaceResource>& surface) const {
            return m_surface.lock() == surface;
        }

        std::function<void(CGlass*)> m_onGone;

      private:
        struct SState {
            uint8_t             kind   = 0;
            bool                lifted = false;
            std::vector<SPiece> pieces;
        };

        UP<CVarunaGlassV1>     m_resource;
        WP<CWLSurfaceResource> m_surface;
        SState                 m_pending;
        bool                   m_dirty = false;
        std::string            m_key;
        CHyprSignalListener    m_committed;
        CHyprSignalListener    m_gone;

        void                   apply() {
            const auto surface = m_surface.lock();
            if (!surface || !g_apply)
                return;
            const auto key = keyOf(surface);
            if (key != m_key && !m_key.empty())
                g_apply(m_key, 0, false, {});
            if (key.empty())
                return;
            if (!m_dirty && key == m_key)
                return;
            m_key   = key;
            m_dirty = false;
            g_apply(key, m_pending.kind, m_pending.lifted, m_pending.pieces);
        }

        void clear() {
            if (!m_key.empty() && g_apply)
                g_apply(m_key, 0, false, {});
            m_key.clear();
        }

        void forget() {
            clear();
            if (m_onGone)
                m_onGone(this);
        }
    };

    class CProtocol : public IWaylandProtocol {
      public:
        CProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {}

        void bindManager(wl_client* client, void*, uint32_t ver, uint32_t id) override {
            const auto manager = m_managers.emplace_back(makeUnique<CVarunaGlassManagerV1>(client, ver, id)).get();
            manager->setOnDestroy([this](CVarunaGlassManagerV1* m) { forgetManager(m); });
            manager->setDestroy([this](CVarunaGlassManagerV1* m) { forgetManager(m); });
            manager->setGetGlass([this](CVarunaGlassManagerV1* m, uint32_t id, wl_resource* surface) { makeGlass(m, id, CWLSurfaceResource::fromResource(surface)); });
        }

      private:
        std::vector<UP<CVarunaGlassManagerV1>> m_managers;
        std::vector<UP<CGlass>>                m_glasses;

        void                                   forgetManager(CVarunaGlassManagerV1* manager) {
            std::erase_if(m_managers, [manager](const auto& m) { return m.get() == manager; });
        }

        void makeGlass(CVarunaGlassManagerV1* manager, uint32_t id, SP<CWLSurfaceResource> surface) {
            if (std::ranges::any_of(m_glasses, [&](const auto& g) { return g->isFor(surface); })) {
                manager->error(VARUNA_GLASS_MANAGER_V1_ERROR_ALREADY_HAS_GLASS, "the surface already has glass");
                return;
            }
            auto glass = makeUnique<CGlass>(makeUnique<CVarunaGlassV1>(manager->client(), manager->version(), id), surface);
            if (!glass->good()) {
                manager->noMemory();
                return;
            }
            glass->m_onGone = [this](CGlass* gone) { g_pEventLoopManager->doLater([this, gone] { std::erase_if(m_glasses, [gone](const auto& g) { return g.get() == gone; }); }); };
            m_glasses.emplace_back(std::move(glass));
        }
    };

    static UP<CProtocol> g_protocol;

    static void          init(Apply apply) {
        g_apply    = std::move(apply);
        g_protocol = makeUnique<CProtocol>(&varuna_glass_manager_v1_interface, 1, "VarunaGlass");
    }

    static void stop() {
        g_protocol.reset();
        g_apply = nullptr;
    }
}
