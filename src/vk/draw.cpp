// The pipelines, and a draw: the PICA's state as Vulkan state. (impl.h)

#include "impl.h"

namespace runtime {

using Impl = VulkanRenderer::Impl;

VkPipeline Impl::pipeline(uint64_t key, bool has_depth) {
    auto it = pipelines.find(key);
    if (it != pipelines.end()) return it->second;
    VkPipeline pipe = make_pipeline(key, has_depth, (key >> 41) & 1 ? vert_gpu : vert);
    pipelines[key] = pipe;
    return pipe;
}

// a pipeline with a vertex shader: the CPU's vertices as attributes, or none
// for the GPU's shader (key bit 41). touches nothing of the renderer's, so
// the translator's thread makes them too
VkPipeline Impl::make_pipeline(uint64_t key, bool has_depth, VkShaderModule vertex) const {
    bool on_gpu = (key >> 41) & 1, w_buffer = (key >> 42) & 1;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, w_buffer ? frag_wbuffer : frag,
         "main", nullptr},
    };
    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[8] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, quaternion)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv0)},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv1)},
        {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv2)},
        {6, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, w0)},
        {7, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, view)},
    };
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (!on_gpu) {
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &binding;
        vin.vertexAttributeDescriptionCount = 8;
        vin.pVertexAttributeDescriptions = attributes;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // the key: blend on (bit 0), the equations and factors, the color mask
    static constexpr VkBlendFactor FACTOR[16] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ONE};
    static constexpr VkBlendOp EQUATION[8] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                              VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
    uint32_t func = uint32_t(key >> 8);
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = key & 1;
    blend.colorBlendOp = EQUATION[func & 7];
    blend.alphaBlendOp = EQUATION[(func >> 8) & 7];
    blend.srcColorBlendFactor = FACTOR[(func >> 16) & 15];
    blend.dstColorBlendFactor = FACTOR[(func >> 20) & 15];
    blend.srcAlphaBlendFactor = FACTOR[(func >> 24) & 15];
    blend.dstAlphaBlendFactor = FACTOR[(func >> 28) & 15];
    blend.colorWriteMask = (key >> 4) & 15;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    };
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = uint32_t(std::size(dynamic));
    dyn.pDynamicStates = dynamic;
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &color_format;
    ri.depthAttachmentFormat = has_depth ? depth_format : VK_FORMAT_UNDEFINED;
    ri.stencilAttachmentFormat = has_depth ? depth_format : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vin;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dyn;
    gi.layout = layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(device, pipeline_cache, 1, &gi, nullptr, &pipe);
    return pipe;
}

