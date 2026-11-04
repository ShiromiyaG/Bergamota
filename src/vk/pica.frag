#version 450
// The PICA's fragment stage as one shader: it reads the registers as the
// game wrote them and works the combiners (TEV) out from them, so no state
// needs a shader of its own; the fragment lighting from a block the CPU
// decoded (pica_lighting.h) and its tables. A pipeline made for a state's
// shape (draw.cpp, shape_of) has it in the constants below instead, and the
// driver leaves out all the shape does not use. After Zakuro's tev.rs and
// lighting.rs (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv0;
layout(location = 2) in vec2 v_uv1;
layout(location = 3) in vec2 v_uv2;
layout(location = 4) in float v_w0;
layout(location = 5) in vec4 v_quaternion;
layout(location = 6) in vec3 v_view;
layout(location = 7) in float v_depth;

#ifndef SHADOW
layout(location = 0) out vec4 out_color;
#endif

layout(std140, set = 0, binding = 0) uniform State {
    uvec4 regs[16];
    vec4 depth;
} s;
layout(set = 0, binding = 1) uniform sampler2D textures[3];
layout(set = 0, binding = 10) uniform sampler2D cube_faces[5]; // -X, +Y, -Y, +Z, -Z
#ifdef SHADOW
// the shadow map, a word a pixel: its depth (24 bits) over its intensity
layout(set = 0, binding = 11, r32ui) uniform coherent uimage2D shadow_map;
#endif

// the lighting, as LightingBlock in pica_lighting.h lays it out
struct Light {
    vec4 specular0, specular1, diffuse, ambient, position, direction, spot, distance;
    uvec4 flags; // directional, two-sided, geometric 0/1, distance, spotlight, shadowed; its number
};
layout(std140, set = 0, binding = 2) uniform Lighting {
    uvec4 head;     // configuration, lights on, bump, shadow
    uvec4 switches; // fresnel primary, fresnel secondary, clamp highlights, needs half (1) / view (2)
    uvec4 lookups[7]; // on, input, absolute, scale bits: D0, D1, fresnel, red, green, blue, spotlight
    vec4 global_ambient;
    Light lights[8];
} l;
// the 24 tables, each entry a value and the step to the next
layout(std430, set = 0, binding = 3) readonly buffer Tables { vec2 entries[]; } t;

// the shape of the state, in a pipeline made for it (draw.cpp, shape_of):
// each stage's sources, operands, operations and scales, the combiner
// buffer's updates and the fog, the alpha test's function, the textures'
// configuration and parameters, the procedural texture's, the lighting's
// head and switches and each table read's on, input and absolute
layout(constant_id = 0) const bool SPECIALIZED = false;
layout(constant_id = 1) const uint K1 = 0u;
layout(constant_id = 2) const uint K2 = 0u;
layout(constant_id = 3) const uint K3 = 0u;
layout(constant_id = 4) const uint K4 = 0u;
layout(constant_id = 5) const uint K5 = 0u;
layout(constant_id = 6) const uint K6 = 0u;
layout(constant_id = 7) const uint K7 = 0u;
layout(constant_id = 8) const uint K8 = 0u;
layout(constant_id = 9) const uint K9 = 0u;
layout(constant_id = 10) const uint K10 = 0u;
layout(constant_id = 11) const uint K11 = 0u;
layout(constant_id = 12) const uint K12 = 0u;
layout(constant_id = 13) const uint K13 = 0u;
layout(constant_id = 14) const uint K14 = 0u;
layout(constant_id = 15) const uint K15 = 0u;
layout(constant_id = 16) const uint K16 = 0u;
layout(constant_id = 17) const uint K17 = 0u;
layout(constant_id = 18) const uint K18 = 0u;
layout(constant_id = 19) const uint K19 = 0u;
layout(constant_id = 20) const uint K20 = 0u;
layout(constant_id = 21) const uint K21 = 0u;
layout(constant_id = 22) const uint K22 = 0u;
layout(constant_id = 23) const uint K23 = 0u;
layout(constant_id = 24) const uint K24 = 0u;
layout(constant_id = 25) const uint K25 = 0u;
layout(constant_id = 26) const uint K26 = 0u;
layout(constant_id = 27) const uint K27 = 0u;
layout(constant_id = 28) const uint K28 = 0u;
layout(constant_id = 29) const uint K29 = 0u;
layout(constant_id = 30) const uint K30 = 0u;
layout(constant_id = 31) const uint K31 = 0u;
layout(constant_id = 32) const uint K32 = 0u;
layout(constant_id = 33) const uint K33 = 0u;
layout(constant_id = 34) const uint K34 = 0u;
layout(constant_id = 35) const uint K35 = 0u;
layout(constant_id = 36) const uint K36 = 0u;
layout(constant_id = 37) const uint K37 = 0u;
layout(constant_id = 38) const uint K38 = 0u;
layout(constant_id = 39) const uint K39 = 0u;
layout(constant_id = 40) const uint K40 = 0u;
layout(constant_id = 41) const uint K41 = 0u;
layout(constant_id = 42) const uint K42 = 0u;
layout(constant_id = 43) const uint K43 = 0u;
layout(constant_id = 44) const uint K44 = 0u;
layout(constant_id = 45) const uint K45 = 0u;
layout(constant_id = 46) const uint K46 = 0u;

