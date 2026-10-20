// varuna-glass: live liquid glass under varuna's bar and dock.
//
// A Wayland client can't read what's behind its own surface, so the glass is
// drawn here, inside the compositor: after windows are rendered and before
// the top layers, for each varuna-bar / varuna-dock layer we copy the region
// behind it from the framebuffer being rendered, blur it, and draw the glass
// (crates/varuna-glass/src/shaders/glass.frag, ported from
// ryohsuke1231/liquid-glass, MIT) under the layer. The layer itself is drawn
// transparent by the varuna shell.
//
// Settings come from ~/.cache/varuna/glass.conf, written by the varuna daemon
// from config.toml [glass]; it is re-read when it changes.
//
// Other varuna surfaces (the Control Center's tiles) send their glass pieces
// with `hyprctl varunaglass shapes <namespace> x,y,w,h,r;...` (surface-local
// logical px; an empty list clears them).

#define WLR_USE_UNSTABLE

#include <algorithm>
#include <GLES3/gl32.h>

#include <cerrno>
#include <chrono>
#include <iterator>
#include <optional>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <sys/prctl.h>
#include <sys/stat.h>

// The drag icon fix (dnd.hpp) needs CWLDataDeviceProtocol::m_dnd, which is
// private: include DataDevice.hpp's dependencies normally, then it alone
// with private made public.
#include <any>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/WaylandProtocol.hpp>
#include <hyprland/src/protocols/types/DataDevice.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprutils/math/Edges.hpp>
#include <hyprland/protocols/xdg-shell.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/protocols/types/SurfaceRole.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
// Likewise CXDGToplevelResource::m_resource, to announce minimize support
// (requests.hpp).
#define private public
#include <hyprland/src/protocols/core/DataDevice.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#undef private
#include <hyprland/src/desktop/state/LayerState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/protocols/wlr-layer-shell-unstable-v1.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/debug/HyprCtl.hpp>
#include <hyprland/src/notification/NotificationOverlay.hpp>
#include "Varuna.hpp"
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

#include "dnd.hpp"
#include "drag.hpp"
#include "keepclear.hpp"
#include "requests.hpp"
#include "minimize.hpp"
#include "popups.hpp"
#include "edge.hpp"
#include "snap.hpp"
#include "corners.hpp"
#include "titlebar.hpp"
#include "conf.hpp"
#include "shaders.hpp"

static void notify(const std::string& text, const CHyprColor& color) {
    Notification::overlay()->addNotification(text, color, 8000);
}

// ---------------------------------------------------------------- settings --

struct SSurface {
    bool  enabled;
    float padding, radius;
    bool  shadow;
};

// Values come from glass.conf, checked against glass.def (see settingsFrom).
struct SSettings {
    bool     enabled;
    float    refraction, thickness, blur, tint, lift, rim, saturation;
    float    tintR, tintG, tintB;
    // "Platter" glass (Control Center tiles): frosted, tinted with the theme.
    float    platterBlur, platterTint, platterLift, platterSaturation, platterRim;
    float    platterR, platterG, platterB;
    // "Veil" glass (bar, Control Center): the platter's lens with a slight
    // black tint, bright backdrop capped (veilLift < 0), under white text.
    float    veilTint, veilLift;
    // Veil glass (Control Center…): white frost over the Dock's lens.
    float    veilWhite;
    // The top bar: veil glass always frosted (its own blur).
    float    barBlur;
    SSurface dock, bar;
};

static SSettings        g_settings;
static std::string      g_confPath;
static timespec         g_confMtime{};
static std::chrono::steady_clock::time_point g_lastConfCheck{};

static std::string confPath() {
    const char* cache = std::getenv("XDG_CACHE_HOME");
    const char* home  = std::getenv("HOME");
    std::string base  = cache && *cache ? cache : std::string(home ? home : "/tmp") + "/.cache";
    return base + "/varuna/glass.conf";
}

static SSettings settingsFrom(const Values& v) {
    const auto num  = [&](const char* k) { return v.at(k); };
    const auto flag = [&](const char* k) { return v.at(k) != 0.f; };
    return SSettings{
        .enabled           = flag("enabled"),
        .refraction        = num("refraction"),
        .thickness         = num("thickness"),
        .blur              = num("blur"),
        .tint              = num("tint"),
        .lift              = num("lift"),
        .rim               = num("rim"),
        .saturation        = num("saturation"),
        .tintR             = num("tint_r"),
        .tintG             = num("tint_g"),
        .tintB             = num("tint_b"),
        .platterBlur       = num("platter_blur"),
        .platterTint       = num("platter_tint"),
        .platterLift       = num("platter_lift"),
        .platterSaturation = num("platter_saturation"),
        .platterRim        = num("platter_rim"),
        .platterR          = num("platter_r"),
        .platterG          = num("platter_g"),
        .platterB          = num("platter_b"),
        .veilTint          = num("veil_tint"),
        .veilLift          = num("veil_lift"),
        .veilWhite         = num("veil_white"),
        .barBlur           = num("bar_blur"),
        .dock              = {flag("dock"), num("dock_padding"), num("dock_radius"), true},
        .bar               = {flag("bar"), num("bar_padding"), num("bar_radius"), false},
    };
}

