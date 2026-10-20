// Generated from crates/varuna-glass/src/shaders. Do not edit.
#pragma once
static const char* QUAD_VERT  = R"GLSL(// Fullscreen triangle; positions come from a 3-vertex buffer.
in vec2 a_pos;
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)GLSL";
static const char* BLUR_FRAG  = R"GLSL(// One direction of a separable Gaussian blur over the backdrop texture.
// Target and source have the same size, so gl_FragCoord maps 1:1.
uniform sampler2D u_tex;
uniform vec2 u_texel;   // 1 / texture size
uniform vec2 u_dir;     // (1,0) or (0,1)
uniform float u_sigma;  // in texels
out vec4 o_color;

void main() {
    vec2 uv = gl_FragCoord.xy * u_texel;
    if (u_sigma < 0.3) {
        o_color = texture(u_tex, uv);
        return;
    }
    int r = min(int(ceil(u_sigma * 3.0)), 48);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int i = -48; i <= 48; i++) {
        if (i < -r || i > r) continue;
        float w = exp(-float(i * i) / (2.0 * u_sigma * u_sigma));
        sum += texture(u_tex, uv + u_dir * u_texel * float(i)) * w;
        wsum += w;
    }
    o_color = sum / wsum;
}
)GLSL";
static const char* GLASS_FRAG = R"GLSL(// Liquid glass over a blurred backdrop.
//
// Ported from ryohsuke1231/liquid-glass, shaders/glass.frag
// (MIT License, Copyright (c) 2026 Ryosuke Watanabe): the rounded-rect SDF,
// superellipse dome over a fixed 22px edge band, Snell refraction with the
// rim-weighted lens, footprint anti-aliasing, directional rim light, inner
// AO and the umbra/penumbra drop shadow. Shared by the GTK GLArea renderer
// and the varuna-glass Hyprland plugin: coordinates are logical px, y down.

uniform sampler2D u_blur;
uniform vec2 u_src_origin;   // backdrop top-left in the same space as u_origin
uniform vec2 u_src_size;     // backdrop size in logical px
uniform float u_grad_step;   // normal-estimation step, from the monitor size
uniform vec2 u_origin;       // this area's top-left in backdrop px
uniform float u_fb_h;        // framebuffer height, device px
uniform float u_flip_y;      // 1: framebuffer y points up (GTK); 0: down
uniform float u_scale;       // device px per logical px
uniform vec4 u_rect;         // glass rect inside the area (x, y, w, h)
uniform float u_radius;
uniform float u_max_z;
uniform float u_disp;
uniform float u_shape_n;
uniform float u_ior;
uniform float u_edge_smooth;
uniform float u_opacity;      // the surface's own fade, 0..1 (1: as drawn)
uniform float u_ref_white;  // white's linear value (0 or 1: sRGB)
uniform vec3 u_tint;
uniform float u_tint_strength;
uniform float u_lift;       // > 0: lift dark backdrop up to this luminance; < 0: cap bright backdrop at -u_lift (0 = off)
uniform float u_saturation;
uniform float u_grain;      // white grain over frosted glass (0 = none)
uniform float u_dye;        // glass dyed u_tint, like water mixed with colour (0 = none)
uniform float u_shape;      // 0: the rounded rect; 1: a drop of liquid, its edge rippling
uniform float u_time;       // seconds, for the drop's ripple
uniform vec2 u_stretch;     // the drop pulled out behind as it moves: the trailing direction, length = how much
uniform float u_rim_width;
uniform float u_rim_intensity;
uniform float u_rim_dir_power;
uniform float u_rim_power;
uniform float u_light_deg;
uniform float u_ao_radius;
uniform float u_ao_intensity;
uniform float u_shadow_radius;
uniform float u_shadow_intensity;
uniform float u_shadow_max;
uniform sampler2D u_mask;
uniform float u_mask_on;
uniform vec4 u_mask_rect;
out vec4 o_color;

#define EDGE_LENS_FALLOFF 2.4
#define EDGE_LENS_REACH 96.0
#define EDGE_LENS_BAND 22.0

float sdRoundRect(vec2 p, vec2 b, float r) {
    vec2 d = abs(p) - b + vec2(r);
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;
}