uint shape_sources(int i) { switch (i) { case 0: return K1; case 1: return K2; case 2: return K3; case 3: return K4; case 4: return K5; default: return K6; } }
uint shape_operands(int i) { switch (i) { case 0: return K7; case 1: return K8; case 2: return K9; case 3: return K10; case 4: return K11; default: return K12; } }
uint shape_operations(int i) { switch (i) { case 0: return K13; case 1: return K14; case 2: return K15; case 3: return K16; case 4: return K17; default: return K18; } }
uint shape_scales(int i) { switch (i) { case 0: return K19; case 1: return K20; case 2: return K21; case 3: return K22; case 4: return K23; default: return K24; } }
uint shape_params(int i) { switch (i) { case 0: return K28; case 1: return K29; default: return K30; } }
uint shape_head(int i) { switch (i) { case 0: return K32; case 1: return K33; case 2: return K34; default: return K35; } }
uint shape_switches(int i) { switch (i) { case 0: return K36; case 1: return K37; case 2: return K38; default: return K39; } }
uint shape_lookups(int i) { switch (i) { case 0: return K40; case 1: return K41; case 2: return K42; case 3: return K43; case 4: return K44; case 5: return K45; default: return K46; } }

// the registers, by the slots renderer.cpp puts them in
uint reg(int slot) { return s.regs[slot >> 2][slot & 3]; }
const int TEV = 0, TEV_UPDATE = 30, TEV_BUFFER = 31, ALPHA_TEST = 32, TEX_CONFIG = 33, TEX_PARAMS = 34, TEX_BORDER = 37,
          FOG_COLOR = 40, PROCTEX = 41, TEX_SHADOW = 47, SHADOW_OUTPUT = 48,
          TEX_FLIP = 63; // the units whose texture's rows go the other way (draw.cpp) // the procedural texture's six: configuration, noise u, v, frequency, table, offset

// what the shape fixes, from the constants or the blocks
uint stage_reg(int stage, int k) {
    if (SPECIALIZED) return k == 0 ? shape_sources(stage) : k == 1 ? shape_operands(stage) : k == 2 ? shape_operations(stage) : shape_scales(stage);
    return reg(TEV + stage * 5 + k);
}
uint tev_update() { return SPECIALIZED ? K25 : reg(TEV_UPDATE); }
uint alpha_test() { return SPECIALIZED ? (K26 & 0x71u) | (reg(ALPHA_TEST) & 0xFF00u) : reg(ALPHA_TEST); } // the reference as it is
uint tex_config() { return SPECIALIZED ? K27 : reg(TEX_CONFIG); }
uint tex_params(int unit) { return SPECIALIZED ? shape_params(unit) : reg(TEX_PARAMS + unit); }
uint proctex_config() { return SPECIALIZED ? K31 : reg(PROCTEX); }
uint light_head(int k) { return SPECIALIZED ? shape_head(k) : l.head[k]; }
uint light_switch(int k) { return SPECIALIZED ? shape_switches(k) : l.switches[k]; }
bool lookup_on(int slot) { return SPECIALIZED ? (shape_lookups(slot) & 1u) != 0u : l.lookups[slot].x != 0u; }
uint lookup_input(int slot) { return SPECIALIZED ? (shape_lookups(slot) >> 1) & 7u : l.lookups[slot].y; }
bool lookup_absolute(int slot) { return SPECIALIZED ? (shape_lookups(slot) & 16u) != 0u : l.lookups[slot].z != 0u; }
const int D0 = 0, D1 = 1, FRESNEL = 3, REFLECT_BLUE = 4, REFLECT_GREEN = 5, REFLECT_RED = 6, SPOTLIGHT = 8, DISTANCE = 16;
const int LOOKUP_D0 = 0, LOOKUP_D1 = 1, LOOKUP_FRESNEL = 2, LOOKUP_RED = 3, LOOKUP_GREEN = 4, LOOKUP_BLUE = 5, LOOKUP_SPOT = 6;

float table(int which, uint entry, float delta) {
    vec2 e = t.entries[which * 256 + int(entry)];
    return e.x + e.y * delta;
}

