#version 450
// The PICA's vertex shader run here, on the host's GPU: the program the
// game uploaded, interpreted for each vertex as src/pica/shader.cpp runs it
// on the CPU, its outputs placed by the output map as vertex.cpp places
// them. A frame drawn between the game's runs it twice, with the frame
// before's inputs and uniforms and with this one's, and puts the vertex
// between the two. The same text is the frame of each program translated
// to GLSL (translate.cpp): run() is swapped for the program's own.

// the registers the stages read (see impl.h, State)
layout(std140, set = 0, binding = 0) uniform State {
    uvec4 regs[16];
    vec4 depth; // scale, offset, w buffer, unused
} s;

// the program and its operand descriptors
layout(std430, set = 0, binding = 4) readonly buffer Program {
    uint code[4096];
    uint desc[128];
} prog;

// a draw's uniforms and output map (impl.h, UnitBlock)
struct Unit {
    vec4 f[96];
    ivec4 i[4];   // the integer uniforms' bytes: count, start, step, unused
    uvec4 misc;   // bools, entry, output mask, output total
    uvec4 map[2]; // the output map's seven words
};
layout(std140, set = 0, binding = 5) uniform Now { Unit u; } now;
layout(std140, set = 0, binding = 7) uniform Before { Unit u; } before;

// the input registers, stride of them a vertex
layout(std430, set = 0, binding = 6) readonly buffer Inputs { vec4 v[]; } inputs_now;
layout(std430, set = 0, binding = 8) readonly buffer InputsBefore { vec4 v[]; } inputs_before;

// each vertex's outputs as a draw of the game's left them (24 values a
// vertex), for its frame between to take instead of running it again
layout(std430, set = 0, binding = 9) buffer Outputs { float v[]; } outputs;

layout(push_constant) uniform Push {
    uint stride, before_stride;
    float t;      // where between the two: 0 the frame before, 1 this one
    uint between; // 1: run the frame before's too; 2: take both from outputs
    uint now_out, before_out; // where in outputs (a draw between), in floats
    uint store;   // where this draw's go, or ~0 for nowhere
    float jump;   // a vertex between that moved more than this (-1 to 1) stays where it is now
} pc;

// a vertex between two frames: where it was and is, mixed, unless it
// crossed the eye's plane (behind the camera in one, in front in the other:
// mixed, its w passes through 0 and it lands at infinity, its triangles
// stretched across the screen as the camera swings or a character comes
// in), or moved far more than the rest of its draw did (between.cpp,
// jump_limit: not the same point, as when a batch of NPCs' shadows comes in
// another order)
float between_t(vec4 earlier, vec4 now) {
    if (earlier.w * now.w <= 0.0) return 1.0;
    if (earlier.w > 0.0 && now.w > 0.0) {
        vec2 d = abs(earlier.xy / earlier.w - now.xy / now.w);
        if (max(d.x, d.y) > pc.jump) return 1.0;
    }
    return pc.t;
}



layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv0;
layout(location = 2) out vec2 v_uv1;
layout(location = 3) out vec2 v_uv2;
layout(location = 4) out float v_w0;
layout(location = 5) out vec4 v_quaternion;
layout(location = 6) out vec3 v_view;
layout(location = 7) out float v_depth; // the PICA's depth, for the w buffer

// which of the two a run reads: false this frame's, true the frame before's
bool from_before;

vec4 uniform_at(int k) { return from_before ? before.u.f[k] : now.u.f[k]; }
ivec4 int_at(uint k) { return from_before ? before.u.i[k] : now.u.i[k]; }
uvec4 misc() { return from_before ? before.u.misc : now.u.misc; }
uint map_word(uint slot) { return from_before ? before.u.map[slot >> 2][slot & 3] : now.u.map[slot >> 2][slot & 3]; }
vec4 input_at(uint reg) {
    uint stride = from_before ? pc.before_stride : pc.stride;
    if (reg >= stride) return vec4(0.0);
    uint at = uint(gl_VertexIndex) * stride + reg;
    return from_before ? inputs_before.v[at] : inputs_now.v[at];
}

// the shader's multiply: zero times infinity is zero, not NaN
float mul1(float a, float b) {
    float p = a * b;
    return isnan(p) && !isnan(a) && !isnan(b) ? 0.0 : p;
}
vec4 mul4(vec4 a, vec4 b) { return vec4(mul1(a.x, b.x), mul1(a.y, b.y), mul1(a.z, b.z), mul1(a.w, b.w)); }

vec4 o[16];
vec4 temp[16];
vec4 vin[16]; // the inputs, for a translated program
ivec3 address; // a0.x, a0.y, aL
bvec2 cond;
uint pc_;

