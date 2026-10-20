#pragma once

#include <istream>
#include <optional>
#include <string>

#include <linux/input-event-codes.h>

#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace picker {

    static constexpr const char* CANCELLED = "cancel";

    static bool                  g_active         = false;
    static bool                  g_swallowRelease = false;

    static void (*g_draw)(const PHLMONITOR& mon)                                        = nullptr;
    static std::optional<std::string> (*g_colourAt)(const PHLMONITOR&, const Vector2D&) = nullptr;
    static void (*g_forget)()                                                           = nullptr;

    static void damageAll() {
        for (const auto& m : State::monitorState()->monitors())
            if (m)
                g_pHyprRenderer->damageMonitor(m);
    }

    static void finish(const std::string& result) {
        if (!g_active)
            return;
        g_active = false;
        g_pHyprRenderer->setCursorHidden(false);
        if (g_forget)
            g_forget();
        damageAll();
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "varunapick", .data = result});
    }

    static std::string pickedHere() {
        const auto at  = g_pInputManager->getMouseCoordsInternal();
        const auto mon = State::monitorState()->query().vec(at).run();
        if (!mon || !g_colourAt)
            return CANCELLED;
        return g_colourAt(mon, at).value_or(CANCELLED);
    }

    static std::string command(std::istream& in) {
        std::string what;
        in >> what;
        if (what == "cancel") {
            finish(CANCELLED);
            return "ok";
        }
        if (g_active)
            return "busy";
        g_active = true;
        g_pHyprRenderer->setCursorHidden(true);
        damageAll();
        return "ok";
    }

    static void onRenderLast(const PHLMONITOR& mon) {
        if (g_active && g_draw)
            g_draw(mon);
    }

    static void onButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED && g_swallowRelease) {
            g_swallowRelease = false;
            info.cancelled   = true;
            return;
        }
        if (!g_active || e.state != WL_POINTER_BUTTON_STATE_PRESSED)
            return;
        info.cancelled   = true;
        g_swallowRelease = true;
        finish(e.button == BTN_LEFT ? pickedHere() : CANCELLED);
    }

    static void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
        if (!g_active)
            return;
        info.cancelled = true;
        if (e.keycode == KEY_ESC && e.state == WL_KEYBOARD_KEY_STATE_PRESSED)
            finish(CANCELLED);
    }

    static void init() {
        static auto move   = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) {
            if (g_active)
                damageAll();
        });
        static auto button = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onButton(e, info); });
        static auto key    = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) { onKey(e, info); });
    }

    static void stop() {
        finish(CANCELLED);
    }
}