// v turned by the unit quaternion q
vec3 rotate(vec4 q, vec3 v) {
    vec3 inner = cross(q.xyz, v);
    return v + 2.0 * cross(q.xyz, inner + v * q.w);
}

// a table read: the dot product its input names, as an index, absolute or
// signed (wrapping into the upper half), scaled
float lookup(int slot, int which, float result, bool two_sided) {
    uint entry;
    float delta;
    if (lookup_absolute(slot)) {
        result = two_sided ? abs(result) : max(result, 0.0);
        float e = clamp(floor(result * 256.0), 0.0, 255.0);
        entry = uint(e);
        delta = result * 256.0 - e;
    } else {
        float e = clamp(floor(result * 128.0), -128.0, 127.0);
        entry = uint(int(e)) & 0xFFu;
        delta = result * 128.0 - e;
    }
    return uintBitsToFloat(l.lookups[slot].w) * table(which, entry, delta);
}

vec4 quantize(vec4 c) { return floor(clamp(c, 0.0, 1.0) * 255.0) / 255.0; }

// the primary (diffuse) and secondary (specular) fragment colors
void light(vec4 tex[4], out vec4 primary, out vec4 secondary) {
    uint config = light_head(0), count = light_head(1), bump = light_head(2), shadow_cfg = light_head(3);
    bool needs_half = (light_switch(3) & 1u) != 0u, needs_view = (light_switch(3) & 2u) != 0u;
    float length2 = dot(v_quaternion, v_quaternion);
    vec4 q = length2 > 0.0 ? v_quaternion * inversesqrt(length2) : vec4(0.0, 0.0, 0.0, 1.0);

    bool shadowing = (shadow_cfg & 1u) != 0u;
    vec4 shadow = vec4(1.0);
    if (shadowing) {
        shadow = tex[(shadow_cfg >> 4) & 3u];
        if ((shadow_cfg & 0x100u) != 0u) shadow = 1.0 - shadow;
    }
    vec3 surface_normal = vec3(0.0, 0.0, 1.0), surface_tangent = vec3(1.0, 0.0, 0.0);
    uint bump_unit = (bump >> 4) & 3u;
    if ((bump & 3u) == 1u) {
        surface_normal = tex[bump_unit].rgb * 2.0 - 1.0;
        if ((bump & 0x100u) != 0u) surface_normal.z = sqrt(max(1.0 - dot(surface_normal.xy, surface_normal.xy), 0.0));
    } else if ((bump & 3u) == 2u) {
        surface_tangent = tex[bump_unit].rgb * 2.0 - 1.0;
    }
    vec3 normal = rotate(q, surface_normal);
    // only the last configuration has a table that reads the tangent
    vec3 tangent = config == 8u ? rotate(q, surface_tangent) : surface_tangent;
    vec3 norm_view = needs_view ? normalize(v_view) : vec3(0.0);

    vec4 diffuse_sum = vec4(0.0, 0.0, 0.0, 1.0), specular_sum = vec4(0.0, 0.0, 0.0, 1.0);
    for (uint i = 0u; i < count; i++) {
        Light li = l.lights[i];
        uint flags = li.flags.x, number = li.flags.y;
        bool two_sided = (flags & 2u) != 0u;
        vec3 light_vector = (flags & 1u) != 0u ? li.direction.xyz : normalize(li.position.xyz + v_view);
        vec3 half_v = needs_half ? norm_view + light_vector : vec3(0.0);
        vec3 half_unit = needs_half ? normalize(half_v) : vec3(0.0);

        float distance = 1.0;
        if ((flags & 16u) != 0u) {
            vec3 offset = -v_view - li.position.xyz;
            float place = clamp(li.distance.y * length(offset) + li.distance.x, 0.0, 1.0);
            float e = clamp(floor(place * 256.0), 0.0, 255.0);
            distance = table(DISTANCE + int(number), uint(e), place * 256.0 - e);
        }

        // the dot products the tables can take
        float inputs[6];
        inputs[0] = dot(normal, half_unit);
        inputs[1] = dot(norm_view, half_unit);
        inputs[2] = dot(normal, norm_view);
        inputs[3] = dot(light_vector, normal);
        inputs[4] = dot(light_vector, li.spot.xyz);
        inputs[5] = config == 8u ? dot(half_unit - normal * dot(normal, half_unit), tangent) : 0.0;
        #define VALUE(slot, which) lookup(slot, which, lookup_input(slot) < 6u ? inputs[lookup_input(slot)] : 0.0, two_sided)

        float spot = (flags & 32u) != 0u ? VALUE(LOOKUP_SPOT, SPOTLIGHT + int(number)) : 1.0;
        float d0 = lookup_on(LOOKUP_D0) ? VALUE(LOOKUP_D0, D0) : 1.0;
        float red = lookup_on(LOOKUP_RED) ? VALUE(LOOKUP_RED, REFLECT_RED) : 1.0;
        vec3 reflect_rgb = vec3(red, lookup_on(LOOKUP_GREEN) ? VALUE(LOOKUP_GREEN, REFLECT_GREEN) : red,
                                lookup_on(LOOKUP_BLUE) ? VALUE(LOOKUP_BLUE, REFLECT_BLUE) : red);
        float d1 = lookup_on(LOOKUP_D1) ? VALUE(LOOKUP_D1, D1) : 1.0;
        vec3 specular0 = li.specular0.rgb * d0;
        vec3 specular1 = d1 * reflect_rgb * li.specular1.rgb;

        // only the last light applies fresnel
        if (i == count - 1u && lookup_on(LOOKUP_FRESNEL)) {
            float fresnel = VALUE(LOOKUP_FRESNEL, FRESNEL);
            if (light_switch(0) != 0u) diffuse_sum.a = fresnel;
            if (light_switch(1) != 0u) specular_sum.a = fresnel;
        }
        #undef VALUE

        float facing = dot(light_vector, normal);
        facing = two_sided ? abs(facing) : max(facing, 0.0);
        float highlights = light_switch(2) != 0u && facing == 0.0 ? 0.0 : 1.0;
        if ((flags & 12u) != 0u) {
            float len = dot(half_v, half_v);
            float factor = len == 0.0 ? 0.0 : min(facing / len, 1.0);
            if ((flags & 4u) != 0u) specular0 *= factor;
            if ((flags & 8u) != 0u) specular1 *= factor;
        }

        bool shadowed = (flags & 64u) != 0u;
        vec3 shadow_primary = shadowing && (shadow_cfg & 0x200u) != 0u && shadowed ? shadow.rgb : vec3(1.0);
        vec3 shadow_secondary = shadowing && (shadow_cfg & 0x400u) != 0u && shadowed ? shadow.rgb : vec3(1.0);
        vec3 diffuse = (li.diffuse.rgb * facing + li.ambient.rgb) * distance * spot;
        vec3 specular = (specular0 + specular1) * highlights * distance * spot;
        diffuse_sum.rgb += diffuse * shadow_primary;
        specular_sum.rgb += specular * shadow_secondary;
    }
    if (shadowing && (shadow_cfg & 0x800u) != 0u) {
        if (light_switch(0) != 0u) diffuse_sum.a *= shadow.a;
        if (light_switch(1) != 0u) specular_sum.a *= shadow.a;
    }
    diffuse_sum.rgb += l.global_ambient.rgb;
    // the unit hands the combiners 8-bit colors
    primary = quantize(diffuse_sum);
    secondary = quantize(specular_sum);
}