// Snapping's settings (snap.hpp), from the same file.
static void configureSnap(const Values& v) {
    snap::configure(v.at("snap") != 0.f, v.at("snap_gap"), v.at("snap_gap_in"), v.at("buttons_right") != 0.f);
}

static void configureTitleBars(const Values& v) {
    const auto num    = [&](const std::string& key) { return v.at(key); };
    const auto rgb    = [&](const std::string& key) { return CHyprColor{num(key + "_r"), num(key + "_g"), num(key + "_b"), 1.f}; };
    const auto rgba   = [&](const std::string& key) { return CHyprColor{num(key + "_r"), num(key + "_g"), num(key + "_b"), num(key + "_a")}; };
    const auto button = [&](const std::string& state) {
        const auto key = "light_" + state;
        return titlebar::SButtonLook{
            .fill   = {rgba(key + "_close"), rgba(key + "_minimize"), rgba(key + "_zoom")},
            .symbol = rgba(key + "_symbol"),
            .finish = static_cast<titlebar::eFinish>(std::clamp((int)std::lround(num(key + "_finish")), 0, 2)),
            .ring   = num(key + "_ring") != 0.f,
            .hollow = num(key + "_hollow") != 0.f,
        };
    };
    titlebar::configure(titlebar::SLook{
        .on     = num("titlebars") != 0.f,
        .right  = num("buttons_right") != 0.f,
        .header = {.active = rgb("titlebar"), .backdrop = rgb("titlebar_backdrop"), .fg = rgba("titlebar_fg"), .shade = num("titlebar_shade")},
        .fontPt = num("titlebar_font_pt"),
        .iconPx = num("light_icon_px"),
        .rest   = button("rest"),
        .lit    = button("lit"),
    });
}

static void rejectConf(const std::string& why) {
    notify("[varuna-glass] ignoring glass.conf (" + why + "); keeping the previous settings", CHyprColor{1, .6, .2, 1});
}

static void loadSettings(bool force = false) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - g_lastConfCheck < std::chrono::milliseconds(500))
        return;
    g_lastConfCheck = now;

    struct stat st{};
    if (stat(g_confPath.c_str(), &st) != 0)
        return;
    if (!force && st.st_mtim.tv_sec == g_confMtime.tv_sec && st.st_mtim.tv_nsec == g_confMtime.tv_nsec)
        return;
    // Each version of the file is judged once (and a bad one reported once).
    g_confMtime = st.st_mtim;
    if (!S_ISREG(st.st_mode) || st.st_size > (off_t)MAX_CONF_BYTES) {
        rejectConf("not a small regular file");
        return;
    }
    std::ifstream in(g_confPath, std::ios::binary);
    std::string   text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        rejectConf("unreadable");
        return;
    }
    std::string error;
    if (const auto values = parseConf(text.substr(0, MAX_CONF_BYTES), error)) {
        g_settings = settingsFrom(*values);
        configureSnap(*values);
        configureTitleBars(*values);
    } else
        rejectConf(error);
}

// ---------------------------------------------------------------------- GL --

struct SGL {
    bool   ready = false, failed = false;
    GLuint blurProg = 0, glassProg = 0, vao = 0, vbo = 0;
    // capture (full res), then half-res ping-pong for the blur
    GLuint capTex = 0, capFb = 0, aTex = 0, aFb = 0, bTex = 0, bFb = 0;
    int    capW = 0, capH = 0, halfW = 0, halfH = 0;
    // Half-float targets for HDR range.
    bool   capFloat = false;
};
static SGL g_gl;

