#pragma once

// Converts a decoded PyroWave surface to RGB in one full-screen pass. The
// target can be a swapchain image (PwSwapchain) or any other colour image,
// which is how the desktop test checks the output.

#include "pw_decoder.h"
#include "pw_vulkan.h"

#include <string>
#include <vector>

struct PwColor {
    int colorspace = 1;     // COLORSPACE_REC_601/709/2020
    bool fullRange = false; // COLOR_RANGE_FULL
};

class PwPresenter {
public:
    PwPresenter() = default;
    ~PwPresenter();

    PwPresenter(const PwPresenter&) = delete;
    PwPresenter& operator=(const PwPresenter&) = delete;

    bool initialize(PwVulkan& vulkan, const PwDecoder& decoder, std::string& error);

    // Builds the pipeline for a target format; cheap if it is unchanged.
    bool setTargetFormat(VkFormat format, std::string& error);

    // Records the conversion of one decoded surface into target, whose
    // previous contents are discarded. The decoder's output must already be
    // visible to fragment shaders (the caller waits on its semaphore).
    // rotation is the surface transform in quarter turns (0-3). On return
    // target is in finalLayout, made available to finalStage/finalAccess.
    void record(VkCommandBuffer cmd, int surface, const PwColor& color,
                VkImage target, VkImageView targetView, VkExtent2D extent, int rotation,
                VkImageLayout finalLayout, VkPipelineStageFlags2 finalStage, VkAccessFlags2 finalAccess);

private:
    void destroyPipeline();

    PwVulkan* m_Vulkan = nullptr;
    int m_VideoWidth = 0;
    int m_VideoHeight = 0;
    bool m_TenBit = false;

    VkSampler m_Sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_Sets;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkShaderModule m_VertexShader = VK_NULL_HANDLE;
    VkShaderModule m_FragmentShader = VK_NULL_HANDLE;
    VkPipeline m_Pipeline = VK_NULL_HANDLE;
    VkFormat m_TargetFormat = VK_FORMAT_UNDEFINED;
};

// Converts a VkSurfaceTransformFlagBitsKHR to quarter turns for record()
int pwRotationFromTransform(VkSurfaceTransformFlagBitsKHR transform);