// where the tables are in t.entries, after lighting's 24 of 256
const int FOG_TABLE = 24 * 256, PROCTEX_MAPS = FOG_TABLE + 128, PROCTEX_COLORS = PROCTEX_MAPS + 3 * 128,
          PROCTEX_STEPS = PROCTEX_COLORS + 2 * 256;

// the procedural texture, texture 3, made up from its coordinates (Zakuro's
// raster.frag, as its software rasterizer has it)
// a map read at a coordinate from 0 to 1
precise float proctex_lookup(int map, float coordinate) {
    float at = coordinate * 128.0;
    int entry = min(int(max(at, 0.0)), 127);
    vec2 value = t.entries[PROCTEX_MAPS + map * 128 + entry];
    return value.x + (at - float(entry)) * value.y;
}

vec4 proctex_color(uint entry, int base) {
    return vec4(t.entries[base + int(entry) * 2], t.entries[base + int(entry) * 2 + 1]);
}

// a pseudo-random value from -1 to 1 for a point of the noise's grid
float proctex_random(uint x, uint y) {
    const uint rows[16] = uint[16](0u, 4u, 10u, 8u, 4u, 9u, 7u, 12u, 5u, 15u, 13u, 14u, 11u, 15u, 2u, 11u);
    const uint mixes[16] = uint[16](10u, 2u, 15u, 8u, 0u, 7u, 4u, 5u, 5u, 13u, 2u, 6u, 13u, 9u, 3u, 14u);
    uint u = (((x % 9u + 2u) * 3u) & 0xFu) ^ rows[(x / 9u) & 0xFu];
    uint v = (((y % 9u + 2u) * 3u) & 0xFu) ^ rows[(y / 9u) & 0xFu];
    if ((u & 3u) == 1u) v += 4u;
    v ^= (u & 1u) * 6u;
    v += 10u + u;
    v &= 0xFu;
    v ^= mixes[u];
    return -1.0 + float(v) * 2.0 / 15.0;
}