void Impl::render(Pica &p) {
    const auto &r = p.regs;
    self.stats.draws++;
    const bool on_gpu = gpu_draw != nullptr;
    if (on_gpu ? gpu_draw->indices.empty() : triangles.empty()) return;
    uint32_t dims = r[FRAMEBUFFER_DIMENSIONS], w = dims & 0x7FF, h = ((dims >> 12) & 0x3FF) + 1;
    uint32_t color_address = (r[COLOR_BUFFER_ADDRESS] & 0x0FFFFFFF) << 3;
    uint32_t depth_address = (r[DEPTH_BUFFER_ADDRESS] & 0x0FFFFFFF) << 3;
    if (!color_address || !w || w > 1024 || h > 1024) return;
    uint32_t mask_reg = r[DEPTH_COLOR_MASK], stencil_reg = r[STENCIL_TEST];
    bool depth_test = mask_reg & 1, depth_write = (mask_reg >> 12) & 1 && (r[DEPTH_STENCIL_WRITE] & 2);
    bool stencil = (stencil_reg & 1) && (r[DEPTH_BUFFER_FORMAT] & 3) == 3;
    bool use_depth = depth_address && (depth_test || depth_write || stencil);

    // what the draw reads first, outside a pass
    VkImageView views[3];
    for (int u = 0; u < 3; u++) views[u] = forced_views ? forced_views[u] : texture(p, u);
    Surface *color = surface(color_address, w, h, (r[COLOR_BUFFER_FORMAT] >> 16) & 7, false);
    Surface *depth = use_depth ? surface(depth_address, w, h, r[DEPTH_BUFFER_FORMAT] & 3, true) : nullptr;
    if (!color) return;
    if (between && !replaying) note_draw(p, views);
    // a texture that is this pass's target cannot be read inside it
    if (rendering && (color_target != color || depth_target != depth)) end_cause = 0, end_rendering();

    VkDeviceSize vsize = triangles.size() * sizeof(Vertex);
    VkDeviceSize in_size = 0, index_size = 0, before_size = 0;
    // a frame between whose draws both kept their outputs takes them, and
    // runs nothing; a draw of the game's with frames between keeps its own
    const bool from_outputs = on_gpu && replaying && gpu_draw->outputs != ~0u && gpu_draw->output_turn == output_turn &&
                              (!gpu_before || (gpu_before->outputs != ~0u && gpu_before->output_turn + 1 == output_turn));
    uint32_t store = ~0u;
    if (on_gpu && between && !replaying && store_outputs && gpu_draw->stride) {
        VkDeviceSize need = VkDeviceSize(gpu_draw->inputs.size() / gpu_draw->stride) * 24 * sizeof(float);
        if (output_at + need <= OUTPUT_HALF) {
            store = uint32_t(((output_turn & 1) * OUTPUT_HALF + output_at) / sizeof(float));
            output_at += need;
        }
        // the frame between's copy of this draw, made just before, learns where
        if (!ops_now.empty() && ops_now.back().gpu && ops_now.back().kind == Op::Draw) {
            ops_now.back().gpu->outputs = store;
            ops_now.back().gpu->output_turn = output_turn;
        }
    }
    if (on_gpu) {
        vsize = 0;
        in_size = from_outputs ? 0 : gpu_draw->inputs.size() * sizeof(Vec4);
        index_size = gpu_draw->indices.size() * sizeof(uint32_t);
        before_size = gpu_before && !from_outputs ? gpu_before->inputs.size() * sizeof(Vec4) : 0;
    }
    if (vsize + in_size + index_size + before_size + 1024 > VERTEX_RING) return;
    State st{};
    for (size_t i = 0; i < std::size(STATE_REGS); i++) st.regs[i] = r[STATE_REGS[i]];
    st.depth[0] = f24(r[DEPTH_RANGE]);
    st.depth[1] = f24(r[DEPTH_NEAR]);
    st.depth[2] = float(r[DEPTHMAP_ENABLE] & 1);
    pica::LightingBlock lit;
    lighting.decode(r.data(), lit);
    // into the rings; a ring that runs out submits, which leaves what
    // went before in the other slot, so then it all goes again
    VkDeviceSize voff = 0, uoff, in_off = 0, index_off = 0, unit_off = 0, before_off = 0, before_unit_off = 0;
    for (;;) {
        uint64_t serial = submitted;
        if (on_gpu) {
            in_off = take(vertices, vertex_at, std::max<VkDeviceSize>(in_size, 16), 256);
            std::memcpy(vertices.mapped + in_off, gpu_draw->inputs.data(), in_size);
            index_off = take(vertices, vertex_at, index_size, 16);
            std::memcpy(vertices.mapped + index_off, gpu_draw->indices.data(), index_size);
            unit_off = take(uniforms, uniform_at, sizeof(UnitBlock), 256);
            std::memcpy(uniforms.mapped + unit_off, &gpu_draw->unit, sizeof(UnitBlock));
            before_off = in_off, before_unit_off = unit_off;
            if (gpu_before && !from_outputs) {
                before_off = take(vertices, vertex_at, before_size, 256);
                std::memcpy(vertices.mapped + before_off, gpu_before->inputs.data(), before_size);
                before_unit_off = take(uniforms, uniform_at, sizeof(UnitBlock), 256);
                std::memcpy(uniforms.mapped + before_unit_off, &gpu_before->unit, sizeof(UnitBlock));
            }
            // the program, kept while it and the slot stay the same
            if (program_serial != submitted || program_sent != gpu_draw->program_hash) {
                program_offset = take(uniforms, uniform_at, sizeof(ProgramBlock), 256);
                std::memcpy(uniforms.mapped + program_offset, gpu_draw->program.get(), sizeof(ProgramBlock));
                program_sent = gpu_draw->program_hash;
                program_serial = submitted;
            }
        } else {
            voff = take(vertices, vertex_at, vsize, 16);
            std::memcpy(vertices.mapped + voff, triangles.data(), vsize);
        }
        uoff = take(uniforms, uniform_at, sizeof st, 256);
        std::memcpy(uniforms.mapped + uoff, &st, sizeof st);
        // the lighting block and the tables, kept while they and the
        // slot stay the same: most draws in a row share them
        if (light_serial != submitted || std::memcmp(&lit, &light_block, sizeof lit) != 0) {
            light_offset = take(uniforms, uniform_at, sizeof lit, 256);
            std::memcpy(uniforms.mapped + light_offset, &lit, sizeof lit);
            light_block = lit;
            light_serial = submitted;
        }
        if (tables_serial != submitted || tables_sent != p.tables_version) {
            if (tables_decoded != p.tables_version) {
                pica::decode_light_tables(p.light_tables, light_tables.light);
                // the fog's: the factor in 0.11 above the step, 13 bits with the sign on top
                for (size_t e = 0; e < p.fog_table.size(); e++) {
                    uint32_t value = p.fog_table[e];
                    light_tables.fog[e] = {float((value >> 13) & 0x7FF) / 2048.0f, float(int32_t(value << 19) >> 19) / 2048.0f};
                }
                // the procedural texture's (Zakuro's proctex.rs): tables 0, 2
                // and 3 a 0.12 value and a 12-bit signed step, 4 the colors,
                // 5 their steps as halved signed bytes
                static constexpr int MAPS[3] = {0, 2, 3};
                for (int m = 0; m < 3; m++) {
                    for (int e = 0; e < 128; e++) {
                        uint32_t value = p.proctex_tables[MAPS[m]][e];
                        light_tables.maps[m][e] = {float(value & 0xFFF) / 4095.0f, float(int32_t(value << 8) >> 20) / 4095.0f};
                    }
                }
                for (int e = 0; e < 256; e++) {
                    for (int c = 0; c < 4; c++) {
                        light_tables.colors[e][c] = float((p.proctex_tables[4][e] >> (c * 8)) & 0xFF);
                        light_tables.steps[e][c] = float(int8_t(p.proctex_tables[5][e] >> (c * 8))) * 2.0f;
                    }
                }
                tables_decoded = p.tables_version;
            }
            tables_offset = take(uniforms, uniform_at, sizeof light_tables, 256);
            std::memcpy(uniforms.mapped + tables_offset, &light_tables, sizeof light_tables);
            tables_sent = p.tables_version;
            tables_serial = submitted;
        }
        if (submitted == serial) break;
    }

    if (!rendering) {
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = color->image.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo da = ca;
        if (depth) da.imageView = depth->image.view;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {w, h}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = depth ? &da : nullptr;
        ri.pStencilAttachment = depth ? &da : nullptr;
        vkCmdBeginRendering(cmd, &ri);
        self.stats.passes++;
        pass_scope = gpu_open(0);
        rendering = true;
        color_target = color, depth_target = depth;
    }

    // the pipeline: blending (or not, for the logic op) and the color mask
    bool blend = (r[COLOR_OPERATION] >> 8) & 1;
    uint32_t color_mask = r[COLOR_BUFFER_WRITE] & 0xF ? (mask_reg >> 8) & 15 : 0;
    uint64_t key = uint64_t(blend) | uint64_t(color_mask) << 4 | uint64_t(blend ? r[BLEND_FUNC] & 0xFFFF0707 : 0) << 8 |
                   uint64_t(depth != nullptr) << 40 | uint64_t(on_gpu) << 41 |
                   uint64_t(depth != nullptr && !(r[DEPTHMAP_ENABLE] & 1)) << 42;
    // the program's own shader once it is made, the interpreter until then
    VkPipeline pipe = VK_NULL_HANDLE;
    if (on_gpu && translator) {
        pipe = translator->pipeline(gpu_draw->program, gpu_draw->program_hash, gpu_draw->unit.misc[1], key, depth != nullptr);
    }
    if (on_gpu && pipe) self.stats.translated_draws++;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe ? pipe : pipeline(key, depth != nullptr));

    // the viewport, rows the PICA's way up
    float vw = f24(r[VIEWPORT_WIDTH]) * 2, vh = f24(r[VIEWPORT_HEIGHT]) * 2;
    float vx = float(int32_t(r[VIEWPORT_XY] << 22) >> 22), vy = float(int32_t((r[VIEWPORT_XY] >> 16) << 22) >> 22);
    VkViewport viewport{vx, float(h) - (vy + vh), vw, vh, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    uint32_t cull = r[FACE_CULLING] & 3;
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetCullMode(cmd, cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    static constexpr VkCompareOp COMPARE[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL,
                                               VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL,
                                               VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL};
    vkCmdSetDepthTestEnable(cmd, depth && (depth_test || depth_write));
    vkCmdSetDepthWriteEnable(cmd, depth && depth_write);
    vkCmdSetDepthCompareOp(cmd, depth_test ? COMPARE[(mask_reg >> 4) & 7] : VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cmd, depth && stencil);
    static constexpr VkStencilOp OPS[8] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
                                          VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                          VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
    uint32_t ops = r[STENCIL_OP];
    vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, OPS[ops & 7], OPS[(ops >> 8) & 7], OPS[(ops >> 4) & 7],
                      COMPARE[(stencil_reg >> 4) & 7]);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_reg >> 24);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, r[DEPTH_STENCIL_WRITE] & 1 ? (stencil_reg >> 8) & 0xFF : 0);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, (stencil_reg >> 16) & 0xFF);
    uint32_t bc = r[BLEND_COLOR];
    float constants[4] = {(bc & 0xFF) / 255.0f, (bc >> 8 & 0xFF) / 255.0f, (bc >> 16 & 0xFF) / 255.0f, (bc >> 24) / 255.0f};
    vkCmdSetBlendConstants(cmd, constants);

    VkDescriptorBufferInfo state_info{uniforms.buffer, uoff, sizeof st};
    VkDescriptorImageInfo images[3];
    for (int u = 0; u < 3; u++) images[u] = {sampler(r[UNIT_BASE[u] + 2]), views[u], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo light_info{uniforms.buffer, light_offset, sizeof light_block};
    VkDescriptorBufferInfo tables_info{uniforms.buffer, tables_offset, sizeof light_tables};
    VkWriteDescriptorSet writes[10]{};
    for (auto &w : writes) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &state_info;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 3;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = images;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &light_info;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &tables_info;
    if (!on_gpu) {
        push_descriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 4, writes);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &voff);
        vkCmdDraw(cmd, uint32_t(triangles.size()), 1, 0, 0);
        self.stats.triangles += triangles.size() / 3;
    } else {
        VkDescriptorBufferInfo gpu_info[6] = {
            {uniforms.buffer, program_offset, sizeof(ProgramBlock)},
            {uniforms.buffer, unit_off, sizeof(UnitBlock)},
            {vertices.buffer, in_off, std::max<VkDeviceSize>(in_size, 16)},
            {uniforms.buffer, before_unit_off, sizeof(UnitBlock)},
            {vertices.buffer, before_off, std::max<VkDeviceSize>(gpu_before && !from_outputs ? before_size : in_size, 16)},
            {outputs, 0, VK_WHOLE_SIZE},
        };
        static constexpr VkDescriptorType TYPES[6] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        for (int k = 0; k < 6; k++) {
            writes[4 + k].dstBinding = uint32_t(4 + k);
            writes[4 + k].descriptorCount = 1;
            writes[4 + k].descriptorType = TYPES[k];
            writes[4 + k].pBufferInfo = &gpu_info[k];
        }
        push_descriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 10, writes);
        VertexPush push{gpu_draw->stride, gpu_before ? gpu_before->stride : gpu_draw->stride, gpu_t, gpu_before ? 1u : 0u,
                        0, 0, store, 0};
        if (from_outputs) {
            push.between = 2;
            push.now_out = gpu_draw->outputs;
            push.before_out = gpu_before ? gpu_before->outputs : gpu_draw->outputs;
            self.stats.reused_draws++;
        }
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, &push);
        vkCmdBindIndexBuffer(cmd, vertices.buffer, index_off, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, uint32_t(gpu_draw->indices.size()), 1, 0, 0, 0);
        self.stats.triangles += gpu_draw->indices.size() / 3;
    }

    // drawn: memory is behind until written back, which the CPU asks for
    // through the guards
    color->dirty = true;
    color->version++;
    guard(*color);
    if (depth && (depth_write || stencil)) {
        depth->dirty = true;
        depth->version++;
        guard(*depth);
    }
}