static GLuint compile(GLenum kind, const std::string& src) {
    GLuint s = glCreateShader(kind);
    const char* c = src.c_str();
    glShaderSource(s, 1, &c, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        Log::logger->log(Log::ERR, "[varuna-glass] shader: {}", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint program(const char* frag) {
    const std::string header = "#version 300 es\nprecision highp float;\n";
    GLuint            v = compile(GL_VERTEX_SHADER, header + QUAD_VERT), f = compile(GL_FRAGMENT_SHADER, header + frag);
    if (!v || !f)
        return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

static void makeTarget(GLuint& tex, GLuint& fb, int w, int h, bool fp) {
    if (!tex)
        glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (fp)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!fb)
        glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
}

static bool initGL() {
    if (g_gl.ready || g_gl.failed)
        return g_gl.ready;
    g_gl.blurProg  = program(BLUR_FRAG);
    g_gl.glassProg = program(GLASS_FRAG);
    if (!g_gl.blurProg || !g_gl.glassProg) {
        g_gl.failed = true;
        notify("[varuna-glass] shader compilation failed, see the Hyprland log", CHyprColor{1, .3, .3, 1});
        return false;
    }
    const float tri[] = {-1, -1, 3, -1, -1, 3};
    glGenVertexArrays(1, &g_gl.vao);
    glGenBuffers(1, &g_gl.vbo);
    glBindVertexArray(g_gl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_gl.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
    g_gl.ready = true;
    return true;
}

static void freeGL() {
    if (!g_gl.ready)
        return;
    GLuint texs[] = {g_gl.capTex, g_gl.aTex, g_gl.bTex};
    GLuint fbs[]  = {g_gl.capFb, g_gl.aFb, g_gl.bFb};
    glDeleteTextures(3, texs);
    glDeleteFramebuffers(3, fbs);
    glDeleteProgram(g_gl.blurProg);
    glDeleteProgram(g_gl.glassProg);
    glDeleteVertexArrays(1, &g_gl.vao);
    glDeleteBuffers(1, &g_gl.vbo);
    g_gl = {};
}

// Everything we touch, restored exactly so Hyprland's cached GL state stays true.
struct SGLState {
    GLint prog, drawFb, readFb, vao, arrayBuf, activeTex, tex0, viewport[4], scissorBox[4], blendSrcRGB, blendDstRGB, blendSrcA, blendDstA;
    GLboolean scissor, blend;
    void save() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFb);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuf);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        blend   = glIsEnabled(GL_BLEND);
    }
    void restore() {
        glUseProgram(prog);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFb);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFb);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, arrayBuf);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex0);
        glActiveTexture(activeTex);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
        glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcA, blendDstA);
        scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
        blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    }
};

// ------------------------------------------------------------------ geometry --

// How far the lens (and blur) samples beyond the glass, logical px.
/// How far a lifted piece's shadow reaches, px.
static constexpr float LIFTED_SHADOW = 30.f;

static float reach(float blur = g_settings.blur) {
    return 96.f + blur * 3.f + 4.f;
}

struct SGlass {
    CBox  box; // glass rect, monitor-local logical px
    float radius = 0;
    float shadow = 0; // shadow reach outside the rect, px
    uint8_t kind = 0;  // eKind
    int     tint = -1; // 0xrrggbb: glass tinted with it (water mixed with colour)
    float   sx = 0, sy = 0; // liquid: how it's pulled out behind as it moves
    bool    lifted = false; // a deeper shadow (search: over busy windows)
    float   opacity = 1.f;  // its surface's fade (opening, closing)
};

/// What a piece of glass looks like: clear (Dock), platter (the switcher),
/// veil (Control Center, notifications, menus: the Dock's, darkened) or
/// bar (always frosted).
enum eKind : uint8_t {
    KIND_CLEAR,
    KIND_PLATTER,
    KIND_VEIL,
    KIND_BAR,
    KIND_LIQUID,
    KIND_BACKDROP,
};

struct SOptics {
    float blur, tint, lift, r, g, b, saturation, rim, grain = 0.f;
    float dye  = 0.f;
    bool  lens = true; // refraction, rim light and edge shading
};

static SOptics opticsFor(uint8_t kind) {
    const auto& s = g_settings;
    if (kind == KIND_PLATTER)
        return {s.platterBlur, s.platterTint, s.platterLift, s.platterR, s.platterG, s.platterB, s.platterSaturation, s.platterRim};
    // The Dock's glass (lens, saturation, rim) with white frost and blur
    // that the frost setting raises (as varuna_glass::veil_blur): at
    // minimum, exactly the Dock's.
    if (kind == KIND_VEIL)
        return {s.blur + 0.4f * s.platterBlur, s.veilWhite, 0.f, 1.f, 1.f, 1.f, s.saturation, s.rim};
    // Always frosted, soft grey with a faint grain (varuna_glass::BAR_FROST,
    // BAR_WHITE, BAR_GRAIN), whatever frost is set to; flat, no lens.
    if (kind == KIND_BAR)
        return {s.barBlur, 0.3f, 0.f, 0.62f, 0.62f, 0.62f, 1.4f, 0.f, 0.035f, 0.f, false};
    // Behind the Control Center while editing: the screen out of focus,
    // dimmed (varuna_glass::BACKDROP_BLUR, BACKDROP_DIM); flat.
    if (kind == KIND_BACKDROP)
        return {48.f, 0.35f, 0.f, 0.04f, 0.04f, 0.06f, 1.2f, 0.f, 0.f, 0.f, false};
    return {s.blur, s.tint, s.lift, s.tintR, s.tintG, s.tintB, s.saturation, s.rim};
}

