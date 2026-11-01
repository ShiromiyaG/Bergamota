#version 450
// A screen's pixels, opaque (present.cpp), with the post-processing asked
// for: anti-aliasing (FXAA: edges found by their contrast in luma and
// blended along their direction) and sharpening (an unsharp mask held
// within the neighbors' range, so it adds no halos). A placement can be
// blurred (a fill behind the screens), dimmed and see-through.

layout(set = 0, binding = 0) uniform sampler2D screen;

layout(push_constant) uniform Push {
    layout(offset = 40) float blur; // texels
    layout(offset = 48) vec4 post;  // anti-aliasing on (x), sharpening 0-1 (y), brightness (z), opacity (w)
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec3 at(vec2 uv) { return texture(screen, uv).rgb; }

vec3 antialiased(vec2 uv, vec2 px) {
    vec3 c = at(uv);
    float lc = luma(c);
    float nw = luma(at(uv + vec2(-1.0, -1.0) * px)), ne = luma(at(uv + vec2(1.0, -1.0) * px));
    float sw = luma(at(uv + vec2(-1.0, 1.0) * px)), se = luma(at(uv + vec2(1.0, 1.0) * px));
    float low = min(lc, min(min(nw, ne), min(sw, se))), high = max(lc, max(max(nw, ne), max(sw, se)));
    // flat: as it is
    if (high - low < max(0.03, high * 0.125)) return c;
    // across the edge's gradient, along the edge
    vec2 along = vec2((sw + se) - (nw + ne), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * 0.03125, 1.0 / 128.0);
    along = clamp(along / (min(abs(along.x), abs(along.y)) + reduce), -8.0, 8.0) * px;
    vec3 near = 0.5 * (at(uv - along / 6.0) + at(uv + along / 6.0));
    vec3 far = 0.5 * near + 0.25 * (at(uv - along * 0.5) + at(uv + along * 0.5));
    float lf = luma(far);
    // the wider blend unless it went past what is around
    return lf < low || lf > high ? near : far;
}

vec3 sharpened(vec3 c, vec2 uv, vec2 px, float amount) {
    vec3 n = at(uv + vec2(0.0, -px.y)), s = at(uv + vec2(0.0, px.y)), w = at(uv + vec2(-px.x, 0.0)), e = at(uv + vec2(px.x, 0.0));
    vec3 low = min(c, min(min(n, s), min(w, e))), high = max(c, max(max(n, s), max(w, e)));
    vec3 blur = (n + s + w + e) * 0.25;
    return clamp(c + (c - blur) * amount * 2.0, low, high);
}

// two rings of eight around the point, wide: a fill, not a lens
vec3 blurred(vec2 uv, vec2 px, float radius) {
    vec3 sum = at(uv);
    for (int i = 0; i < 8; i++) {
        float angle = float(i) * 0.785398;
        vec2 d = vec2(cos(angle), sin(angle)) * px * radius;
        sum += at(uv + d) + at(uv + d * 0.5);
    }
    return sum / 17.0;
}

void main() {
    vec2 px = 1.0 / vec2(textureSize(screen, 0));
    vec3 c;
    if (pc.blur > 0.0) {
        c = blurred(v_uv, px, pc.blur);
    } else {
        c = pc.post.x > 0.5 ? antialiased(v_uv, px) : at(v_uv);
        if (pc.post.y > 0.0) c = sharpened(c, v_uv, px, pc.post.y);
    }
    out_color = vec4(c * pc.post.z, pc.post.w);
}