float proctex_shift(float other, uint mode, uint clamping) {
    float amount = clamping == 3u ? 1.0 : 0.5;
    int o = int(other);
    if (mode == 1u) return amount * float((o / 2) % 2);
    if (mode == 2u) return amount * float(((o + 1) / 2) % 2);
    return 0.0;
}

precise float proctex_clamp(float c, uint mode) {
    switch (mode) {
    case 0u: return c > 1.0 ? 0.0 : c;
    case 1u: return min(c, 1.0);
    case 2u: return c - floor(c);
    case 3u: {
        int whole = int(c);
        float part = c - float(whole);
        return (whole % 2) == 0 ? part : 1.0 - part;
    }
    case 4u: return c <= 0.5 ? 0.0 : 1.0;
    default: return clamp(c, 0.0, 1.0);
    }
}

precise float proctex_combine(float u, float v, uint function) {
    float len = sqrt(u * u + v * v);
    switch (function) {
    case 0u: return u;
    case 1u: return u * u;
    case 2u: return v;
    case 3u: return v * v;
    case 4u: return (u + v) * 0.5;
    case 5u: return (u * u + v * v) * 0.5;
    case 6u: return min(len, 1.0);
    case 7u: return min(u, v);
    case 8u: return max(u, v);
    case 9u: return min(((u + v) * 0.5 + len) * 0.5, 1.0);
    default: return 0.0;
    }
}

precise vec4 procedural(vec2 uv) {
    uint config = proctex_config();
    uint noise_u = reg(PROCTEX + 1), noise_v = reg(PROCTEX + 2), lut = reg(PROCTEX + 4);
    uvec2 clamps = uvec2(config & 7u, (config >> 3) & 7u);
    float u = abs(uv.x);
    float v = abs(uv.y);
    vec2 shifts = vec2(proctex_shift(v, (config >> 16) & 3u, clamps.x), proctex_shift(u, (config >> 18) & 3u, clamps.y));
    if ((config & (1u << 15)) != 0u) {
        vec2 frequency = unpackHalf2x16(reg(PROCTEX + 3));
        float x = 9.0 * frequency.x * abs(u + float(noise_u >> 16) / 4096.0);
        float y = 9.0 * frequency.y * abs(v + float(noise_v >> 16) / 4096.0);
        uint xi = uint(x);
        uint yi = uint(y);
        float xf = x - float(xi);
        float yf = y - float(yi);
        float g0 = proctex_random(xi, yi) * (xf + yf);
        float g1 = proctex_random(xi + 1u, yi) * (xf + yf - 1.0);
        float g2 = proctex_random(xi, yi + 1u) * (xf + yf - 1.0);
        float g3 = proctex_random(xi + 1u, yi + 1u) * (xf + yf - 2.0);
        float sx = proctex_lookup(0, xf);
        float ty = proctex_lookup(0, yf);
        float noise = mix(mix(g0, g1, sx), mix(g2, g3, sx), ty);
        u = abs(u + noise * float(int(noise_u << 16) >> 16) / 4095.0);
        v = abs(v + noise * float(int(noise_v << 16) >> 16) / 4095.0);
    }
    u = proctex_clamp(u + shifts.x, clamps.x);
    v = proctex_clamp(v + shifts.y, clamps.y);

    float coordinate = proctex_lookup(1, proctex_combine(u, v, (config >> 6) & 0xFu));
    float width = float((lut >> 11) & 0xFFu);
    float at = float(reg(PROCTEX + 5) & 0xFFu) + coordinate * max(width - 1.0, 0.0);
    uint filtering = lut & 7u;
    vec4 color;
    if (filtering == 1u || filtering == 3u || filtering == 5u) {
        uint entry = min(uint(max(at, 0.0)), 255u);
        color = proctex_color(entry, PROCTEX_COLORS) + (at - float(entry)) * proctex_color(entry, PROCTEX_STEPS);
    } else {
        color = proctex_color(min(uint(max(round(at), 0.0)), 255u), PROCTEX_COLORS);
    }
    color = clamp(color / 255.0, 0.0, 1.0);
    // alpha of its own skips the color table, the map gives it
    if ((config & (1u << 14)) != 0u) {
        color.a = clamp(proctex_lookup(2, proctex_combine(u, v, (config >> 10) & 0xFu)), 0.0, 1.0);
    }
    return color;
}

vec4 unpack(uint c) { return vec4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24) / 255.0; }