// Pieces sent by the shell per layer namespace, surface-local.
struct SShape {
    double x, y, w, h;
    float  radius;
    int    tint = -1; // optional 6th field, rrggbb
    float  sx = 0, sy = 0; // optional 7th and 8th: a drop's stretch
};
static std::map<std::string, std::vector<SShape>> g_shapes;
/// Each namespace's kind of glass (`shapes <ns> platter|veil|bar ...`; clear
/// when not named).
static std::map<std::string, uint8_t> g_kind;
/// Namespaces whose pieces cast a deeper shadow (`shapes <ns> lifted ...`).
static std::set<std::string> g_lifted;

/// Shapes are keyed "<namespace>@<monitor>" (one dock per monitor shares a
/// namespace), or just "<namespace>" for every monitor.
static bool keyMatches(const std::string& key, const PHLLS& ls) {
    const auto at = key.find('@');
    if (at == std::string::npos)
        return ls->m_namespace == key;
    const auto mon = ls->m_monitor.lock();
    return ls->m_namespace == key.substr(0, at) && mon && mon->m_name == key.substr(at + 1);
}

static std::string onShapesCommand(eHyprCtlOutputFormat, std::string request) {
    // "varunaglass shapes <namespace> [x,y,w,h,r;...]"
    std::istringstream in(request);
    std::string        cmd, sub, ns, list;
    in >> cmd >> sub;
    // Minimizing into the Dock (minimize.hpp).
    if (sub == "features")
        return "minimize";
    if (sub == "minimize" || sub == "restore" || sub == "snapinfo" || sub == "where")
        return minimize::command(sub, in);
    // Snapping a floating window (snap.hpp).
    if (sub == "snap")
        return snap::command(in);
    if (sub == "fill")
        return snap::command(in, true);
    in >> ns;
    std::getline(in, list);
    // Optional "lifted" before the list: a deeper shadow under its pieces.
    bool lifted = false;
    if (const auto at = list.find("lifted"); at != std::string::npos && at < list.find_first_of("0123456789-")) {
        lifted = true;
        list.erase(at, 6);
    }
    // Optional kind before the list: "platter", "veil" or "bar".
    uint8_t kind = KIND_CLEAR;
    for (const auto& [word, k] : {std::pair{std::string("platter"), KIND_PLATTER}, std::pair{std::string("veil"), KIND_VEIL},
                                  std::pair{std::string("bar"), KIND_BAR}, std::pair{std::string("liquid"), KIND_LIQUID},
                                  std::pair{std::string("backdrop"), KIND_BACKDROP}}) {
        const auto at = list.find(word);
        if (at != std::string::npos && at < list.find_first_of("0123456789-")) {
            kind = k;
            list = list.substr(at + word.size());
            break;
        }
    }
    if (sub != "shapes" || ns.empty())
        return "usage: varunaglass shapes <namespace> x,y,w,h,r;...";
    std::vector<SShape> shapes;
    std::istringstream  items(list);
    for (std::string item; std::getline(items, item, ';');) {
        SShape   sh{};
        unsigned rgb = 0;
        const int n  = std::sscanf(item.c_str(), " %lf,%lf,%lf,%lf,%f,%6x,%f,%f", &sh.x, &sh.y, &sh.w, &sh.h, &sh.radius, &rgb, &sh.sx, &sh.sy);
        if (n >= 6)
            sh.tint = (int)rgb;
        if (n >= 5 && sh.w > 1 && sh.h > 1)
            shapes.push_back(sh);
    }
    const bool changed = [&] {
        static const std::vector<SShape> none;
        const auto                       it  = g_shapes.find(ns);
        const auto&                      old = it == g_shapes.end() ? none : it->second;
        if (old.size() != shapes.size())
            return true;
        for (size_t i = 0; i < old.size(); i++)
            if (old[i].x != shapes[i].x || old[i].y != shapes[i].y || old[i].w != shapes[i].w || old[i].h != shapes[i].h)
                return true;
        return false;
    }();
    std::vector<SShape> old;
    if (const auto it = g_shapes.find(ns); it != g_shapes.end())
        old = it->second;
    // Redraw only where glass was or will be (plus what the lens can see):
    // the dock sends a new shape every frame while it magnifies.
    if (changed)
        for (const auto& ls : Desktop::layerState()->layers()) {
            if (!ls || !keyMatches(ns, ls))
                continue;
            for (const auto* list : {&old, &shapes})
                for (const auto& sh : *list) {
                    // Global layout coordinates; also schedules the frame.
                    CBox b{ls->m_geometry.x + sh.x, ls->m_geometry.y + sh.y, sh.w, sh.h};
                    g_pHyprRenderer->damageBox(b.expand(reach(std::max({g_settings.blur, g_settings.platterBlur, g_settings.barBlur})) + 10));
                }
        }
    // An empty list forgets the namespace (banners come and go by the dozen).
    if (shapes.empty()) {
        g_shapes.erase(ns);
        g_kind.erase(ns);
        g_lifted.erase(ns);
    } else {
        g_shapes[ns] = std::move(shapes);
        g_kind[ns]   = kind;
        if (lifted)
            g_lifted.insert(ns);
        else
            g_lifted.erase(ns);
    }
    return "ok";
}