// what a translated program calls
vec4 uniform_i(int k) { return k >= 0 && k < 96 ? uniform_at(k) : vec4(0.0); }
vec4 pmax(vec4 a, vec4 b) { return vec4(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w); }
vec4 pmin(vec4 a, vec4 b) { return vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }
float dp3(vec4 a, vec4 b) { return mul1(a.x, b.x) + mul1(a.y, b.y) + mul1(a.z, b.z); }
float dp4(vec4 a, vec4 b) { return dp3(a, b) + mul1(a.w, b.w); }

vec4 read(uint reg, uint index, uint pattern, bool negate) {
    vec4 v = vec4(0.0);
    if (reg < 0x10u) v = input_at(reg);
    else if (reg < 0x20u) v = temp[reg - 0x10u];
    else {
        int k = int(reg) - 0x20 + (index == 1u ? address.x : index == 2u ? address.y : index == 3u ? address.z : 0);
        if (k >= 0 && k < 96) v = uniform_at(k);
    }
    vec4 r = vec4(v[(pattern >> 6) & 3u], v[(pattern >> 4) & 3u], v[(pattern >> 2) & 3u], v[pattern & 3u]);
    return negate ? -r : r;
}

void write(uint reg, uint mask, vec4 v) {
    // the mask's top bit is x
    if (reg < 0x10u) {
        for (int c = 0; c < 4; c++) if ((mask & (8u >> c)) != 0u) o[reg][c] = v[c];
    } else if (reg < 0x20u) {
        for (int c = 0; c < 4; c++) if ((mask & (8u >> c)) != 0u) temp[reg - 0x10u][c] = v[c];
    }
}

bool condition(uint w) {
    bool x = cond.x == (((w >> 25) & 1u) != 0u), y = cond.y == (((w >> 24) & 1u) != 0u);
    switch ((w >> 22) & 3u) {
    case 0u: return x || y;
    case 1u: return x && y;
    case 2u: return x;
    default: return y;
    }
}

bool compare(uint mode, float x, float y) {
    switch (mode) {
    case 0u: return x == y;
    case 1u: return x != y;
    case 2u: return x < y;
    case 3u: return x <= y;
    case 4u: return x > y;
    case 5u: return x >= y;
    default: return true;
    }
}

// the call, if and loop blocks open: where each ends and goes back to
const int DEPTH = 16;
uint b_end[DEPTH], b_back[DEPTH], b_repeat[DEPTH], b_start[DEPTH];
int b_step[DEPTH];
bool b_loop[DEPTH];
int depth;

void enter(uint start, uint count, uint back) {
    if (depth == DEPTH) return;
    b_end[depth] = start + count, b_back[depth] = back, b_repeat[depth] = 0u, b_step[depth] = 0;
    b_start[depth] = start, b_loop[depth] = false;
    depth++;
}

// the innermost block ended: a loop goes round again, the rest return
void end_block() {
    int top = depth - 1;
    address.z += b_step[top];
    if (b_repeat[top] == 0u) {
        pc_ = b_back[top];
        depth--;
    } else {
        b_repeat[top]--;
        pc_ = b_start[top];
    }
}

void begin_run() {
    for (int r = 0; r < 16; r++) o[r] = vec4(0.0), temp[r] = vec4(0.0, 0.0, 0.0, 1.0), vin[r] = input_at(uint(r));
    address = ivec3(0);
    cond = bvec2(false);
    depth = 0;
    pc_ = misc().y;
}

// -- run: the interpreter; a translated program's goes here instead

