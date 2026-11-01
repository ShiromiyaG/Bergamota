#version 450
// A screen's rectangle in the window (present.cpp): two triangles, and
// where in the screen's image each corner reads. The image is the
// framebuffer, the screen turned on its side: a screen column is a row of
// it, bottom to top.

layout(push_constant) uniform Push {
    vec4 rect;   // left, top, right, bottom, in Vulkan's -1..1
    vec4 from;   // the part of the screen drawn: left, top, right, bottom, 0..1
    vec2 extent; // the part of the image the screen fills, 0..1
} pc;

layout(location = 0) out vec2 v_uv;

void main() {
    // 0-1-2, 2-1-3 over the corners (0,0) (1,0) (0,1) (1,1)
    const int CORNER[6] = int[6](0, 1, 2, 2, 1, 3);
    int c = CORNER[gl_VertexIndex];
    vec2 at = vec2(c & 1, c >> 1); // across, down the screen
    gl_Position = vec4(mix(pc.rect.x, pc.rect.z, at.x), mix(pc.rect.y, pc.rect.w, at.y), 0.0, 1.0);
    // the screen's x is the image's row, its y the image's column from the right
    vec2 on = vec2(mix(pc.from.x, pc.from.z, at.x), mix(pc.from.y, pc.from.w, at.y));
    v_uv = vec2((1.0 - on.y) * pc.extent.x, on.x * pc.extent.y);
}