static std::vector<SGlass> glassesFor(const PHLMONITOR& mon) {
    std::vector<SGlass> out;
    // With glass off, the top bar stays frosted all the same.
    const bool barOnly = !g_settings.enabled;
    // Desktop::layerState() is the authoritative list in 0.56 (the monitor's
    // per-level vectors are not kept up to date for this purpose).
    // A full-screen window hides the background, bottom and top layers
    // (Hyprland doesn't draw them, though they stay "visible"): no glass
    // for them either, or it would float over the app with nothing on it.
    const bool fullscreen = Fullscreen::controller()->hasFullscreen(mon);
    for (const auto& ls : Desktop::layerState()->layers()) {
        if (!ls || ls->m_monitor != mon)
            continue;
        // A surface fading out (closed, Hyprland still drawing it as it
        // goes) keeps its glass, fading with it; fading in, likewise.
        const float alpha   = std::clamp(ls->alpha().value(), 0.f, 1.f);
        const bool  leaving = !ls->m_mapped && alpha > 0.01f;
        if (!leaving && (!ls->m_mapped || !ls->visible()))
            continue;
        if (barOnly && ls->m_namespace != "varuna-bar")
            continue;
        if (fullscreen && ls->m_layer < ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY)
            continue;
        const std::string perMonitor = ls->m_namespace + "@" + mon->m_name;
        auto              it         = g_shapes.find(perMonitor);
        if (it == g_shapes.end())
            it = g_shapes.find(ls->m_namespace);
        // Only surfaces that send their shapes fade their glass; the bar and
        // Dock below are drawn while mapped.
        if (leaving && it == g_shapes.end())
            continue;
        if (it != g_shapes.end()) {
            for (const auto& sh : it->second) {
                CBox b{ls->m_geometry.x - mon->m_position.x + sh.x, ls->m_geometry.y - mon->m_position.y + sh.y, sh.w, sh.h};
                const auto k = g_kind.find(it->first);
                const uint8_t kind = k == g_kind.end() ? (uint8_t)KIND_CLEAR : k->second;
                // A drop's tail reaches past its box: room for it; a lifted
                // piece's shadow reaches further.
                const bool lifted = g_lifted.contains(it->first);
                const float   shadow = kind == KIND_BACKDROP ? 0.f : kind == KIND_LIQUID ? 26.f : lifted ? LIFTED_SHADOW : 10.f;
                out.push_back({b, sh.radius, shadow, kind, sh.tint, sh.sx, sh.sy, lifted, alpha});
            }
            continue;
        }
        // The bar and Dock sit on the top layer, or overlay (above
        // full-screen apps, see the shell's slide.rs).
        if (ls->m_layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP)
            continue;
        const SSurface* s = ls->m_namespace == "varuna-dock" ? &g_settings.dock : ls->m_namespace == "varuna-bar" ? &g_settings.bar : nullptr;
        if (!s || !s->enabled)
            continue;
        CBox b = ls->m_geometry.copy();
        b.translate(-mon->m_position);
        b.x += s->padding;
        b.y += s->padding;
        b.w -= 2 * s->padding;
        b.h -= 2 * s->padding;
        if (b.w <= 1 || b.h <= 1)
            continue;
        // The bar carries white text: veil glass, always frosted. The Dock: clear.
        out.push_back({b, s->radius, s->shadow ? s->padding : 0.f, s == &g_settings.bar ? (uint8_t)KIND_BAR : (uint8_t)KIND_CLEAR, -1, 0, 0, false, alpha});
    }
    // A backdrop goes first: the glass over it refracts the blurred screen.
    std::stable_partition(out.begin(), out.end(), [](const SGlass& g) { return g.kind == KIND_BACKDROP; });
    return out;
}

