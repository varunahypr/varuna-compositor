#pragma once

#include <format>
#include <map>
#include <sstream>
#include <string>

#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace edges {

    static constexpr double TOUCH = 2.0;

    enum eSide : uint8_t {
        TOP,
        BOTTOM,
        LEFT,
        RIGHT,
    };

    struct SZone {
        std::string monitor;
        eSide       side     = TOP;
        double      from     = 0;
        double      to       = 0;
        double      depth    = 0;
        bool        touching = false;
        bool        near     = false;
    };

    static std::map<std::string, SZone> g_zones;
    static bool                         g_checkQueued = false;

    static std::optional<eSide>         sideNamed(const std::string& name) {
        if (name == "top")
            return TOP;
        if (name == "bottom")
            return BOTTOM;
        if (name == "left")
            return LEFT;
        if (name == "right")
            return RIGHT;
        return std::nullopt;
    }

    static void post(const std::string& name, const char* what, bool on) {
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "varunaedge", .data = std::format("{},{},{}", name, what, on ? 1 : 0)});
    }

    static void update(const std::string& name, SZone& zone, const Vector2D& p, bool announce) {
        const auto mon      = State::monitorState()->query().name(zone.monitor).run();
        bool       touching = false, near = false;
        if (mon) {
            const CBox   box      = mon->logicalBox();
            const bool   inside   = box.containsPoint(p);
            const bool   across   = zone.side == TOP || zone.side == BOTTOM;
            const double along    = across ? p.x - box.x : p.y - box.y;
            const double distance = zone.side == TOP ? p.y - box.y : zone.side == BOTTOM ? box.y + box.h - p.y : zone.side == LEFT ? p.x - box.x : box.x + box.w - p.x;
            const bool   over     = zone.to <= zone.from || (along >= zone.from && along < zone.to);
            touching              = inside && over && distance <= TOUCH;
            near                  = inside && over && distance <= zone.depth;
        }
        if (announce || touching != zone.touching)
            post(name, "touch", touching);
        if (announce || near != zone.near)
            post(name, "near", near);
        zone.touching = touching;
        zone.near     = near;
    }

    static void check() {
        const Vector2D p = g_pInputManager->getMouseCoordsInternal();
        for (auto& [name, zone] : g_zones)
            update(name, zone, p, false);
    }

    static void checkSoon() {
        if (g_checkQueued || g_zones.empty())
            return;
        g_checkQueued = true;
        g_pEventLoopManager->doLater([] {
            g_checkQueued = false;
            check();
        });
    }

    static std::string command(std::istringstream& in) {
        std::string sub, name;
        in >> sub >> name;
        if (sub == "clear" && !name.empty()) {
            g_zones.erase(name);
            return "ok";
        }
        std::string monitor, side;
        SZone       zone;
        if (sub != "set" || name.empty() || !(in >> monitor >> side >> zone.from >> zone.to >> zone.depth))
            return "usage: varunaglass edge set NAME MONITOR top|bottom|left|right FROM TO DEPTH | edge clear NAME";
        const auto s = sideNamed(side);
        if (!s)
            return "no such side";
        zone.monitor = monitor;
        zone.side    = *s;
        update(name, g_zones[name] = zone, g_pInputManager->getMouseCoordsInternal(), true);
        return "ok";
    }

    static void init() {
        static auto move  = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) { checkSoon(); });
        static auto frame = Event::bus()->m_events.render.pre.listen([](PHLMONITOR) { checkSoon(); });
    }

    static void stop() {
        g_zones.clear();
    }

} // namespace edges
