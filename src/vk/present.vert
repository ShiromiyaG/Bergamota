#version 450
// A screen's rectangle in the window, two triangles from the vertex index
// alone, and where in it each corner is.

layout(push_constant) uniform Screen {
    vec4 rect;   // left, top, right, bottom, in Vulkan's normalized coordinates
    uint rotate; // read from a framebuffer, which is the screen on its side
} p;

layout(location = 0) out vec2 v_at;

void main() {
    vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    v_at = corner;
    gl_Position = vec4(mix(p.rect.xy, p.rect.zw, corner), 0.0, 1.0);
}