// ------------------------------------------------------------ pass element --

class CGlassElement : public IPassElement {
  public:
    CGlassElement(PHLMONITOR mon, SGlass glass) : m_mon(mon), m_glass(glass) {}

    std::vector<UP<IPassElement>> draw() override {
        if (auto mon = m_mon.lock())
            render(mon);
        return {};
    }
    bool needsLiveBlur() override {
        return false;
    }
    bool needsPrecomputeBlur() override {
        return false;
    }
    const char* passName() override {
        return "CVarunaGlassElement";
    }
    ePassElementType type() override {
        return EK_CUSTOM;
    }
    std::optional<CBox> boundingBox() override {
        return m_glass.box.copy().expand(m_glass.shadow);
    }
    bool disableSimplification() override {
        return false;
    }

  private:
    PHLMONITORREF m_mon;
    SGlass        m_glass;

    void          render(const PHLMONITOR& mon);
};

void CGlassElement::render(const PHLMONITOR& mon) {
    auto fb = dynamicPointerCast<Render::GL::CGLFramebuffer>(g_pHyprRenderer->m_renderData.currentFB);
    if (!fb || !initGL())
        return;

    const float scale = mon->m_scale;
    // m_renderData.fbSize can be stale (e.g. left at the cursor buffer's
    // size), so use the framebuffer's own. Rotated monitors aren't handled.
    const int fbW = (int)fb->m_size.x, fbH = (int)fb->m_size.y;
    if (fbW <= 0 || fbH <= 0 || fb->m_size != mon->m_transformedSize || mon->m_transform != WL_OUTPUT_TRANSFORM_NORMAL)
        return;

    // Region behind the glass that the lens and blur may sample (logical).
    const float pad = m_glass.shadow;
    auto        o   = opticsFor(m_glass.kind);
    // The bubble's drop: clear glass with a little of its colour dissolved
    // in it (varuna_glass::LIQUID_DYE), its edge rippling.
    const bool liquid = m_glass.kind == KIND_LIQUID;
    if (liquid && m_glass.tint >= 0) {
        o.r    = ((m_glass.tint >> 16) & 0xff) / 255.f;
        o.g    = ((m_glass.tint >> 8) & 0xff) / 255.f;
        o.b    = (m_glass.tint & 0xff) / 255.f;
        o.tint = 0.f;
        o.lift = 0.f;
        o.dye  = 0.28f;
        // No bright ring round the drop: it's water, not a lens.
        o.rim  = 0.f;
    } else if (m_glass.tint >= 0) {
        // Tinted: dyed with the colour, like water mixed with colour
        // (varuna_glass::tinted, TINTED_DYE).
        o.r    = ((m_glass.tint >> 16) & 0xff) / 255.f;
        o.g    = ((m_glass.tint >> 8) & 0xff) / 255.f;
        o.b    = (m_glass.tint & 0xff) / 255.f;
        o.tint = 0.f;
        o.lift = 0.f;
        o.dye  = 0.85f;
    }
    CBox        src = m_glass.box.copy().expand(reach(o.blur));
    src.x = std::max(0.0, src.x);
    src.y = std::max(0.0, src.y);
    src.w = std::min(mon->m_size.x - src.x, src.w);
    src.h = std::min(mon->m_size.y - src.y, src.h);
    const int px = (int)std::floor(src.x * scale), py = (int)std::floor(src.y * scale);
    const int pw = std::max(1, (int)std::ceil(src.w * scale)), ph = std::max(1, (int)std::ceil(src.h * scale));

    // HDR: linear light, 1.0 = 80 nits.
    const bool  hdr      = mon->m_cmType == NCMType::CM_HDR || mon->m_cmType == NCMType::CM_HDR_EDID;
    const float refWhite = hdr ? std::max(mon->m_sdrMaxLuminance * mon->m_sdrBrightness, 1.f) / 80.f : 1.f;
    const bool  fp       = refWhite != 1.f;

    SGLState st;
    st.save();

    // 1. capture: framebuffer -> capTex (full res)
    if (pw > g_gl.capW || ph > g_gl.capH || fp != g_gl.capFloat) {
        g_gl.capW     = std::max(pw, g_gl.capW);
        g_gl.capH     = std::max(ph, g_gl.capH);
        g_gl.capFloat = fp;
        makeTarget(g_gl.capTex, g_gl.capFb, g_gl.capW, g_gl.capH, fp);
        g_gl.halfW = std::max(1, g_gl.capW / 2);
        g_gl.halfH = std::max(1, g_gl.capH / 2);
        makeTarget(g_gl.aTex, g_gl.aFb, g_gl.halfW, g_gl.halfH, fp);
        makeTarget(g_gl.bTex, g_gl.bFb, g_gl.halfW, g_gl.halfH, fp);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb->getFBID());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl.capFb);
    // Hyprland's framebuffer has row 0 at the top of the screen (it scissors
    // with damage rects as-is), so no flip here or when drawing.
    glBlitFramebuffer(px, py, px + pw, py + ph, 0, 0, pw, ph, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // 2. downsample to half res, then separable Gaussian: a -> b -> a
    const int hw = std::max(1, pw / 2), hh = std::max(1, ph / 2);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.capFb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl.aFb);
    glBlitFramebuffer(0, 0, pw, ph, 0, 0, hw, hh, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glDisable(GL_BLEND);
    glBindVertexArray(g_gl.vao);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(g_gl.blurProg);
    glUniform1i(glGetUniformLocation(g_gl.blurProg, "u_tex"), 0);
    glUniform2f(glGetUniformLocation(g_gl.blurProg, "u_texel"), 1.f / g_gl.halfW, 1.f / g_gl.halfH);
    glUniform1f(glGetUniformLocation(g_gl.blurProg, "u_sigma"), o.blur * scale / 2.f);
    glViewport(0, 0, hw, hh);
    const GLuint passes[2][3] = {{g_gl.aTex, g_gl.bFb, 0}, {g_gl.bTex, g_gl.aFb, 1}};
    for (const auto& p : passes) {
        glBindFramebuffer(GL_FRAMEBUFFER, p[1]);
        glBindTexture(GL_TEXTURE_2D, p[0]);
        glUniform2f(glGetUniformLocation(g_gl.blurProg, "u_dir"), p[2] ? 0.f : 1.f, p[2] ? 1.f : 0.f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // 3. the glass, into the framebuffer being rendered, within the damage
    glBindFramebuffer(GL_FRAMEBUFFER, fb->getFBID());
    glViewport(0, 0, fbW, fbH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_gl.glassProg);
    glBindTexture(GL_TEXTURE_2D, g_gl.aTex);
    const GLuint gp = g_gl.glassProg;
    auto         u1 = [gp](const char* n, float v) { glUniform1f(glGetUniformLocation(gp, n), v); };
    glUniform1i(glGetUniformLocation(gp, "u_blur"), 0);
    u1("u_ref_white", refWhite);
    // The blurred texture covers src at (hw, hh) of a (halfW, halfH) texture:
    // express it as a src box scaled so uv = (p - origin) / size lands right.
    glUniform2f(glGetUniformLocation(gp, "u_src_origin"), src.x, src.y);
    glUniform2f(glGetUniformLocation(gp, "u_src_size"), src.w * g_gl.halfW / (float)hw, src.h * g_gl.halfH / (float)hh);
    glUniform2f(glGetUniformLocation(gp, "u_origin"), 0.f, 0.f);
    glUniform4f(glGetUniformLocation(gp, "u_rect"), m_glass.box.x, m_glass.box.y, m_glass.box.w, m_glass.box.h);
    glUniform3f(glGetUniformLocation(gp, "u_tint"), o.r, o.g, o.b);
    u1("u_fb_h", (float)fbH);
    u1("u_flip_y", 0.f);
    u1("u_scale", scale);
    u1("u_grad_step", std::clamp((float)std::min(mon->m_size.x, mon->m_size.y) / 560.f, 0.45f, 1.2f));
    u1("u_radius", m_glass.radius);
    u1("u_max_z", o.lens ? g_settings.thickness : 0.f);
    u1("u_disp", o.lens ? g_settings.refraction : 0.f);
    u1("u_shape_n", 3.6f);
    u1("u_ior", 2.4f);
    u1("u_edge_smooth", 0.5f);
    u1("u_opacity", m_glass.opacity);
    u1("u_tint_strength", o.tint);
    u1("u_lift", o.lift);
    u1("u_saturation", o.saturation);
    u1("u_grain", o.grain);
    u1("u_dye", o.dye);
    u1("u_shape", liquid ? 1.f : 0.f);
    glUniform2f(glGetUniformLocation(gp, "u_stretch"), m_glass.sx, m_glass.sy);
    static const auto start = std::chrono::steady_clock::now();
    u1("u_time", std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count());
    u1("u_rim_width", o.lens && !liquid ? 2.3f : 0.f);
    u1("u_rim_intensity", o.rim);
    u1("u_rim_dir_power", 1.9f);
    u1("u_rim_power", 3.0f);
    u1("u_light_deg", 90.f);
    u1("u_ao_radius", 1.f);
    u1("u_ao_intensity", o.lens ? 0.65f : 0.f);
    // A drop's box is padded for its tail, not for a bigger shadow.
    const float shadowPad = liquid ? 10.f : pad;
    u1("u_shadow_radius", std::min(shadowPad, 50.f));
    u1("u_shadow_intensity", m_glass.lifted ? 0.45f : 0.22f);
    u1("u_shadow_max", std::max(shadowPad - 1.f, 0.f));

    // Only inside this frame's damage: outside it the framebuffer already
    // holds last frame's glass, and drawing again would stack the tint.
    CRegion area = g_pHyprRenderer->m_renderData.damage.copy().intersect(boundingBox()->copy().scale(scale).round());
    glEnable(GL_SCISSOR_TEST);
    for (const auto& r : area.getRects()) {
        const int x = r.x1, w = r.x2 - r.x1, h = r.y2 - r.y1;
        glScissor(x, r.y1, w, h);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    st.restore();
}

// ------------------------------------------------------------------ hooks --

static void onRenderPre(PHLMONITOR mon) {
    minimize::onRenderPre(mon);
    dnd::track();
    drag::remember();
    keepclear::clamp(mon);
    loadSettings();
    // If anything the lens can see changed, redraw all it sees this frame
    // (added to the monitor's damage ring, so every buffer stays consistent).
    // All of it, not just the glass: the capture reads around the glass, and
    // anything there left undamaged still holds last frame's finished image
    // -- this surface's own tiles and text -- which the glass would then
    // refract (ghost copies, flickering edges). Redrawn areas reach further
    // glass (tiles side by side), so it spreads until nothing new is hit.
    CRegion damage = mon->m_damage.getBufferDamage(1);
    if (damage.empty())
        return;
    const auto       glasses = glassesFor(mon);
    std::vector<CBox> seen;
    std::vector<bool> done(glasses.size(), false);
    for (const auto& g : glasses)
        seen.push_back(g.box.copy().expand(std::max<double>(reach(opticsFor(g.kind).blur), g.shadow)).scale(mon->m_scale).round());
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t i = 0; i < glasses.size(); i++) {
            if (done[i] || damage.copy().intersect(seen[i]).empty())
                continue;
            done[i] = true;
            grew    = true;
            damage.add(seen[i]);
            mon->m_damage.damage(seen[i]);
        }
    }
}

static void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_POST_WINDOWS)
        return;
    const auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!mon)
        return;
    // Minimizing windows draw over the others, under the Dock's glass.
    minimize::onRenderStage(mon);
    // Where a dragged window would snap.
    snap::render(mon);
    for (const auto& g : glassesFor(mon)) {
        g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassElement>(mon, g));
        // The drop keeps moving: draw it again next frame.
        if (g.kind == KIND_LIQUID)
            g_pHyprRenderer->damageBox(g.box.copy().expand(g.shadow).translate(mon->m_position));
    }
}

