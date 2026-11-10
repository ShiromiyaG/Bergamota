// The PICA's vertex programs as GLSL (translate.h). Each instruction is
// decoded here once: its operands, swizzles and mask are written into the
// code, so the GPU only does the arithmetic. The flow (calls, ifs, loops,
// jumps) keeps the interpreter's blocks: run() goes round a switch on the
// program counter, with a case only where the flow can land, and runs
// straight on between them.

#include "translate.h"
#include "impl.h"

#ifdef RUNTIME_GLSLANG
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#else
#include <shaderc/shaderc.h>
#endif

#ifdef __linux__
#include <sched.h>
#include <sys/resource.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

namespace runtime {

namespace {

// GLSL to SPIR-V for Vulkan 1.1's devices (SPIR-V 1.3): shaderc, the
// Vulkan SDK's, where it is; glslang alone elsewhere (Android)
class Spirv {
public:
#ifdef RUNTIME_GLSLANG
    Spirv() { glslang_initialize_process(); }
    ~Spirv() { glslang_finalize_process(); }
    bool vertex(const std::string &text, std::vector<uint32_t> &words, std::string &error) {
        glslang_input_t input{};
        input.language = GLSLANG_SOURCE_GLSL;
        input.stage = GLSLANG_STAGE_VERTEX;
        input.client = GLSLANG_CLIENT_VULKAN;
        input.client_version = GLSLANG_TARGET_VULKAN_1_1;
        input.target_language = GLSLANG_TARGET_SPV;
        input.target_language_version = GLSLANG_TARGET_SPV_1_3;
        input.code = text.c_str();
        input.default_version = 450;
        input.default_profile = GLSLANG_NO_PROFILE;
        input.messages = GLSLANG_MSG_DEFAULT_BIT;
        input.resource = glslang_default_resource();
        glslang_shader_t *shader = glslang_shader_create(&input);
        bool ok = glslang_shader_preprocess(shader, &input) && glslang_shader_parse(shader, &input);
        glslang_program_t *program = nullptr;
        if (ok) {
            program = glslang_program_create();
            glslang_program_add_shader(program, shader);
            ok = glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
            if (ok) {
                glslang_program_SPIRV_generate(program, GLSLANG_STAGE_VERTEX);
                words.resize(glslang_program_SPIRV_get_size(program));
                glslang_program_SPIRV_get(program, words.data());
            } else {
                error = glslang_program_get_info_log(program);
            }
        } else {
            error = glslang_shader_get_info_log(shader);
        }
        if (program) glslang_program_delete(program);
        glslang_shader_delete(shader);
        return ok;
    }
#else
    Spirv() : compiler_(shaderc_compiler_initialize()), options_(shaderc_compile_options_initialize()) {
        shaderc_compile_options_set_optimization_level(options_, shaderc_optimization_level_performance);
        shaderc_compile_options_set_target_env(options_, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    }
    ~Spirv() {
        shaderc_compile_options_release(options_);
        shaderc_compiler_release(compiler_);
    }
    bool vertex(const std::string &text, std::vector<uint32_t> &words, std::string &error) {
        shaderc_compilation_result_t result =
            shaderc_compile_into_spv(compiler_, text.data(), text.size(), shaderc_vertex_shader, "pica", "main", options_);
        bool ok = shaderc_result_get_compilation_status(result) == shaderc_compilation_status_success;
        if (ok) {
            const auto *bytes = reinterpret_cast<const uint32_t *>(shaderc_result_get_bytes(result));
            words.assign(bytes, bytes + shaderc_result_get_length(result) / 4);
        } else {
            error = shaderc_result_get_error_message(result);
        }
        shaderc_result_release(result);
        return ok;
    }

private:
    shaderc_compiler_t compiler_;
    shaderc_compile_options_t options_;
#endif
};

const char FRAME[] =
#include "pica_gpu.vert.txt"
    ;

enum : uint32_t {
    ADD = 0x00, DP3 = 0x01, DP4 = 0x02, DPH = 0x03, DST = 0x04, EX2 = 0x05, LG2 = 0x06, LITP = 0x07, MUL = 0x08,
    SGE = 0x09, SLT = 0x0A, FLR = 0x0B, MAX = 0x0C, MIN = 0x0D, RCP = 0x0E, RSQ = 0x0F, MOVA = 0x12, MOV = 0x13,
    DPHI = 0x18, DSTI = 0x19, SGEI = 0x1A, SLTI = 0x1B, BREAK = 0x20, NOP = 0x21, END = 0x22, BREAKC = 0x23,
    CALL = 0x24, CALLC = 0x25, CALLU = 0x26, IFU = 0x27, IFC = 0x28, LOOP = 0x29, EMIT = 0x2A, SETEMIT = 0x2B,
    JMPC = 0x2C, JMPU = 0x2D, CMP = 0x2E, MADI = 0x30, MAD = 0x38,
};

uint32_t op_of(uint32_t word) {
    uint32_t raw = word >> 26;
    return raw >= MAD ? MAD : raw >= MADI ? MADI : raw == 0x2F ? CMP : raw;
}

bool is_flow(uint32_t op) { return op >= BREAK && op <= JMPU && op != NOP && op != EMIT && op != SETEMIT; }

// a source operand as GLSL
std::string operand(uint32_t reg, uint32_t index, uint32_t pattern, bool negate) {
    std::string base;
    if (reg < 0x10) base = "vin[" + std::to_string(reg) + "]";
    else if (reg < 0x20) base = "temp[" + std::to_string(reg - 0x10) + "]";
    else if (!index) base = "uniform_at(" + std::to_string(reg - 0x20) + ")";
    else base = "uniform_i(" + std::to_string(reg - 0x20) + " + address." + "xyz"[index - 1] + ")";
    static const char C[] = "xyzw";
    std::string swizzle = ".";
    for (int c = 0; c < 4; c++) swizzle += C[(pattern >> (6 - c * 2)) & 3];
    std::string v = base + (swizzle == ".xyzw" ? "" : swizzle);
    return negate ? "(-" + v + ")" : v;
}

std::string u(uint32_t v) { return std::to_string(v) + "u"; }

} // namespace

std::string translate_program(const ProgramBlock &p, uint32_t entry) {
    // where the flow can land: the entry, every target, the ends and backs
    // of blocks, and after each flow instruction
    std::set<uint32_t> labels{entry};
    std::set<uint32_t> reached;
    std::vector<uint32_t> todo{entry};
    auto reach = [&](uint32_t pc) {
        if (pc < 4096 && !reached.count(pc)) todo.push_back(pc);
    };
    while (!todo.empty()) {
        uint32_t pc = todo.back();
        todo.pop_back();
        if (pc >= 4096 || reached.count(pc)) continue;
        reached.insert(pc);
        uint32_t w = p.code[pc], op = op_of(w), target = (w >> 10) & 0xFFF, count = w & 0xFF;
        auto label = [&](uint32_t at) {
            labels.insert(at);
            reach(at);
        };
        switch (op) {
        case END: break;
        case CALL: case CALLC: case CALLU: label(target), label(target + count), label(pc + 1); break;
        case JMPC: case JMPU: label(target), label(pc + 1); break;
        case IFC: case IFU: label(pc + 1), label(target), label(target + count); break;
        case LOOP: label(pc + 1), label(target + 1); break;
        case BREAK: case BREAKC: label(pc + 1); break;
        default: reach(pc + 1);
        }
    }

    std::ostringstream s;
    s << "void run() {\n    begin_run();\n    uint bools = misc().x;\n";
    s << "    for (uint steps = 0u; steps < 0x10000u; steps++) {\n        switch (pc_) {\n";
    uint32_t last = ~0u;
    bool open = false; // the code before falls through into what comes next
    for (uint32_t pc : reached) {
        if (open && pc != last + 1) s << "            pc_ = " << u(last + 1) << "; continue;\n";
        if (labels.count(pc) || !open) {
            s << "        case " << u(pc) << ":\n";
            s << "            if (depth > 0 && b_end[depth - 1] == " << u(pc) << ") { end_block(); continue; }\n";
        }
        last = pc;
        open = true;
        uint32_t w = p.code[pc], op = op_of(w), target = (w >> 10) & 0xFFF, count = w & 0xFF;
        std::string bit = "((bools >> " + std::to_string((w >> 22) & 0xF) + "u) & 1u) != 0u";
        std::string cond = "condition(" + u(w) + ")";
        s << "            // " << pc << ": " << std::hex << w << std::dec << "\n";
        if (is_flow(op) || op == END) {
            switch (op) {
            case END: s << "            return;\n"; open = false; break;
            case CALL: case CALLC: case CALLU: {
                std::string go = op == CALL ? "true" : op == CALLU ? bit : cond;
                s << "            if (" << go << ") { enter(" << u(target) << ", " << u(count) << ", " << u(pc + 1) << "); pc_ = "
                  << u(target) << "; continue; }\n";
                break;
            }
            case JMPC: case JMPU: {
                std::string go = op == JMPC ? cond : (count & 1 ? "!(" + bit + ")" : "(" + bit + ")");
                s << "            if (" << go << ") { pc_ = " << u(target) << "; continue; }\n";
                break;
            }
            case IFC: case IFU: {
                std::string go = op == IFC ? cond : bit;
                uint32_t body = target > pc + 1 ? target - (pc + 1) : 0;
                s << "            if (" << go << ") { enter(" << u(pc + 1) << ", " << u(body) << ", " << u(target + count)
                  << "); pc_ = " << u(pc + 1) << "; } else { enter(" << u(target) << ", " << u(count) << ", "
                  << u(target + count) << "); pc_ = " << u(target) << "; }\n            continue;\n";
                open = false;
                break;
            }
            case LOOP:
                s << "            { ivec4 i = int_at(" << u((w >> 22) & 3) << "); address.z = i.y;\n"
                  << "              if (depth < DEPTH) { b_end[depth] = " << u(target + 1) << "; b_back[depth] = " << u(target + 1)
                  << "; b_repeat[depth] = uint(i.x); b_step[depth] = (i.z << 24) >> 24; b_start[depth] = " << u(pc + 1)
                  << "; b_loop[depth] = true; depth++; } }\n";
                s << "            pc_ = " << u(pc + 1) << "; continue;\n";
                open = false;
                break;
            case BREAK: case BREAKC:
                s << "            if (" << (op == BREAK ? "true" : cond) << ") { pc_ = " << u(pc + 1)
                  << "; while (depth > 0) { depth--; if (b_loop[depth]) { pc_ = b_back[depth]; break; } } continue; }\n";
                break;
            }
            continue;
        }
        if (op == NOP || op == EMIT || op == SETEMIT) continue;

        // the arithmetic, its operands from the descriptor
        bool mad = op == MAD || op == MADI;
        uint32_t desc, dest, src[3] = {}, index[3] = {};
        if (mad) {
            desc = p.desc[w & 0x1F];
            dest = (w >> 24) & 0x1F;
            src[0] = (w >> 17) & 0x1F;
            uint32_t i = (w >> 22) & 3;
            if (op == MADI) src[1] = (w >> 12) & 0x1F, src[2] = (w >> 5) & 0x7F, index[2] = i;
            else src[1] = (w >> 10) & 0x7F, src[2] = (w >> 5) & 0x1F, index[1] = i;
        } else {
            desc = p.desc[w & 0x7F];
            dest = (w >> 21) & 0x1F;
            uint32_t i = (w >> 19) & 3;
            bool inverted = op >= DPHI && op <= SLTI;
            if (inverted) src[0] = (w >> 14) & 0x1F, src[1] = (w >> 7) & 0x7F, index[1] = i;
            else src[0] = (w >> 12) & 0x7F, src[1] = (w >> 7) & 0x1F, index[0] = i;
        }
        uint32_t mask = desc & 0xF;
        static constexpr int SHIFT[3] = {5, 14, 23};
        std::string a = operand(src[0], index[0], (desc >> SHIFT[0]) & 0xFF, (desc >> (SHIFT[0] - 1)) & 1);
        std::string b = operand(src[1], index[1], (desc >> SHIFT[1]) & 0xFF, (desc >> (SHIFT[1] - 1)) & 1);
        std::string c = operand(src[2], index[2], (desc >> SHIFT[2]) & 0xFF, (desc >> (SHIFT[2] - 1)) & 1);
        std::string r;
        switch (op) {
        case MAD: case MADI: r = "mul4(a, b) + " + c; break;
        case ADD: r = "a + b"; break;
        case MUL: r = "mul4(a, b)"; break;
        case MAX: r = "pmax(a, b)"; break;
        case MIN: r = "pmin(a, b)"; break;
        case DP3: r = "vec4(dp3(a, b))"; break;
        case DP4: r = "vec4(dp4(a, b))"; break;
        case DPH: case DPHI: r = "vec4(dp3(a, b) + b.w)"; break;
        case MOV: r = "a"; break;
        case FLR: r = "floor(a)"; break;
        case RCP: r = "vec4(1.0 / a.x)"; break;
        case RSQ: r = "vec4(1.0 / sqrt(a.x))"; break;
        case EX2: r = "vec4(exp2(a.x))"; break;
        case LG2: r = "vec4(log2(a.x))"; break;
        case SGE: case SGEI: r = "vec4(greaterThanEqual(a, b))"; break;
        case SLT: case SLTI: r = "vec4(lessThan(a, b))"; break;
        case DST: case DSTI: r = "vec4(1.0, mul1(a.y, b.y), a.z, b.w)"; break;
        case LITP:
            s << "            { vec4 a = " << a << "; cond = bvec2(a.x >= 0.0, a.w >= 0.0);\n";
            r = "vec4(max(a.x, 0.0), clamp(a.y, -127.9961, 127.9961), 0.0, max(a.w, 0.0))";
            break;
        case MOVA:
            s << "            { vec4 a = " << a << ";";
            if (mask & 8) s << " address.x = int(a.x);";
            if (mask & 4) s << " address.y = int(a.y);";
            s << " }\n";
            continue;
        case CMP:
            s << "            { vec4 a = " << a << "; vec4 b = " << b << "; cond = bvec2(compare(" << u((w >> 24) & 7)
              << ", a.x, b.x), compare(" << u((w >> 21) & 7) << ", a.y, b.y)); }\n";
            continue;
        default: continue; // not one the PICA has
        }
        if (dest >= 0x20 || !mask) {
            if (op == LITP) s << "            }\n";
            continue;
        }
        std::string to = dest < 0x10 ? "o[" + std::to_string(dest) + "]" : "temp[" + std::to_string(dest - 0x10) + "]";
        std::string comps;
        for (int k = 0; k < 4; k++) {
            if (mask & (8 >> k)) comps += "xyzw"[k];
        }
        if (op != LITP) s << "            { vec4 a = " << a << "; vec4 b = " << b << ";";
        else s << "             ";
        s << " vec4 r = " << r << "; " << to << (comps.size() == 4 ? "" : "." + comps) << " = r"
          << (comps.size() == 4 ? "" : "." + comps) << "; }\n";
    }
    if (open) s << "            pc_ = " << u(last + 1) << "; continue;\n";
    s << "        default: return;\n        }\n    }\n}\n";
    return s.str();
}

// the frame's text, in the cache's names: a new frame makes them all again
uint64_t frame_version() {
    static const uint64_t version = hash_bytes(reinterpret_cast<const uint8_t *>(FRAME), sizeof FRAME);
    return version;
}

std::string translated_shader(const ProgramBlock &program, uint32_t entry, bool store) {
    std::string frame = FRAME;
    // defined on the line after #version
    if (size_t line = frame.find('\n'); !store && line != std::string::npos) frame.insert(line + 1, "#define NO_STORE 1\n");
    size_t from = frame.find("// -- run:"), to = frame.find("// -- end run");
    if (from == std::string::npos || to == std::string::npos) return frame;
    return frame.substr(0, from) + translate_program(program, entry) + frame.substr(to);
}

Translator::Translator(VkDevice device, MakePipeline make, bool store) : device_(device), make_(std::move(make)), store_(store) {
    unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    for (unsigned i = 0; i < n; i++) threads_.emplace_back([this] { run(); });
}

Translator::~Translator() {
    {
        std::lock_guard g(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    made_.notify_all();
    for (std::thread &t : threads_) t.join();
    for (auto &[key, pipe] : pipelines_) {
        if (pipe) vkDestroyPipeline(device_, pipe, nullptr);
    }
    for (auto &[key, shader] : shaders_) {
        if (shader.module) vkDestroyShaderModule(device_, shader.module, nullptr);
    }
}

void Translator::cache_in(const std::filesystem::path &folder) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::lock_guard g(lock_);
    cache_ = folder;
}

VkPipeline Translator::pipeline(const std::shared_ptr<const ProgramBlock> &program, uint64_t program_hash, uint32_t entry,
                                uint64_t key, bool has_depth) {
    uint64_t shader_key = Translator::shader_key(program_hash, entry);
    std::lock_guard g(lock_);
    auto it = shaders_.find(shader_key);
    if (it == shaders_.end()) {
        shaders_[shader_key];
        queue_.push_back({program, shader_key, entry, 0, false, {}, 0});
        wake_.notify_one();
        return VK_NULL_HANDLE;
    }
    if (!it->second.done || it->second.failed) return VK_NULL_HANDLE;
    auto pipe = pipelines_.find({shader_key, key, 0});
    if (pipe != pipelines_.end()) return pipe->second;
    pipelines_[{shader_key, key, 0}] = VK_NULL_HANDLE;
    queue_.push_back({nullptr, shader_key, entry, key, has_depth, {}, 0});
    wake_.notify_one();
    return VK_NULL_HANDLE;
}

VkPipeline Translator::specialized(uint64_t shader_key, uint64_t key, bool has_depth, const Shape &shape, uint64_t shape_hash,
                                   bool wait) {
    std::unique_lock g(lock_);
    const std::tuple<uint64_t, uint64_t, uint64_t> which{shader_key, key, shape_hash};
    auto pipe = pipelines_.find(which);
    if (pipe != pipelines_.end() && (pipe->second || !wait)) return pipe->second;
    if (pipe == pipelines_.end()) {
        pipelines_[which] = VK_NULL_HANDLE;
        // waited for: before the ones nothing waits for
        if (wait) later_.push_front({nullptr, shader_key, 0, key, has_depth, shape, shape_hash});
        else later_.push_back({nullptr, shader_key, 0, key, has_depth, shape, shape_hash});
        wake_.notify_one();
    }
    if (!wait) return VK_NULL_HANDLE;
    made_.wait(g, [&] { return stop_ || pipelines_[which] || failed_.count(which); });
    return pipelines_[which];
}

namespace {

// a worker gives way to the game's own threads: below their priority, and
// on a phone on its slower cores (the big ones are the game's and the
// GPU's), where it has both kinds
void step_back() {
#ifdef __linux__
    setpriority(PRIO_PROCESS, 0, 10); // this thread, on Linux
#endif
#ifdef __ANDROID__
    long speed[CPU_SETSIZE] = {}, fastest = 0;
    int cores = 0;
    for (; cores < CPU_SETSIZE; cores++) {
        char name[80];
        std::snprintf(name, sizeof name, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cores);
        FILE *f = std::fopen(name, "r");
        if (!f) break;
        if (std::fscanf(f, "%ld", &speed[cores]) != 1) speed[cores] = 0;
        std::fclose(f);
        fastest = std::max(fastest, speed[cores]);
    }
    cpu_set_t slower;
    CPU_ZERO(&slower);
    int count = 0;
    for (int cpu = 0; cpu < cores; cpu++) {
        if (speed[cpu] < fastest) {
            CPU_SET(cpu, &slower);
            count++;
        }
    }
    if (count) sched_setaffinity(0, sizeof slower, &slower);
#endif
}

} // namespace

void Translator::run() {
    step_back();
    Spirv spirv;
    const char *dump = std::getenv("RUNTIME_DUMP_SHADERS");
    for (;;) {
        Work work;
        {
            std::unique_lock g(lock_);
            wake_.wait(g, [this] { return stop_ || !queue_.empty() || !later_.empty(); });
            if (stop_) break;
            auto &from = queue_.empty() ? later_ : queue_;
            work = std::move(from.front());
            from.pop_front();
        }
        if (work.program) {
            // made before, in another run
            std::filesystem::path file;
            {
                std::lock_guard g(lock_);
                if (!cache_.empty()) {
                    char name[40];
                    std::snprintf(name, sizeof name, "%016llx.spv", (unsigned long long)(work.shader_key ^ frame_version() ^ (store_ ? 0 : 0x4E4F53544F524531ull)));
                    file = cache_ / name;
                }
            }
            std::vector<uint32_t> words;
            if (!file.empty()) {
                if (FILE *f = open_file(file, "rb")) {
                    std::fseek(f, 0, SEEK_END);
                    long size = std::ftell(f);
                    std::fseek(f, 0, SEEK_SET);
                    if (size > 0 && size % 4 == 0) {
                        words.resize(size_t(size) / 4);
                        if (std::fread(words.data(), 4, words.size(), f) != words.size()) words.clear();
                    }
                    std::fclose(f);
                }
            }
            if (!words.empty()) {
                VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                smi.codeSize = words.size() * 4;
                smi.pCode = words.data();
                VkShaderModule module = VK_NULL_HANDLE;
                bool ok = vkCreateShaderModule(device_, &smi, nullptr, &module) == VK_SUCCESS;
                std::lock_guard g(lock_);
                Shader &shader = shaders_[work.shader_key];
                shader.module = module;
                shader.done = true;
                shader.failed = !ok;
                (ok ? loaded : failed)++;
                continue;
            }
            std::string text = translated_shader(*work.program, work.entry, store_);
            if (dump) {
                char name[512];
                std::snprintf(name, sizeof name, "%s/%016llx.vert", dump, (unsigned long long)work.shader_key);
                if (FILE *f = std::fopen(name, "wb")) std::fwrite(text.data(), 1, text.size(), f), std::fclose(f);
            }
            std::vector<uint32_t> spv;
            std::string why;
            VkShaderModule module = VK_NULL_HANDLE;
            bool ok = spirv.vertex(text, spv, why);
            if (ok) {
                VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                smi.codeSize = spv.size() * 4;
                smi.pCode = spv.data();
                ok = vkCreateShaderModule(device_, &smi, nullptr, &module) == VK_SUCCESS;
                if (ok && !file.empty()) {
                    if (FILE *f = open_file(file, "wb")) std::fwrite(smi.pCode, 1, smi.codeSize, f), std::fclose(f);
                }
            } else {
                std::fprintf(stderr, "a vertex program did not translate: %s\n", why.c_str());
            }
            std::lock_guard g(lock_);
            Shader &shader = shaders_[work.shader_key];
            shader.module = module;
            shader.done = true;
            shader.failed = !ok;
            (ok ? compiled : failed)++;
            continue;
        }
        VkShaderModule module = VK_NULL_HANDLE;
        if (work.shader_key) {
            std::lock_guard g(lock_);
            module = shaders_[work.shader_key].module;
        }
        VkPipeline pipe = make_(work.key, work.has_depth, module, work.shape_hash ? work.shape.data() : nullptr);
        if (!pipe) std::fprintf(stderr, "a pipeline was not made\n");
        std::lock_guard g(lock_);
        pipelines_[{work.shader_key, work.key, work.shape_hash}] = pipe;
        pipelines_made++;
        if (work.shape_hash) {
            if (!pipe) failed_.insert({work.shader_key, work.key, work.shape_hash});
            made_.notify_all();
        }
    }
}

} // namespace runtime
