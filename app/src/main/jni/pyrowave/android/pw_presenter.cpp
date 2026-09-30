#include "pw_presenter.h"

#include "shaders/shaders_spv.h"

#include <Limelight.h>

#include <algorithm>

namespace {

struct PushConstants {
    float row0[4];
    float row1[4];
    float row2[4];
    float offset[4];
    float target[4];
    float videoRect[4];
    int32_t rotation;
};

// YCbCr to RGB rows for normalized code values, with range scaling folded in
void colorMatrix(const PwColor& color, bool tenBit, PushConstants& pc)
{
    float kr, kb;
    switch (color.colorspace) {
    case COLORSPACE_REC_601:
        kr = 0.299f;
        kb = 0.114f;
        break;
    case COLORSPACE_REC_2020:
        kr = 0.2627f;
        kb = 0.0593f;
        break;
    case COLORSPACE_REC_709:
    default:
        kr = 0.2126f;
        kb = 0.0722f;
        break;
    }
    const float kg = 1.0f - kr - kb;

    const float maxCode = tenBit ? 1023.0f : 255.0f;
    const float shift = tenBit ? 4.0f : 1.0f; // 10-bit code values are 8-bit ones times 4
    float yOffset, yScale, cScale;
    if (color.fullRange) {
        yOffset = 0.0f;
        yScale = 1.0f;
        cScale = 1.0f;
    }
    else {
        yOffset = 16.0f * shift / maxCode;
        yScale = maxCode / (219.0f * shift);
        cScale = maxCode / (224.0f * shift);
    }
    const float cOffset = 128.0f * shift / maxCode;

    const float crToR = 2.0f * (1.0f - kr);
    const float cbToB = 2.0f * (1.0f - kb);
    const float cbToG = -cbToB * kb / kg;
    const float crToG = -crToR * kr / kg;

    const float rows[3][3] = {
        { yScale, 0.0f, crToR * cScale },
        { yScale, cbToG * cScale, crToG * cScale },
        { yScale, cbToB * cScale, 0.0f },
    };
    float* out[3] = { pc.row0, pc.row1, pc.row2 };
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            out[r][c] = rows[r][c];
        }
        out[r][3] = 0.0f;
    }
    pc.offset[0] = yOffset;
    pc.offset[1] = cOffset;
    pc.offset[2] = cOffset;
    pc.offset[3] = 0.0f;
}

VkShaderModule createShader(VkDevice device, const uint32_t* code, size_t size)
{
    VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = size;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &module);
    return module;
}

}

int pwRotationFromTransform(VkSurfaceTransformFlagBitsKHR transform)
{
    switch (transform) {
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: return 1;
    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return 2;
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return 3;
    default: return 0;
    }
}

PwPresenter::~PwPresenter()
{
    if (m_Vulkan == nullptr) {
        return;
    }
    VkDevice device = m_Vulkan->device();
    destroyPipeline();
    if (m_VertexShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_VertexShader, nullptr);
    }
    if (m_FragmentShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_FragmentShader, nullptr);
    }
    if (m_PipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
    }
    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_DescriptorPool, nullptr);
    }
    if (m_SetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_SetLayout, nullptr);
    }
    if (m_Sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_Sampler, nullptr);
    }
}

