#version 450
// The PICA's fragment stage as one shader: it reads the registers as the
// game wrote them and works the combiners (TEV) out from them, so no state
// needs a shader of its own; the fragment lighting from a block the CPU
// decoded (pica_lighting.h) and its tables. After Zakuro's tev.rs and
// lighting.rs (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv0;
layout(location = 2) in vec2 v_uv1;
layout(location = 3) in vec2 v_uv2;
layout(location = 4) in float v_w0;
layout(location = 5) in vec4 v_quaternion;
layout(location = 6) in vec3 v_view;
layout(location = 7) in float v_depth;

layout(location = 0) out vec4 out_color;

layout(std140, set = 0, binding = 0) uniform State {
    uvec4 regs[16];
    vec4 depth;
} s;
layout(set = 0, binding = 1) uniform sampler2D textures[3];

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
    uvec4 k = l.lookups[slot];
    uint entry;
    float delta;
    if (k.z != 0u) {
        result = two_sided ? abs(result) : max(result, 0.0);
        float e = clamp(floor(result * 256.0), 0.0, 255.0);
        entry = uint(e);
        delta = result * 256.0 - e;
    } else {
        float e = clamp(floor(result * 128.0), -128.0, 127.0);
        entry = uint(int(e)) & 0xFFu;
        delta = result * 128.0 - e;
    }
    return uintBitsToFloat(k.w) * table(which, entry, delta);
}

vec4 quantize(vec4 c) { return floor(clamp(c, 0.0, 1.0) * 255.0) / 255.0; }

// the primary (diffuse) and secondary (specular) fragment colors
void light(vec4 tex[4], out vec4 primary, out vec4 secondary) {
    uint config = l.head.x, count = l.head.y, bump = l.head.z, shadow_cfg = l.head.w;
    bool needs_half = (l.switches.w & 1u) != 0u, needs_view = (l.switches.w & 2u) != 0u;
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
        #define VALUE(slot, which) lookup(slot, which, l.lookups[slot].y < 6u ? inputs[l.lookups[slot].y] : 0.0, two_sided)

        float spot = (flags & 32u) != 0u ? VALUE(LOOKUP_SPOT, SPOTLIGHT + int(number)) : 1.0;
        float d0 = l.lookups[LOOKUP_D0].x != 0u ? VALUE(LOOKUP_D0, D0) : 1.0;
        float red = l.lookups[LOOKUP_RED].x != 0u ? VALUE(LOOKUP_RED, REFLECT_RED) : 1.0;
        vec3 reflect_rgb = vec3(red, l.lookups[LOOKUP_GREEN].x != 0u ? VALUE(LOOKUP_GREEN, REFLECT_GREEN) : red,
                                l.lookups[LOOKUP_BLUE].x != 0u ? VALUE(LOOKUP_BLUE, REFLECT_BLUE) : red);
        float d1 = l.lookups[LOOKUP_D1].x != 0u ? VALUE(LOOKUP_D1, D1) : 1.0;
        vec3 specular0 = li.specular0.rgb * d0;
        vec3 specular1 = d1 * reflect_rgb * li.specular1.rgb;

        // only the last light applies fresnel
        if (i == count - 1u && l.lookups[LOOKUP_FRESNEL].x != 0u) {
            float fresnel = VALUE(LOOKUP_FRESNEL, FRESNEL);
            if (l.switches.x != 0u) diffuse_sum.a = fresnel;
            if (l.switches.y != 0u) specular_sum.a = fresnel;
        }
        #undef VALUE

        float facing = dot(light_vector, normal);
        facing = two_sided ? abs(facing) : max(facing, 0.0);
        float highlights = l.switches.z != 0u && facing == 0.0 ? 0.0 : 1.0;
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
        if (l.switches.x != 0u) diffuse_sum.a *= shadow.a;
        if (l.switches.y != 0u) specular_sum.a *= shadow.a;
    }
    diffuse_sum.rgb += l.global_ambient.rgb;
    // the unit hands the combiners 8-bit colors
    primary = quantize(diffuse_sum);
    secondary = quantize(specular_sum);
}