vec2 sdRoundRectDir(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    if (max(q.x, q.y) < 0.0) {
        return (q.x > q.y) ? vec2(sign(p.x), 0.0) : vec2(0.0, sign(p.y));
    }
    return sign(p) * normalize(max(q, 0.0) + vec2(1e-6));
}

// The drop: a circle filling the box, its edge rippling with a few slow
// waves around it that drift with time (water floating in space).
float sdBlob(vec2 p, vec2 b) {
    float a = atan(p.y, p.x);
    float t = u_time * 1.7;
    float w = 0.050 * sin(3.0 * a + t * 1.10)
            + 0.032 * sin(5.0 * a - t * 1.70 + 0.8)
            + 0.022 * sin(2.0 * a + t * 0.63 + 2.1);
    // Moving, it's a drop pulled into a tail behind it, a little flatter
    // in front (and wobbles back through it when let go).
    float s = length(u_stretch);
    if (s > 0.001) {
        float c = dot(p, u_stretch) / (max(length(p), 0.001) * s);
        float back = max(c, 0.0), front = max(-c, 0.0);
        w += s * (0.95 * back * back - 0.35 * front * front);
    }
    return length(p) - min(b.x, b.y) * 0.9 * (1.0 + w);
}

float sdShape(vec2 p, vec2 b, float r) {
    return u_shape > 0.5 ? sdBlob(p, b) : sdRoundRect(p, b, r);
}

vec2 sdShapeDir(vec2 p, vec2 b, float r) {
    if (u_shape < 0.5) return sdRoundRectDir(p, b, r);
    vec2 e = vec2(0.5, 0.0);
    vec2 g = vec2(sdBlob(p + e.xy, b) - sdBlob(p - e.xy, b), sdBlob(p + e.yx, b) - sdBlob(p - e.yx, b));
    return length(g) > 1e-5 ? normalize(g) : vec2(0.0, 1.0);
}

float profileHeight(float t, float zScale) {
    float n = max(u_shape_n, 1.01);
    float inner = max(1.0 - pow(clamp(1.0 - t, 0.0, 1.0), n), 0.0);
    return pow(inner, 1.0 / n) * zScale;
}

float getHeight(vec2 p, vec2 b, float r, float band, float zScale) {
    float d = sdShape(p, b, r);
    float smoothZone = max(u_edge_smooth, 1.0);
    if (d > smoothZone) return 0.0;
    float t = clamp(max(-d, 0.0) / max(band, 1.0), 0.0, 1.0);
    return profileHeight(t, zScale) * (1.0 - smoothstep(-smoothZone, smoothZone, d));
}

float gradientStep() {
    return u_grad_step;
}

vec2 heightGradient(vec2 p, vec2 b, float r, float band, float zScale) {
    vec2 dir = sdShapeDir(p, b, r);
    float e = gradientStep();
    float hOut = getHeight(p + dir * e, b, r, band, zScale);
    float hIn = getHeight(p - dir * e, b, r, band, zScale);
    return dir * ((hOut - hIn) / (2.0 * e));
}

// HDR: linear light, white above 1.
float refWhite() { return u_ref_white > 0.0 ? u_ref_white : 1.0; }

vec3 toGamma(vec3 c) {
    c = max(c, 0.0);
    return mix(c * 12.92, pow(c, vec3(1.0 / 2.4)) * 1.055 - 0.055, step(vec3(0.0031308), c));
}