// the vertex shader unit as the GPU's shader reads it: its program (made
// again only when it changes) and the draw's uniforms and output map
void Impl::gpu_unit(Pica &p, GpuDraw &d) {
    const ShaderUnit &u = p.vertex_shader;
    if (program_version != u.version || !program_now) {
        auto block = std::make_shared<ProgramBlock>();
        std::memcpy(block->code, u.program.data(), sizeof block->code);
        std::memcpy(block->desc, u.descriptors.data(), sizeof block->desc);
        program_hash = hash_bytes(reinterpret_cast<const uint8_t *>(block.get()), sizeof(ProgramBlock));
        program_now = std::move(block);
        program_version = u.version;
    }
    d.program = program_now;
    d.program_hash = program_hash;
    UnitBlock &b = d.unit;
    std::memcpy(b.f, u.floats.data(), sizeof b.f);
    for (int k = 0; k < 4; k++) {
        for (int c = 0; c < 4; c++) b.i[k][c] = u.ints[k][c];
    }
    b.misc[0] = u.bools;
    b.misc[1] = u.entry;
    b.misc[2] = p.regs[VS_BLOCK + OUTPUT_MASK];
    b.misc[3] = p.regs[OUTPUT_TOTAL];
    for (int s = 0; s < 7; s++) b.map[s] = p.regs[OUTPUT_MAP + s];
    b.map[7] = 0;
}

} // namespace runtime