bool PwPresenter::initialize(PwVulkan& vulkan, const PwDecoder& decoder, std::string& error)
{
    m_Vulkan = &vulkan;
    m_VideoWidth = decoder.config().width;
    m_VideoHeight = decoder.config().height;
    m_TenBit = decoder.config().tenBit;
    VkDevice device = vulkan.device();

    // Bilinear sampling with normalized coordinates reconstructs 4:2:0 chroma
    // sited at the centre of each 2x2 quad
    VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_Sampler) != VK_SUCCESS) {
        error = "vkCreateSampler failed";
        return false;
    }

    const VkSampler samplers[3] = { m_Sampler, m_Sampler, m_Sampler };
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding bindings[3] = { binding, binding, binding };
    for (uint32_t i = 0; i < 3; i++) {
        bindings[i].binding = i;
        bindings[i].pImmutableSamplers = &samplers[i];
    }
    VkDescriptorSetLayoutCreateInfo setLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setLayoutInfo.bindingCount = 3;
    setLayoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &m_SetLayout) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout failed";
        return false;
    }

    const uint32_t surfaceCount = uint32_t(decoder.surfaceCount());
    VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * surfaceCount };
    VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = surfaceCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_DescriptorPool) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed";
        return false;
    }

    // One set per output surface; the planes never change
    m_Sets.resize(surfaceCount);
    std::vector<VkDescriptorSetLayout> layouts(surfaceCount, m_SetLayout);
    VkDescriptorSetAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = surfaceCount;
    allocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &allocInfo, m_Sets.data()) != VK_SUCCESS) {
        error = "vkAllocateDescriptorSets failed";
        return false;
    }
    for (uint32_t s = 0; s < surfaceCount; s++) {
        VkDescriptorImageInfo images[3];
        VkWriteDescriptorSet writes[3];
        for (uint32_t plane = 0; plane < 3; plane++) {
            images[plane] = { VK_NULL_HANDLE, decoder.surface(int(s)).views[plane], VK_IMAGE_LAYOUT_GENERAL };
            writes[plane] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[plane].dstSet = m_Sets[s];
            writes[plane].dstBinding = plane;
            writes[plane].descriptorCount = 1;
            writes[plane].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[plane].pImageInfo = &images[plane];
        }
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    }

    VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants) };
    VkPipelineLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_SetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed";
        return false;
    }

    m_VertexShader = createShader(device, k_fullscreen_vert, sizeof(k_fullscreen_vert));
    m_FragmentShader = createShader(device, k_ycbcr_to_rgb_frag, sizeof(k_ycbcr_to_rgb_frag));
    if (m_VertexShader == VK_NULL_HANDLE || m_FragmentShader == VK_NULL_HANDLE) {
        error = "vkCreateShaderModule failed";
        return false;
    }
    return true;
}

void PwPresenter::destroyPipeline()
{
    if (m_Pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_Vulkan->device(), m_Pipeline, nullptr);
        m_Pipeline = VK_NULL_HANDLE;
    }
    m_TargetFormat = VK_FORMAT_UNDEFINED;
}

bool PwPresenter::setTargetFormat(VkFormat format, std::string& error)
{
    if (format == m_TargetFormat && m_Pipeline != VK_NULL_HANDLE) {
        return true;
    }
    destroyPipeline();

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_VertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_FragmentShader;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;

    VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = m_PipelineLayout;
    if (vkCreateGraphicsPipelines(m_Vulkan->device(), VK_NULL_HANDLE, 1, &info, nullptr, &m_Pipeline) != VK_SUCCESS) {
        m_Pipeline = VK_NULL_HANDLE;
        error = "vkCreateGraphicsPipelines failed";
        return false;
    }
    m_TargetFormat = format;
    return true;
}

void PwPresenter::record(VkCommandBuffer cmd, int surface, const PwColor& color,
                         VkImage target, VkImageView targetView, VkExtent2D extent, int rotation,
                         VkImageLayout finalLayout, VkPipelineStageFlags2 finalStage, VkAccessFlags2 finalAccess)
{
    VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = 0;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);

    // Every pixel is written (letterbox bars are black), so nothing is loaded
    VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    attachment.imageView = targetView;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    rendering.renderArea = { { 0, 0 }, extent };
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(cmd, &rendering);

    const VkViewport viewport = { 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
    const VkRect2D scissor = { { 0, 0 }, extent };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout, 0, 1,
                            &m_Sets[size_t(surface)], 0, nullptr);

    PushConstants pc = {};
    colorMatrix(color, m_TenBit, pc);
    pc.target[0] = float(extent.width);
    pc.target[1] = float(extent.height);
    pc.target[2] = 1.0f / float(extent.width);
    pc.target[3] = 1.0f / float(extent.height);

    // Fit the video inside the upright display area, preserving its aspect ratio
    const bool sideways = (rotation & 1) != 0;
    const float displayWidth = float(sideways ? extent.height : extent.width);
    const float displayHeight = float(sideways ? extent.width : extent.height);
    const float scale = std::min(displayWidth / float(m_VideoWidth), displayHeight / float(m_VideoHeight));
    const float rectWidth = float(m_VideoWidth) * scale / displayWidth;
    const float rectHeight = float(m_VideoHeight) * scale / displayHeight;
    pc.videoRect[0] = (1.0f - rectWidth) * 0.5f;
    pc.videoRect[1] = (1.0f - rectHeight) * 0.5f;
    pc.videoRect[2] = 1.0f / rectWidth;
    pc.videoRect[3] = 1.0f / rectHeight;
    pc.rotation = rotation;
    vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = finalStage;
    barrier.dstAccessMask = finalAccess;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = finalLayout;
    vkCmdPipelineBarrier2(cmd, &dependency);
}
