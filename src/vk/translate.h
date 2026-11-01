// The PICA's vertex programs translated to GLSL and compiled to SPIR-V
// (translate.cpp), each once, on threads of their own: the renderer draws
// with the interpreter (pica_gpu.vert) until a program's shader and
// pipeline are ready.
#ifndef RUNTIME_VK_TRANSLATE_H
#define RUNTIME_VK_TRANSLATE_H

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

namespace runtime {

struct ProgramBlock;

// the shape of a draw's state for the fragment shader (pica.frag's
// constants: SPECIALIZED, then K1-K46)
constexpr size_t SHAPE_SIZE = 47;
using Shape = std::array<uint32_t, SHAPE_SIZE>;

// a program's run(), in GLSL, for the frame pica_gpu.vert gives it
std::string translate_program(const ProgramBlock &program, uint32_t entry);
// the whole shader: the frame with run() swapped for the program's
std::string translated_shader(const ProgramBlock &program, uint32_t entry);

class Translator {
public:
    // pipeline(key, has depth, vertex shader or null for the renderer's,
    // shape or null for none) makes a pipeline; called on the translator's
    // threads
    using MakePipeline = std::function<VkPipeline(uint64_t, bool, VkShaderModule, const uint32_t *)>;
    Translator(VkDevice device, MakePipeline make);
    ~Translator();

    // the pipeline for a draw with the program (by its hash, at entry):
    // null until it is ready, asked for the first time it is not
    VkPipeline pipeline(const std::shared_ptr<const ProgramBlock> &program, uint64_t program_hash, uint32_t entry,
                        uint64_t key, bool has_depth);

    // the same with a fragment shader made for the draw's shape, for the
    // program's shader (shader_key) or the renderer's own (0): null until
    // it is ready
    VkPipeline specialized(uint64_t shader_key, uint64_t key, bool has_depth, const Shape &shape, uint64_t shape_hash);
    static uint64_t shader_key(uint64_t program_hash, uint32_t entry) {
        return program_hash ^ (uint64_t(entry) + 1) * 0x9E3779B97F4A7C15ull;
    }

    // the shaders made kept in folder, as <key>.spv, and taken from there
    void cache_in(const std::filesystem::path &folder);

    uint64_t compiled = 0, loaded = 0, failed = 0, pipelines_made = 0;

private:
    struct Work {
        std::shared_ptr<const ProgramBlock> program; // a shader to make, or
        uint64_t shader_key;
        uint32_t entry;
        uint64_t key;      // a pipeline to make with it
        bool has_depth;
        Shape shape;       // and a shape, when shape_hash is not 0
        uint64_t shape_hash;
    };
    struct Shader {
        VkShaderModule module = VK_NULL_HANDLE;
        bool done = false, failed = false;
    };
    void run();

    VkDevice device_;
    MakePipeline make_;
    std::mutex lock_;
    std::condition_variable wake_;
    std::deque<Work> queue_;
    std::deque<Work> later_; // the pipelines for a shape, after all the rest
    std::unordered_map<uint64_t, Shader> shaders_;
    // by the shader, the pipeline's key and the shape's hash (0 none); null
    // while it is being made
    std::map<std::tuple<uint64_t, uint64_t, uint64_t>, VkPipeline> pipelines_;
    bool stop_ = false;
    std::filesystem::path cache_;
    std::vector<std::thread> threads_; // a few: a driver takes a while over each pipeline
};

} // namespace runtime

#endif