// the registers, by the slots renderer.cpp puts them in
uint reg(int slot) { return s.regs[slot >> 2][slot & 3]; }
const int TEV = 0, TEV_UPDATE = 30, TEV_BUFFER = 31, ALPHA_TEST = 32, TEX_CONFIG = 33, TEX_PARAMS = 34, TEX_BORDER = 37,
          FOG_COLOR = 40, PROCTEX = 41; // the procedural texture's six: configuration, noise u, v, frequency, table, offset
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
    uint config = s.regs[PROCTEX >> 2][PROCTEX & 3];
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
    if ((reg(TEX_CONFIG) & (1u << unit)) == 0u) return vec4(0.0);
    uint params = reg(TEX_PARAMS + unit);
    bool border_s = ((params >> 12) & 3u) == 1u, border_t = ((params >> 8) & 3u) == 1u;
    if ((border_s && (uv.x < 0.0 || uv.x > 1.0)) || (border_t && (uv.y < 0.0 || uv.y > 1.0))) {
        return unpack(reg(TEX_BORDER + unit));
    }
    return texture(textures[unit], vec2(uv.x, 1.0 - uv.y));
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

void main() {
    // texture 2 reads coordinate set 1 or 2
    vec4 tex[4];
    // texture 0 projected (its type, 3): the coordinates over their w
    tex[0] = texel(0, ((reg(TEX_PARAMS) >> 28) & 7u) == 3u ? v_uv0 / v_w0 : v_uv0);
    tex[1] = texel(1, v_uv1);
    tex[2] = texel(2, (reg(TEX_CONFIG) & (1u << 13)) != 0u ? v_uv1 : v_uv2);
    // the procedural one reads coordinate set 0, 1 or 2
    uint coordinates = min((reg(TEX_CONFIG) >> 8) & 3u, 2u);
    tex[3] = (reg(TEX_CONFIG) & (1u << 10)) != 0u ? procedural(coordinates == 0u ? v_uv0 : coordinates == 1u ? v_uv1 : v_uv2) : vec4(0.0);
    vec4 primary = v_color;
    // without lighting the primary fragment color is the vertex's, and
    // there is no specular term
    vec4 frag_primary = primary, frag_secondary = vec4(0.0, 0.0, 0.0, 1.0);
    if (l.head.y != 0u) light(tex, frag_primary, frag_secondary);

    vec4 previous = primary;
    // the buffer lags a stage: the first reads zero, the second the
    // configured color, a stage's update reaches the stage after next
    vec4 held = vec4(0.0);
    vec4 next_buffer = unpack(reg(TEV_BUFFER));
    uint update = reg(TEV_UPDATE);
    for (int i = 0; i < 6; i++) {
        uint src = reg(TEV + i * 5), ops = reg(TEV + i * 5 + 1), comb = reg(TEV + i * 5 + 2);
        vec4 konst = unpack(reg(TEV + i * 5 + 3));
        uint sc = reg(TEV + i * 5 + 4);
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

    // the alpha test, against an 8-bit reference
    uint test = reg(ALPHA_TEST);
    if ((test & 1u) != 0u && !compare((test >> 4) & 7u, round(previous.a * 255.0), float((test >> 8) & 0xFFu))) discard;

    // fog (mode 5, under the combiner buffer's update): the fog's color
    // mixed in by the fragment's depth through its table, from the far end
    // with bit 16. the depth as the depth buffer gets it (times w for the w
    // buffer), screen-linear (Zakuro's fog.rs)
    uint fog = reg(TEV_UPDATE);
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
    out_color = previous;
#ifdef W_BUFFER
    // the w buffer (DEPTHMAP_ENABLE off): the depth map times w, which
    // the rasterizer cannot give
    gl_FragDepth = clamp(gl_FragCoord.z / gl_FragCoord.w, 0.0, 1.0);
#endif
}