// a texture unit's color: v up as the PICA has it, and the border color
// outside when the unit clamps to it
vec4 texel(int unit, vec2 uv) {
    if ((tex_config() & (1u << unit)) == 0u) return vec4(0.0);
    uint params = tex_params(unit);
    bool border_s = ((params >> 12) & 3u) == 1u, border_t = ((params >> 8) & 3u) == 1u;
    if ((border_s && (uv.x < 0.0 || uv.x > 1.0)) || (border_t && (uv.y < 0.0 || uv.y > 1.0))) {
        return unpack(reg(TEX_BORDER + unit));
    }
    bool turned = ((reg(TEX_FLIP) >> unit) & 1u) != 0u;
    return texture(textures[unit], vec2(uv.x, turned ? uv.y : 1.0 - uv.y));
}

// texture 0 as a cube map: the face the direction (u, v, w) points at most,
// where on it (the other components over the largest), and how far along
int cube_face(vec3 d, out vec2 uv, out float far) {
    vec3 a = abs(d);
    int face;
    vec2 st;
    if (a.x > a.y && a.x > a.z) {
        face = d.x > 0.0 ? 0 : 1;
        st = vec2(-d.z, d.x > 0.0 ? -d.y : d.y) / d.x;
        far = a.x;
    } else if (a.y > a.z) {
        face = d.y > 0.0 ? 2 : 3;
        st = vec2(d.y > 0.0 ? d.x : -d.x, d.z) / d.y;
        far = a.y;
    } else {
        face = d.z > 0.0 ? 4 : 5;
        st = vec2(d.x, d.z > 0.0 ? -d.y : d.y) / d.z;
        far = a.z;
    }
    uv = st * 0.5 + 0.5;
    return face;
}

vec4 face_texel(int face, vec2 at) {
    switch (face) {
    case 0: return texture(textures[0], at);
    case 1: return texture(cube_faces[0], at);
    case 2: return texture(cube_faces[1], at);
    case 3: return texture(cube_faces[2], at);
    case 4: return texture(cube_faces[3], at);
    default: return texture(cube_faces[4], at);
    }
}

// one texel as it is, no filtering (a shadow map's depths cannot be mixed)
vec4 face_fetch(int face, vec2 uv) {
    ivec2 size = face == 0 ? textureSize(textures[0], 0) : textureSize(cube_faces[max(face - 1, 0)], 0);
    ivec2 at = clamp(ivec2(floor(vec2(uv.x, 1.0 - uv.y) * vec2(size))), ivec2(0), size - 1);
    switch (face) {
    case 0: return texelFetch(textures[0], at, 0);
    case 1: return texelFetch(cube_faces[0], at, 0);
    case 2: return texelFetch(cube_faces[1], at, 0);
    case 3: return texelFetch(cube_faces[2], at, 0);
    case 4: return texelFetch(cube_faces[3], at, 0);
    default: return texelFetch(cube_faces[4], at, 0);
    }
}

vec4 cube_texel(vec3 d) {
    vec2 uv;
    float far;
    int face = cube_face(d, uv, far);
    return face_texel(face, vec2(uv.x, 1.0 - uv.y));
}

// texture 0 as a shadow map (2D, type 2, or cube, 4): its texel's depth
// (alpha, blue and green, high to low) against the fragment's, less the
// bias: the texel's intensity (red) where the fragment is in front of what
// the map saw, none behind
vec4 shadow_texel(bool cube) {
    uint config = reg(TEX_SHADOW);
    vec2 uv;
    float far;
    int face = 0;
    if (cube) {
        face = cube_face(vec3(v_uv0, v_w0), uv, far);
    } else {
        uv = (config & 1u) != 0u ? v_uv0 : v_uv0 / v_w0; // orthographic, or over w
        far = abs(v_w0);
    }
    int z = int(min(far, 1.0) * 16777215.0) - int(((config >> 1) & 0x7FFFFFu) << 1);
    uvec4 c = uvec4(round(face_fetch(face, uv) * 255.0));
    int stored = int(c.a << 16 | c.b << 8 | c.g);
    return vec4(stored >= z ? float(c.r) / 255.0 : 0.0);
}

vec3 color_operand(vec4 v, uint op) {
    switch (op) {
    case 1u: return 1.0 - v.rgb;
    case 2u: return vec3(v.a);
    case 3u: return vec3(1.0 - v.a);
    case 4u: return vec3(v.r);
    case 5u: return vec3(1.0 - v.r);
    case 8u: return vec3(v.g);
    case 9u: return vec3(1.0 - v.g);
    case 12u: return vec3(v.b);
    case 13u: return vec3(1.0 - v.b);
    default: return v.rgb;
    }
}

float alpha_operand(vec4 v, uint op) {
    switch (op) {
    case 0u: return v.a;
    case 1u: return 1.0 - v.a;
    case 2u: return v.r;
    case 3u: return 1.0 - v.r;
    case 4u: return v.g;
    case 5u: return 1.0 - v.g;
    case 6u: return v.b;
    default: return 1.0 - v.b;
    }
}

