#version 450
// The PICA's vertices, already shaded on the CPU: clip space to Vulkan's,
// and the values the fragment shader takes.

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 quaternion;
layout(location = 2) in vec4 color;
layout(location = 3) in vec2 uv0;
layout(location = 4) in vec2 uv1;
layout(location = 5) in vec2 uv2;
layout(location = 6) in float w0;
layout(location = 7) in vec3 view;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv0;
layout(location = 2) out vec2 v_uv1;
layout(location = 3) out vec2 v_uv2;
layout(location = 4) out float v_w0;
layout(location = 5) out vec4 v_quaternion;
layout(location = 6) out vec3 v_view;
layout(location = 7) out float v_depth; // the PICA's depth, for the w buffer

// the registers the stages read (see renderer.cpp, State)
layout(std140, set = 0, binding = 0) uniform State {
    uvec4 regs[16];
    vec4 depth; // scale, offset, w buffer, unused
} s;

void main() {
    // the PICA keeps z in [-w, 0] and its y up; Vulkan has z in [0, w] and
    // y down (the viewport sets rows the PICA's way)
    float z = position.z / position.w * s.depth.x + s.depth.y;
    v_depth = z;
    gl_Position = vec4(position.x, -position.y, clamp(z, 0.0, 1.0) * position.w, position.w);
    v_color = clamp(color, 0.0, 1.0);
    v_uv0 = uv0;
    v_uv1 = uv1;
    v_uv2 = uv2;
    v_w0 = w0;
    v_quaternion = quaternion;
    v_view = view;
}