vec3 toLinear(vec3 c) {
    c = max(c, 0.0);
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

// Premultiplied, into the target space.
vec4 target(vec3 rgb, float a) {
    float w = refWhite();
    if (w != 1.0 && a > 0.001)
        rgb = toLinear(rgb / a) * w * a;
    return vec4(rgb, a);
}

vec3 sampleSrc(vec2 px) {
    vec2 m = vec2(1.2) / u_src_size;
    vec3 c = texture(u_blur, clamp((px - u_src_origin) / u_src_size, m, vec2(1.0) - m)).rgb;
    float w = refWhite();
    return w == 1.0 ? c : toGamma(c / w);
}

// Four rotated-grid taps, plus six along the refraction footprint where the
// lens compresses the backdrop (see the original's sampleBackdrop()).
vec3 sampleBackdrop(vec2 px, vec2 ext, float spread) {
    vec2 o1 = vec2(0.375, -0.125) * spread;
    vec2 o2 = vec2(0.125, 0.375) * spread;
    vec2 o3 = vec2(-0.375, 0.125) * spread;
    vec2 o4 = vec2(-0.125, -0.375) * spread;
    vec3 sum = sampleSrc(px + o1) + sampleSrc(px + o2) + sampleSrc(px + o3) + sampleSrc(px + o4);
    if (dot(ext, ext) <= 0.0) return sum * 0.25;
    sum += sampleSrc(px + ext * 0.90 + o1) + sampleSrc(px + ext * 0.55 + o2)
         + sampleSrc(px + ext * 0.22 + o3) + sampleSrc(px - ext * 0.22 + o4)
         + sampleSrc(px - ext * 0.55 + o1) + sampleSrc(px - ext * 0.90 + o2);
    return sum * 0.1;
}

float dither(vec2 p) {
    return (fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) / 255.0;
}

void main() {
    vec2 p = vec2(gl_FragCoord.x, mix(gl_FragCoord.y, u_fb_h - gl_FragCoord.y, u_flip_y)) / u_scale;
    vec2 center = u_rect.xy + u_rect.zw * 0.5;
    vec2 local = p - center;
    vec2 box = max(u_rect.zw * 0.5, vec2(1.0));
    float radius = min(u_radius, min(box.x, box.y));
    float d = sdShape(local, box, radius);

    float feather = max(u_edge_smooth, 0.75);
    float outside = smoothstep(-feather, feather, d);
    float inside = 1.0 - outside;

    // Drop shadow (outside the shape only).
    float lightRad = radians(u_light_deg);
    vec2 light2D = vec2(cos(lightRad), -sin(lightRad));
    float align = max(dot(normalize(local + vec2(1e-4)), -light2D), 0.0);
    float maxR = max(u_shadow_max, 1.0);
    float effR = max(min(u_shadow_radius * (0.85 + align * 0.15), maxR), 0.001);
    float effI = u_shadow_intensity * (0.85 + align * 0.15) * smoothstep(0.0, 0.75, u_shadow_radius);
    float umbra = (1.0 - clamp(d / max(effR * 0.40, 0.5), 0.0, 1.0)) * 0.80;
    float pf = 1.0 - clamp(d / effR, 0.0, 1.0);
    float penumbra = pf * pf * pf * (pf * (pf * 6.0 - 15.0) + 10.0) * 0.55;
    float bf = 1.0 - smoothstep(maxR * 0.85, maxR, d);
    float shadowA = clamp((umbra + penumbra) * outside * effI, 0.0, 1.0)
                  * bf * bf * bf * (bf * (bf * 6.0 - 15.0) + 10.0) * (1.0 - step(maxR, d));

    if (inside <= 0.0) {
        o_color = target(vec3(0.03, 0.04, 0.08) * shadowA, shadowA) * u_opacity;
        return;
    }

    // The lens: a dome over a fixed band along the edge.
    float band = max(min(EDGE_LENS_BAND, min(box.x, box.y)), 1.0);
    float lensScale = band / EDGE_LENS_BAND;
    vec2 gradH = heightGradient(local, box, radius, band, u_max_z * lensScale);
    vec3 normal = normalize(vec3(-gradH, 1.0));

    vec2 dispPx = vec2(0.0);
    if (d <= 0.0) {
        vec3 ray = refract(vec3(0.0, 0.0, -1.0), normal, 1.0 / max(u_ior, 1.001));
        if (length(ray) > 0.0001) {
            dispPx = ray.xy / max(-ray.z, 0.15) * u_disp;
        }
    }
    float edgeT = clamp(1.0 - max(-d, 0.0) / band, 0.0, 1.0);
    float lensShape = pow(edgeT, EDGE_LENS_FALLOFF);
    dispPx *= lensShape * lensScale;
    float dispLen = length(dispPx);
    vec2 dispDir = dispPx / max(dispLen, 1e-4);
    if (dispLen > EDGE_LENS_REACH) {
        dispLen = EDGE_LENS_REACH;
        dispPx = dispDir * EDGE_LENS_REACH;
    }
    float shapeRate = EDGE_LENS_FALLOFF * pow(edgeT, EDGE_LENS_FALLOFF - 1.0) / band;
    float footprint = 2.0 * dispLen * (shapeRate + 1.0 / band);
    vec2 ext = footprint > 2.0 ? dispDir * (min(footprint, 64.0) * 0.5) : vec2(0.0);

    float edgeProx = 1.0 - smoothstep(0.0, feather * 4.0, -d);
    vec3 refracted = sampleBackdrop(u_origin + p + dispPx, ext, mix(0.75, 2.5, edgeProx));

    float luma = dot(refracted, vec3(0.299, 0.587, 0.114));
    refracted = max(mix(vec3(luma), refracted, u_saturation), 0.0);
    vec3 base = mix(refracted, u_tint, u_tint_strength);
    // Dyed: the colour's hue throughout, the backdrop's light and shade
    // still coming through it, and a little of the backdrop itself.
    if (u_dye > 0.0) {
        float l = dot(refracted, vec3(0.2126, 0.7152, 0.0722));
        base = mix(refracted, u_tint * (0.55 + 0.6 * l), u_dye);
    }
    // Light mode, like Tahoe's clear glass: rather than frosting everything,
    // lift only what's too dark for dark text, toward the tint, keeping some
    // of its detail; bright backdrop shows through as it is.
    if (u_lift > 0.0) {
        vec3 w = vec3(0.2126, 0.7152, 0.0722);
        float l = dot(base, w);
        float want = l < u_lift ? u_lift - (u_lift - l) * 0.35 : l;
        float k = clamp((want - l) / max(dot(u_tint, w) - l, 1e-3), 0.0, 1.0);
        base = mix(base, u_tint, k);
    } else if (u_lift < 0.0) {
        // Veil glass, under white text: the same, the other way round. Only
        // backdrop brighter than the cap is darkened toward the (dark) tint,
        // keeping 20% of its detail; darker backdrop shows through as it is.
        vec3 w = vec3(0.2126, 0.7152, 0.0722);
        float cap = -u_lift;
        float l = dot(base, w);
        float want = l > cap ? cap + (l - cap) * 0.2 : l;
        float k = clamp((l - want) / max(l - dot(u_tint, w), 1e-3), 0.0, 1.0);
        base = mix(base, u_tint, k);
    }

    // Frosted grain: fixed white specks, one per device pixel.
    if (u_grain > 0.0) {
        float n = fract(sin(dot(floor(gl_FragCoord.xy), vec2(12.9898, 78.233))) * 43758.5453);
        base = mix(base, vec3(1.0), n * n * u_grain * 2.0);
    }

    // Lighting: directional rim, inner AO where the rim isn't lit.
    vec3 lightDir = normalize(vec3(cos(lightRad), sin(lightRad), 0.38));
    float aoMask = 1.0 - smoothstep(0.0, max(u_ao_radius, 0.001), -d);
    float edgeBand = (1.0 - smoothstep(0.0, max(u_rim_width, 0.001), abs(d))) * step(0.0005, u_rim_width);
    float rimFresnel = pow(max(1.0 - max(normal.z, 0.0), 0.0), max(u_rim_power, 0.001));
    float lightMask = pow(abs(dot(normal, lightDir)), max(u_rim_dir_power, 1.0));
    base *= 1.0 - aoMask * u_ao_intensity * (1.0 - lightMask);
    float rim = mix(pow(edgeBand, 0.85), rimFresnel, 0.55) * edgeBand * lightMask * u_rim_intensity;
    vec3 added = vec3(rim + edgeBand * 0.008);
    vec3 lit = base + added - base * added;
    float mx = max(lit.r, max(lit.g, lit.b));
    if (mx > 1.0) lit /= mx;
    lit = max(lit, 0.0);

    // Premultiplied glass over its shadow.
    float shadowC = shadowA * (1.0 - inside);
    vec3 rgb = lit * inside + vec3(0.03, 0.04, 0.08) * shadowC;
    float a = inside + shadowC;
    // Faded with its surface (opening and closing): premultiplied, so all of it.
    o_color = target(max(rgb + dither(p) * a, 0.0), a) * u_opacity;
    if (u_mask_on > 0.5) {
        vec2 m = (p - u_mask_rect.xy) / max(u_mask_rect.zw, vec2(1.0));
        bool within = all(greaterThanEqual(m, vec2(0.0))) && all(lessThanEqual(m, vec2(1.0)));
        o_color *= smoothstep(0.2, 0.35, within ? texture(u_mask, m).a : 0.0);
    }
}
)GLSL";
