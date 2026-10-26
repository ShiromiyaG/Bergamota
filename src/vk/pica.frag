#version 450
// The PICA's fragment stage as one shader: it reads the registers as the
// game wrote them and works the combiners (TEV) out from them, so no state
// needs a shader of its own. After Zakuro's tev.rs (Rust, MIT, Copyright
// (c) 2026 fearkov; see LICENSE-zakuro).

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

// the registers, by the slots renderer.cpp puts them in
uint reg(int slot) { return s.regs[slot >> 2][slot & 3]; }
const int TEV = 0, TEV_UPDATE = 30, TEV_BUFFER = 31, ALPHA_TEST = 32, TEX_CONFIG = 33, TEX_PARAMS = 34, TEX_BORDER = 37;

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
    tex[0] = texel(0, v_uv0);
    tex[1] = texel(1, v_uv1);
    tex[2] = texel(2, (reg(TEX_CONFIG) & (1u << 13)) != 0u ? v_uv1 : v_uv2);
    tex[3] = vec4(0.0); // procedural textures: not yet
    vec4 primary = v_color;

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
            case 1u: v = primary; break;               // fragment lighting: not yet
            case 2u: v = vec4(0.0, 0.0, 0.0, 1.0); break;
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
    out_color = previous;
}
