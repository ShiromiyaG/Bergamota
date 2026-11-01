// The pipelines, and a draw: the PICA's state as Vulkan state. (impl.h)

#include "impl.h"

namespace runtime {

using Impl = VulkanRenderer::Impl;

VkPipeline Impl::pipeline(uint64_t key, bool has_depth) {
    auto it = pipelines.find(key);
    if (it != pipelines.end()) return it->second;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
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
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &binding;
    vin.vertexAttributeDescriptionCount = 8;
    vin.pVertexAttributeDescriptions = attributes;
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
    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gi, nullptr, &pipe);
    pipelines[key] = pipe;
    return pipe;
}

void Impl::render(Pica &p) {
    const auto &r = p.regs;
    self.draws++;
    if (triangles.empty()) return;
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
    if (rendering && (color_target != color || depth_target != depth)) end_rendering();

    VkDeviceSize vsize = triangles.size() * sizeof(Vertex);
    if (vsize > VERTEX_RING) return;
    State st{};
    for (size_t i = 0; i < std::size(STATE_REGS); i++) st.regs[i] = r[STATE_REGS[i]];
    st.depth[0] = f24(r[DEPTH_RANGE]);
    st.depth[1] = f24(r[DEPTH_NEAR]);
    st.depth[2] = float(r[DEPTHMAP_ENABLE] & 1);
    pica::LightingBlock lit;
    lighting.decode(r.data(), lit);
    // into the rings; a ring that runs out submits, which leaves what
    // went before in the other slot, so then it all goes again
    VkDeviceSize voff, uoff;
    for (;;) {
        uint64_t serial = submitted;
        voff = take(vertices, vertex_at, vsize, 16);
        std::memcpy(vertices.mapped + voff, triangles.data(), vsize);
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
                pica::decode_light_tables(p.light_tables, light_tables);
                tables_decoded = p.tables_version;
            }
            tables_offset = take(uniforms, uniform_at, sizeof light_tables, 256);
            std::memcpy(uniforms.mapped + tables_offset, light_tables.data(), sizeof light_tables);
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
        rendering = true;
        color_target = color, depth_target = depth;
    }

    // the pipeline: blending (or not, for the logic op) and the color mask
    bool blend = (r[COLOR_OPERATION] >> 8) & 1;
    uint32_t color_mask = r[COLOR_BUFFER_WRITE] & 0xF ? (mask_reg >> 8) & 15 : 0;
    uint64_t key = uint64_t(blend) | uint64_t(color_mask) << 4 | uint64_t(blend ? r[BLEND_FUNC] & 0xFFFF0707 : 0) << 8 |
                   uint64_t(depth != nullptr) << 40;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(key, depth != nullptr));

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
    VkWriteDescriptorSet writes[4]{};
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
    push_descriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 4, writes);
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &voff);
    vkCmdDraw(cmd, uint32_t(triangles.size()), 1, 0, 0);
    self.triangles += triangles.size() / 3;

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

} // namespace runtime