vec3 combine3(uint op, vec3 a, vec3 b, vec3 c) {
    switch (op) {
    case 0u: return a;
    case 1u: return a * b;
    case 2u: return min(a + b, 1.0);
    case 3u: return clamp(a + b - 0.5, 0.0, 1.0);
    case 4u: return a * c + b * (1.0 - c);
    case 5u: return max(a - b, 0.0);
    case 6u: case 7u: return vec3(clamp(dot(a * 2.0 - 1.0, b * 2.0 - 1.0), 0.0, 1.0));
    case 8u: return min(a * b + c, 1.0);
    default: return min(a + b, 1.0) * c;
    }
}

float combine1(uint op, float a, float b, float c) {
    switch (op) {
    case 0u: return a;
    case 1u: return a * b;
    case 2u: return min(a + b, 1.0);
    case 3u: return clamp(a + b - 0.5, 0.0, 1.0);
    case 4u: return a * c + b * (1.0 - c);
    case 5u: return max(a - b, 0.0);
    case 6u: case 7u: return a;
    case 8u: return min(a * b + c, 1.0);
    default: return min(a + b, 1.0) * c;
    }
}

float scale(uint raw) { return raw == 1u ? 2.0 : raw == 2u ? 4.0 : 1.0; }

bool compare(uint func, float a, float b) {
    switch (func) {
    case 0u: return false;
    case 1u: return true;
    case 2u: return a == b;
    case 3u: return a != b;
    case 4u: return a < b;
    case 5u: return a <= b;
    case 6u: return a > b;
    default: return a >= b;
    }
}

// a combiner stage; the buffer lags a stage: the first reads zero, the
// second the configured color, a stage's update reaches the stage after next
void tev_stage(int i, vec4 primary, vec4 frag_primary, vec4 frag_secondary, vec4 tex[4], uint update, inout vec4 previous,
               inout vec4 held, inout vec4 next_buffer) {
    uint src = stage_reg(i, 0), ops = stage_reg(i, 1), comb = stage_reg(i, 2);
    vec4 konst = unpack(reg(TEV + i * 5 + 3));
    uint sc = stage_reg(i, 4);
    vec4 in_[6];
    for (int k = 0; k < 6; k++) {
        uint which = (src >> ((k < 3 ? k * 4 : 16 + (k - 3) * 4))) & 0xFu;
        vec4 v;
        switch (which) {
        case 0u: v = primary; break;
        case 1u: v = frag_primary; break;
        case 2u: v = frag_secondary; break;
        case 3u: v = tex[0]; break;
        case 4u: v = tex[1]; break;
        case 5u: v = tex[2]; break;
        case 6u: v = tex[3]; break;
        case 13u: v = held; break;
        case 14u: v = konst; break;
        default: v = previous; break;
        }
        in_[k] = v;
    }
    vec3 rgb = combine3(comb & 0xFu, color_operand(in_[0], ops & 0xFu), color_operand(in_[1], (ops >> 4) & 0xFu),
                        color_operand(in_[2], (ops >> 8) & 0xFu));
    float a = (comb & 0xFu) == 7u ? rgb.r
                                  : combine1((comb >> 16) & 0xFu, alpha_operand(in_[3], (ops >> 12) & 7u),
                                             alpha_operand(in_[4], (ops >> 16) & 7u), alpha_operand(in_[5], (ops >> 20) & 7u));
    previous = clamp(vec4(rgb * scale(sc & 3u), a * scale((sc >> 16) & 3u)), 0.0, 1.0);
    held = next_buffer;
    if (i < 4) {
        if ((update & (0x100u << i)) != 0u) next_buffer.rgb = previous.rgb;
        if ((update & (0x1000u << i)) != 0u) next_buffer.a = previous.a;
    }
}