void run() {
    begin_run();
    uint bools = misc().x;
    for (uint budget = 0u; budget < 0x10000u; budget++) {
        // blocks ending here: loops go round again, the rest return
        while (depth > 0 && pc_ == b_end[depth - 1]) end_block();
        if (pc_ >= 4096u) return;
        uint w = prog.code[pc_];
        uint raw = w >> 26;
        uint op = raw >= 0x38u ? 0x38u : raw >= 0x30u ? 0x30u : raw == 0x2Fu ? 0x2Eu : raw;
        uint next = pc_ + 1u;
        uint target = (w >> 10) & 0xFFFu, count = w & 0xFFu;
        if (op == 0x22u) return; // END
        if (op == 0x21u || op == 0x2Au || op == 0x2Bu) { pc_ = next; continue; } // NOP, EMIT, SETEMIT
        if (op == 0x24u || op == 0x25u || op == 0x26u) { // CALL, CALLC, CALLU
            bool go = op == 0x24u || (op == 0x26u ? ((bools >> ((w >> 22) & 0xFu)) & 1u) != 0u : condition(w));
            if (go) { enter(target, count, pc_ + 1u); next = target; }
            pc_ = next;
            continue;
        }
        if (op == 0x2Cu || op == 0x2Du) { // JMPC, JMPU
            bool go = op == 0x2Cu ? condition(w) : (((bools >> ((w >> 22) & 0xFu)) & 1u) != 0u) == ((count & 1u) == 0u);
            if (go) next = target;
            pc_ = next;
            continue;
        }
        if (op == 0x28u || op == 0x27u) { // IFC, IFU
            bool go = op == 0x28u ? condition(w) : ((bools >> ((w >> 22) & 0xFu)) & 1u) != 0u;
            if (go) {
                enter(pc_ + 1u, target > pc_ + 1u ? target - (pc_ + 1u) : 0u, target + count);
                next = pc_ + 1u;
            } else {
                enter(target, count, target + count);
                next = target;
            }
            pc_ = next;
            continue;
        }
        if (op == 0x29u) { // LOOP
            ivec4 i = int_at((w >> 22) & 3u);
            address.z = i.y;
            if (depth < DEPTH) {
                b_end[depth] = target + 1u, b_back[depth] = target + 1u, b_repeat[depth] = uint(i.x);
                b_step[depth] = (i.z << 24) >> 24, b_start[depth] = pc_ + 1u, b_loop[depth] = true;
                depth++;
            }
            pc_ = next;
            continue;
        }
        if (op == 0x20u || op == 0x23u) { // BREAK, BREAKC
            if (op == 0x20u || condition(w)) {
                // out of the innermost loop, and whatever is open inside it
                while (depth > 0) {
                    depth--;
                    if (b_loop[depth]) {
                        next = b_back[depth];
                        break;
                    }
                }
            }
            pc_ = next;
            continue;
        }

        // the arithmetic: operands from the descriptor
        bool mad = op == 0x38u || op == 0x30u;
        uint desc, dest, s0, s1, s2 = 0u, i0 = 0u, i1 = 0u, i2 = 0u;
        if (mad) {
            desc = prog.desc[w & 0x1Fu];
            dest = (w >> 24) & 0x1Fu;
            s0 = (w >> 17) & 0x1Fu;
            uint index = (w >> 22) & 3u;
            if (op == 0x30u) s1 = (w >> 12) & 0x1Fu, s2 = (w >> 5) & 0x7Fu, i2 = index;
            else s1 = (w >> 10) & 0x7Fu, s2 = (w >> 5) & 0x1Fu, i1 = index;
        } else {
            desc = prog.desc[w & 0x7Fu];
            dest = (w >> 21) & 0x1Fu;
            uint index = (w >> 19) & 3u;
            bool inverted = op >= 0x18u && op <= 0x1Bu;
            if (inverted) s0 = (w >> 14) & 0x1Fu, s1 = (w >> 7) & 0x7Fu, i1 = index;
            else s0 = (w >> 12) & 0x7Fu, s1 = (w >> 7) & 0x1Fu, i0 = index;
        }
        uint mask = desc & 0xFu;
        vec4 a = read(s0, i0, (desc >> 5) & 0xFFu, ((desc >> 4) & 1u) != 0u);
        vec4 b = read(s1, i1, (desc >> 14) & 0xFFu, ((desc >> 13) & 1u) != 0u);
        vec4 r;
        switch (op) {
        case 0x38u: case 0x30u: { // MAD, MADI
            vec4 c = read(s2, i2, (desc >> 23) & 0xFFu, ((desc >> 22) & 1u) != 0u);
            r = mul4(a, b) + c;
            break;
        }
        case 0x00u: r = a + b; break; // ADD
        case 0x08u: r = mul4(a, b); break; // MUL
        // as the hardware: max(0, NaN) is NaN, max(NaN, 0) is 0
        case 0x0Cu: for (int c = 0; c < 4; c++) r[c] = a[c] > b[c] ? a[c] : b[c]; break; // MAX
        case 0x0Du: for (int c = 0; c < 4; c++) r[c] = a[c] < b[c] ? a[c] : b[c]; break; // MIN
        case 0x01u: r = vec4(mul1(a.x, b.x) + mul1(a.y, b.y) + mul1(a.z, b.z)); break; // DP3
        case 0x02u: r = vec4(mul1(a.x, b.x) + mul1(a.y, b.y) + mul1(a.z, b.z) + mul1(a.w, b.w)); break; // DP4
        case 0x03u: case 0x18u: r = vec4(mul1(a.x, b.x) + mul1(a.y, b.y) + mul1(a.z, b.z) + b.w); break; // DPH, DPHI
        case 0x13u: r = a; break; // MOV
        case 0x0Bu: r = floor(a); break; // FLR
        case 0x0Eu: r = vec4(1.0 / a.x); break; // RCP
        case 0x0Fu: r = vec4(1.0 / sqrt(a.x)); break; // RSQ
        case 0x05u: r = vec4(exp2(a.x)); break; // EX2
        case 0x06u: r = vec4(log2(a.x)); break; // LG2
        case 0x09u: case 0x1Au: r = vec4(greaterThanEqual(a, b)); break; // SGE, SGEI
        case 0x0Au: case 0x1Bu: r = vec4(lessThan(a, b)); break; // SLT, SLTI
        case 0x04u: case 0x19u: r = vec4(1.0, mul1(a.y, b.y), a.z, b.w); break; // DST, DSTI
        case 0x07u: // LITP
            cond = bvec2(a.x >= 0.0, a.w >= 0.0);
            r = vec4(max(a.x, 0.0), clamp(a.y, -127.9961, 127.9961), 0.0, max(a.w, 0.0));
            break;
        case 0x12u: // MOVA
            if ((mask & 8u) != 0u) address.x = int(a.x);
            if ((mask & 4u) != 0u) address.y = int(a.y);
            pc_ = next;
            continue;
        case 0x2Eu: // CMP
            cond = bvec2(compare((w >> 24) & 7u, a.x, b.x), compare((w >> 21) & 7u, a.y, b.y));
            pc_ = next;
            continue;
        default: pc_ = next; continue; // not one the PICA has
        }
        write(dest, mask, r);
        pc_ = next;
    }
}
// -- end run

