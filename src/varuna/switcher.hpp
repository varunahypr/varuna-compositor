#pragma once

#include <istream>
#include <string>
#include <vector>

#include <xkbcommon/xkbcommon.h>

#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

namespace switcher {

    static bool                             g_open = false;
    static std::vector<CHyprSignalListener> g_watching;

    static void say(const char* what) {
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "varunaswitcher", .data = what});
    }

    static bool held(uint32_t modifier) {
        return g_pInputManager->getModsFromAllKBs() & modifier;
    }

    static constexpr uint32_t XKB_OFFSET = 8;

    static xkb_keysym_t symbolOf(uint32_t keycode) {
        const auto keyboard = g_pSeatManager->m_keyboard.lock();
        if (!keyboard)
            return XKB_KEY_NoSymbol;
        const auto state = keyboard->m_xkbSymState ? keyboard->m_xkbSymState : keyboard->m_xkbState;
        return state ? xkb_state_key_get_one_sym(state, keycode + XKB_OFFSET) : XKB_KEY_NoSymbol;
    }

    static bool isSuper(xkb_keysym_t symbol) {
        return symbol == XKB_KEY_Super_L || symbol == XKB_KEY_Super_R;
    }

    static void end() {
        g_open = false;
        g_watching.clear();
    }

    static void releaseWhenSuperIsUp() {
        g_pEventLoopManager->doLater([] {
            if (g_open && !held(HL_MODIFIER_META)) {
                end();
                say("release");
            }
        });
    }

    static void watchModifiers() {
        g_watching.clear();
        for (const auto& keyboard : g_pInputManager->m_keyboards)
            if (keyboard)
                g_watching.push_back(keyboard->m_keyboardEvents.modifiers.listen([](const IKeyboard::SModifiersEvent&) { releaseWhenSuperIsUp(); }));
    }

    static const char* stepFor(xkb_keysym_t symbol) {
        switch (symbol) {
            case XKB_KEY_Tab: return held(HL_MODIFIER_SHIFT) ? "prev" : "next";
            case XKB_KEY_ISO_Left_Tab:
            case XKB_KEY_Left:
            case XKB_KEY_Up: return "prev";
            case XKB_KEY_Right:
            case XKB_KEY_Down: return "next";
            case XKB_KEY_Escape: return "cancel";
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter: return "release";
            case XKB_KEY_q:
            case XKB_KEY_Q: return "quit";
            case XKB_KEY_h:
            case XKB_KEY_H: return "hide";
            default: return nullptr;
        }
    }

    static void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
        const bool         pressed = e.state == WL_KEYBOARD_KEY_STATE_PRESSED;
        const xkb_keysym_t symbol  = symbolOf(e.keycode);
        if (!g_open) {
            const bool tab = symbol == XKB_KEY_Tab || symbol == XKB_KEY_ISO_Left_Tab;
            if (pressed && tab && held(HL_MODIFIER_META) && !g_pSessionLockManager->isSessionLocked()) {
                g_open         = true;
                info.cancelled = true;
                watchModifiers();
                say(held(HL_MODIFIER_SHIFT) ? "open prev" : "open next");
            }
            return;
        }
        if (isSuper(symbol)) {
            if (!pressed) {
                end();
                say("release");
            }
            return;
        }
        info.cancelled = true;
        if (!pressed)
            return;
        const char* step = stepFor(symbol);
        if (!step)
            return;
        if (std::string_view(step) == "cancel" || std::string_view(step) == "release")
            end();
        say(step);
    }

    static std::string command(std::istream& in) {
        std::string what;
        in >> what;
        if (what == "end")
            end();
        else if (what != "state")
            return "usage: varunaglass switcher end|state";
        return g_open ? "open" : "closed";
    }

    static void init() {
        static auto key = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) { onKey(e, info); });
    }
}
