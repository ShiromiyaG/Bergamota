#version 450
// A screen from where the game drew it. A framebuffer is the screen turned
// on its side (as picture() reads it back): a screen column is a row of it,
// bottom to top. A picture read from memory is the right way up already.

layout(push_constant) uniform Screen {
    vec4 rect;
    uint rotate;
} p;

layout(set = 0, binding = 0) uniform sampler2D screen;

layout(location = 0) in vec2 v_at;
layout(location = 0) out vec4 out_color;

void main() {
    vec2 at = p.rotate != 0u ? vec2(1.0 - v_at.y, v_at.x) : v_at;
    out_color = vec4(texture(screen, at).rgb, 1.0);
}
