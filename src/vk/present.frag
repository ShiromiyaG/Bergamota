#version 450
// A screen's pixels, opaque (present.cpp).

layout(set = 0, binding = 0) uniform sampler2D screen;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() { out_color = vec4(texture(screen, v_uv).rgb, 1.0); }