// the outputs to the rasterizer's values: the enabled output registers in
// order, each component named by the output map
float values[24];
void place() {
    vec4 packed[16];
    uint out_mask = misc().z & 0xFFFFu;
    for (int r = 0; r < 16; r++) packed[r] = out_mask == 0u ? o[r] : vec4(0.0);
    uint slot = 0u;
    for (uint reg = 0u; reg < 16u && out_mask != 0u; reg++) {
        if (((out_mask >> reg) & 1u) != 0u) packed[slot++] = o[reg];
    }
    for (int k = 0; k < 24; k++) values[k] = 0.0;
    values[3] = 1.0; // w, when nothing says
    uint total = min(misc().w & 7u, 7u);
    for (uint sl = 0u; sl < total; sl++) {
        uint word = map_word(sl);
        for (uint c = 0u; c < 4u; c++) {
            uint semantic = (word >> (c * 8u)) & 0x1Fu;
            if (semantic < 24u) values[semantic] = packed[sl][c];
        }
    }
    if (total == 0u) { // nothing described: position in o0, color in o2, texture in o3
        for (int c = 0; c < 4; c++) values[c] = packed[0][c], values[8 + c] = packed[2][c];
        values[12] = packed[3].x, values[13] = packed[3].y;
    }
}

// a vertex between two frames: its position from the frame before's (was)
// at t (between_t), the rest this frame's
void mix_between(float was[24], float t) {
    for (int k = 0; k < 4; k++) values[k] = mix(was[k], values[k], t);
}

void main() {
    float was[24];
    uint vertex = uint(gl_VertexIndex) * 24u;
    if (pc.between == 2u) {
        // both frames' as their draws left them: nothing to run
        for (uint k = 0u; k < 24u; k++) values[k] = outputs.v[pc.now_out + vertex + k];
        for (uint k = 0u; k < 24u; k++) was[k] = outputs.v[pc.before_out + vertex + k];
        mix_between(was, between_t(vec4(was[0], was[1], was[2], was[3]), vec4(values[0], values[1], values[2], values[3])));
    } else {
    if (pc.between != 0u) {
        from_before = true;
        run();
        place();
        for (int k = 0; k < 24; k++) was[k] = values[k];
    }
    from_before = false;
    run();
    place();
    if (pc.store != 0xFFFFFFFFu) {
        for (uint k = 0u; k < 24u; k++) outputs.v[pc.store + vertex + k] = values[k];
    }
    if (pc.between != 0u) mix_between(was, between_t(vec4(was[0], was[1], was[2], was[3]), vec4(values[0], values[1], values[2], values[3])));
    }
    vec4 position = vec4(values[0], values[1], values[2], values[3]);

    // the PICA keeps z in [-w, 0] and its y up; Vulkan has z in [0, w] and
    // y down (the viewport sets rows the PICA's way)
    float z = position.z / position.w * s.depth.x + s.depth.y;
    v_depth = z;
    gl_Position = vec4(position.x, -position.y, clamp(z, 0.0, 1.0) * position.w, position.w);
    v_color = clamp(vec4(values[8], values[9], values[10], values[11]), 0.0, 1.0);
    v_uv0 = vec2(values[12], values[13]);
    v_uv1 = vec2(values[14], values[15]);
    v_uv2 = vec2(values[22], values[23]);
    v_w0 = values[16];
    v_quaternion = vec4(values[4], values[5], values[6], values[7]);
    v_view = vec3(values[18], values[19], values[20]);
}