void main() {
    // texture 2 reads coordinate set 1 or 2
    vec4 tex[4];
    // texture 0 projected (its type, 3): the coordinates over their w; a
    // cube map (1): (u, v, w) a direction
    uint type0 = (tex_params(0) >> 28) & 7u;
    bool on0 = (tex_config() & 1u) != 0u;
    if (on0 && type0 == 1u) tex[0] = cube_texel(vec3(v_uv0, v_w0));
    else if (on0 && (type0 == 2u || type0 == 4u)) tex[0] = shadow_texel(type0 == 4u);
    else tex[0] = texel(0, type0 == 3u ? v_uv0 / v_w0 : v_uv0);
    // an outline's edge filter (draw.cpp): its neighbors brought nearer
    // texture 0's coordinates, by as much as depth.w says
    float nearer = s.depth.w;
    vec2 uv1 = nearer == 1.0 ? v_uv1 : v_uv0 + (v_uv1 - v_uv0) * nearer;
    vec2 uv2 = nearer == 1.0 ? v_uv2 : v_uv0 + (v_uv2 - v_uv0) * nearer;
    tex[1] = texel(1, uv1);
    tex[2] = texel(2, (tex_config() & (1u << 13)) != 0u ? uv1 : uv2);
    // the procedural one reads coordinate set 0, 1 or 2
    uint coordinates = min((tex_config() >> 8) & 3u, 2u);
    tex[3] = (tex_config() & (1u << 10)) != 0u ? procedural(coordinates == 0u ? v_uv0 : coordinates == 1u ? v_uv1 : v_uv2) : vec4(0.0);
    vec4 primary = v_color;
    // without lighting the primary fragment color is the vertex's, and
    // there is no specular term
    vec4 frag_primary = primary, frag_secondary = vec4(0.0, 0.0, 0.0, 1.0);
    if (light_head(1) != 0u) light(tex, frag_primary, frag_secondary);

    vec4 previous = primary;
    vec4 held = vec4(0.0);
    vec4 next_buffer = unpack(reg(TEV_BUFFER));
    uint update = tev_update();
    tev_stage(0, primary, frag_primary, frag_secondary, tex, update, previous, held, next_buffer);
    tev_stage(1, primary, frag_primary, frag_secondary, tex, update, previous, held, next_buffer);
    tev_stage(2, primary, frag_primary, frag_secondary, tex, update, previous, held, next_buffer);
    tev_stage(3, primary, frag_primary, frag_secondary, tex, update, previous, held, next_buffer);
    tev_stage(4, primary, frag_primary, frag_secondary, tex, update, previous, held, next_buffer);
    tev_stage(5, primary, frag_primary, frag_secondary, tex, update, previous, held, next_buffer);

    // the alpha test, against an 8-bit reference
    uint test = alpha_test();
    if ((test & 1u) != 0u && !compare((test >> 4) & 7u, round(previous.a * 255.0), float((test >> 8) & 0xFFu))) discard;

    // fog (mode 5, under the combiner buffer's update): the fog's color
    // mixed in by the fragment's depth through its table, from the far end
    // with bit 16. the depth as the depth buffer gets it (times w for the w
    // buffer), screen-linear (Zakuro's fog.rs)
    uint fog = update;
    if ((fog & 7u) == 5u) {
        float depth = gl_FragCoord.z;
        if (s.depth.z == 0.0) depth /= gl_FragCoord.w; // the w buffer: DEPTHMAP_ENABLE off
        depth = clamp(depth, 0.0, 1.0);
        if ((fog & 0x10000u) != 0u) depth = 1.0 - depth;
        float index = depth * 128.0, entry = clamp(floor(index), 0.0, 127.0);
        vec2 e = t.entries[FOG_TABLE + int(entry)];
        float factor = clamp(e.x + e.y * (index - entry), 0.0, 1.0);
        previous.rgb = mix(unpack(reg(FOG_COLOR)).rgb, previous.rgb, factor);
    }
#ifndef SHADOW
    out_color = previous;
#else
    // the shadow output (mode 3): the map keeps the nearest depth; a
    // fragment with an intensity (green) only lowers the intensity kept, by
    // it over constant + linear times its depth over the map's (a softer
    // edge farther off). the word changed only while nothing else did
    float fragment_depth = gl_FragCoord.z;
    if (s.depth.z == 0.0) fragment_depth /= gl_FragCoord.w;
    uint depth = uint(clamp(fragment_depth, 0.0, 1.0) * 16777215.0);
    uint intensity = uint(round(previous.g * 255.0));
    vec2 falloff = unpackHalf2x16(reg(SHADOW_OUTPUT)); // constant, linear
    ivec2 at = ivec2(gl_FragCoord.xy);
    uint old = imageLoad(shadow_map, at).x;
    for (int tries = 0; tries < 64; tries++) {
        uint old_depth = old >> 8, old_intensity = old & 0xFFu, now;
        if (depth >= old_depth) break;
        if (intensity == 0u) {
            now = depth << 8 | old_intensity;
        } else {
            float lowered = float(intensity) / (falloff.x + falloff.y * float(depth) / float(old_depth));
            uint kept = uint(clamp(lowered, 0.0, 255.0));
            if (kept >= old_intensity) break;
            now = (old & 0xFFFFFF00u) | kept;
        }
        uint seen = imageAtomicCompSwap(shadow_map, at, old, now);
        if (seen == old) break;
        old = seen;
    }
    discard;
#endif
#ifdef W_BUFFER
    // the w buffer (DEPTHMAP_ENABLE off): the depth map times w, which
    // the rasterizer cannot give
    gl_FragDepth = clamp(gl_FragCoord.z / gl_FragCoord.w, 0.0, 1.0);
#endif
}