// Repaint and re-lay-out everything: after the plugin loads or unloads, the
// windows already open would otherwise keep their last frame (and a stale
// size) until they're moved or clicked.
static void refreshAll() {
    for (const auto& m : State::monitorState()->monitors()) {
        if (!m)
            continue;
        g_layoutManager->recalculateMonitor(m);
        g_pHyprRenderer->damageMonitor(m);
    }
}

static void start() {
    requests::init();
    drag::init();
    keepclear::init();
    popups::init();
    // Before snapping: it sees the pointer held at a monitor's edge.
    edge::init();
    edge::g_held = [] { snap::onMove(); };
    requests::g_onGreenButton = snap::onGreenButtonAt;
    requests::g_limited       = snap::limited;
    snap::init();
    corners::init();
    minimize::init();
    titlebar::init();
    g_settings = settingsFrom(defaultValues());
    g_confPath = confPath();
    loadSettings(true);

    static auto P1 = Event::bus()->m_events.render.pre.listen([](PHLMONITOR m) { onRenderPre(m); });
    static auto P2 = Event::bus()->m_events.render.stage.listen([](eRenderStage s) { onRenderStage(s); });
    static auto P3 = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) {
        if (w)
            minimize::forget(w.get());
    });
}

static void stop() {
    titlebar::stop();
    snap::stop();
    minimize::stop();
    dnd::stop();
    requests::stop();
    g_pHyprRenderer->m_renderPass.removeAllOfType("CVarunaGlassElement");
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    freeGL();
}

static const SHyprCtlCommand COMMAND = {.name = "varunaglass", .exact = false, .fn = onShapesCommand};

static const bool g_announced = setenv("VARUNA_COMPOSITOR", "1", 1) == 0;
static const bool g_named     = prctl(PR_SET_NAME, "Varuna") == 0;

void Varuna::init() {
    start();
    g_pHyprCtl->registerCommand(COMMAND);
    refreshAll();
}

void Varuna::stop() {
    ::stop();
}
