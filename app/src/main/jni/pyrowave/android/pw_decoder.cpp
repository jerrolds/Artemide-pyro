#include "pw_decoder.h"

#include <pyrowave.h>

namespace {

const char* resultString(pyrowave_result result)
{
    switch (result) {
    case PYROWAVE_SUCCESS: return "success";
    case PYROWAVE_TIMEOUT: return "timeout";
    case PYROWAVE_ERROR_GENERIC: return "generic error";
    case PYROWAVE_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case PYROWAVE_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
    case PYROWAVE_ERROR_OUT_OF_DEVICE_MEMORY: return "out of device memory";
    case PYROWAVE_ERROR_NO_VULKAN: return "no Vulkan";
    case PYROWAVE_ERROR_NOT_IMPLEMENTED: return "not implemented";
    case PYROWAVE_ERROR_UNSUPPORTED_EXTERNAL_HANDLE: return "unsupported external handle";
    case PYROWAVE_ERROR_FAILED_EXTERNAL_HANDLE: return "failed external handle";
    default: return "unknown error";
    }
}

void lockQueue(void* userdata)
{
    static_cast<PwVulkan*>(userdata)->queueLock().lock();
}

void unlockQueue(void* userdata)
{
    static_cast<PwVulkan*>(userdata)->queueLock().unlock();
}

}

PwDecoder::~PwDecoder()
{
    // Destroying the decoder waits for its GPU work to finish
    if (m_Decoder != nullptr) {
        pyrowave_decoder_destroy(m_Decoder);
    }
    if (m_Vulkan != nullptr) {
        vkDeviceWaitIdle(m_Vulkan->device());
        for (auto& surface : m_Surfaces) {
            destroySurface(surface);
        }
        if (m_DecodeSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_Vulkan->device(), m_DecodeSemaphore, nullptr);
        }
    }
    if (m_Device != nullptr) {
        pyrowave_device_destroy(m_Device);
    }
}

bool PwDecoder::createSurface(Surface& surface)
{
    VkDevice device = m_Vulkan->device();
    const VkFormat format = m_Config.tenBit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
    // The fragment path renders each plane; the compute path stores to it.
    // Transfers let a surface be filled without decoding (tests).
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        (m_FragmentPath ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_STORAGE_BIT);

    for (int plane = 0; plane < 3; plane++) {
        uint32_t width = uint32_t(m_Config.width);
        uint32_t height = uint32_t(m_Config.height);
        if (plane != 0 && !m_Config.chroma444) {
            width /= 2;
            height /= 2;
        }
        surface.extents[plane] = { width, height };

        VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &imageInfo, nullptr, &surface.images[plane]) != VK_SUCCESS) {
            m_LastError = "vkCreateImage failed";
            return false;
        }

        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, surface.images[plane], &requirements);
        VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = m_Vulkan->findMemoryType(requirements.memoryTypeBits,
                                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX ||
                vkAllocateMemory(device, &allocInfo, nullptr, &surface.memory[plane]) != VK_SUCCESS ||
                vkBindImageMemory(device, surface.images[plane], surface.memory[plane], 0) != VK_SUCCESS) {
            m_LastError = "out of device memory for output surfaces";
            return false;
        }

        VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = surface.images[plane];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(device, &viewInfo, nullptr, &surface.views[plane]) != VK_SUCCESS) {
            m_LastError = "vkCreateImageView failed";
            return false;
        }
    }
    return true;
}

void PwDecoder::destroySurface(Surface& surface)
{
    VkDevice device = m_Vulkan->device();
    for (int plane = 0; plane < 3; plane++) {
        if (surface.views[plane] != VK_NULL_HANDLE) {
            vkDestroyImageView(device, surface.views[plane], nullptr);
        }
        if (surface.images[plane] != VK_NULL_HANDLE) {
            vkDestroyImage(device, surface.images[plane], nullptr);
        }
        if (surface.memory[plane] != VK_NULL_HANDLE) {
            vkFreeMemory(device, surface.memory[plane], nullptr);
        }
    }
    surface = Surface();
}

bool PwDecoder::initialize(PwVulkan& vulkan, const Config& config, int surfaceCount)
{
    if (config.width <= 0 || config.height <= 0 || surfaceCount <= 0) {
        m_LastError = "invalid decoder configuration";
        return false;
    }
    if (!config.chroma444 && ((config.width | config.height) & 1)) {
        m_LastError = "4:2:0 streams need an even width and height";
        return false;
    }

    m_Vulkan = &vulkan;
    m_Config = config;
    m_Geometry = { config.width, config.height, config.chroma444 };

    pyrowave_device_create_queue_info queueInfo = {};
    queueInfo.queue = vulkan.queue();
    queueInfo.familyIndex = vulkan.queueFamily();
    queueInfo.index = 0;

    pyrowave_device_create_info deviceInfo = {};
    deviceInfo.GetInstanceProcAddr = vkGetInstanceProcAddr;
    deviceInfo.instance = vulkan.instance();
    deviceInfo.physical_device = vulkan.physicalDevice();
    deviceInfo.device = vulkan.device();
    deviceInfo.instance_create_info = vulkan.instanceCreateInfo();
    deviceInfo.device_create_info = vulkan.deviceCreateInfo();
    deviceInfo.queue_info = &queueInfo;
    deviceInfo.queue_info_count = 1;
    deviceInfo.queue_lock_callback = lockQueue;
    deviceInfo.queue_unlock_callback = unlockQueue;
    deviceInfo.userdata = &vulkan;

    pyrowave_result result = pyrowave_create_device(&deviceInfo, &m_Device);
    if (result != PYROWAVE_SUCCESS) {
        m_Device = nullptr;
        m_LastError = std::string("PyroWave cannot use this Vulkan device: ") + resultString(result);
        return false;
    }
    // Decode on the graphics queue the renderer presents from
    pyrowave_device_set_queue_type(m_Device, VK_QUEUE_GRAPHICS_BIT);

    m_FragmentPath = pyrowave_decoder_device_prefers_fragment_path(m_Device);

    pyrowave_decoder_create_info decoderInfo = {};
    decoderInfo.device = m_Device;
    decoderInfo.width = config.width;
    decoderInfo.height = config.height;
    decoderInfo.chroma = config.chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    decoderInfo.fragment_path = m_FragmentPath;
    result = pyrowave_decoder_create(&decoderInfo, &m_Decoder);
    if (result != PYROWAVE_SUCCESS) {
        m_Decoder = nullptr;
        m_LastError = std::string("decoder creation failed: ") + resultString(result);
        return false;
    }

    VkSemaphoreTypeCreateInfo timelineInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semaphoreInfo.pNext = &timelineInfo;
    if (vkCreateSemaphore(vulkan.device(), &semaphoreInfo, nullptr, &m_DecodeSemaphore) != VK_SUCCESS) {
        m_DecodeSemaphore = VK_NULL_HANDLE;
        m_LastError = "vkCreateSemaphore failed";
        return false;
    }

    m_Surfaces.resize(size_t(surfaceCount));
    for (auto& surface : m_Surfaces) {
        if (!createSurface(surface)) {
            return false;
        }
    }

    // PyroWave performs no layout transitions on application images, so move
    // every plane to GENERAL once; it stays there for decoding and sampling.
    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = vulkan.queueFamily();
    VkCommandPool pool;
    if (vkCreateCommandPool(vulkan.device(), &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        m_LastError = "vkCreateCommandPool failed";
        return false;
    }

    VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdInfo.commandPool = pool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(vulkan.device(), &cmdInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    std::vector<VkImageMemoryBarrier2> barriers;
    for (const auto& surface : m_Surfaces) {
        for (auto image : surface.images) {
            VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            barriers.push_back(barrier);
        }
    }
    VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dependency.imageMemoryBarrierCount = uint32_t(barriers.size());
    dependency.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    VkResult vr;
    {
        std::lock_guard<std::mutex> guard(vulkan.queueLock());
        vr = vkQueueSubmit(vulkan.queue(), 1, &submitInfo, VK_NULL_HANDLE);
        if (vr == VK_SUCCESS) {
            vr = vkQueueWaitIdle(vulkan.queue());
        }
    }
    vkDestroyCommandPool(vulkan.device(), pool, nullptr);
    if (vr != VK_SUCCESS) {
        m_LastError = "initializing output surfaces failed";
        return false;
    }

    return true;
}

bool PwDecoder::decode(const uint8_t* data, size_t size,
                       const std::vector<PyroWaveFraming::Segment>& packets, size_t criticalPackets,
                       int surfaceIndex, VkSemaphore waitSemaphore, uint64_t waitValue, uint64_t& decodeValue)
{
    m_LastFramePartial = false;
    if (!PyroWaveFraming::parse(data, size, packets, criticalPackets, m_Geometry, m_Parsed, m_LastError)) {
        return false;
    }
    m_LastFraming = m_Parsed.framing;

    // Every frame is independent. Clearing first keeps the 3-bit sequence
    // counter from treating a frame after a long drop as stale.
    pyrowave_decoder_clear(m_Decoder);
    for (const auto& span : m_Parsed.spans) {
        const pyrowave_result result = pyrowave_decoder_push_packet(m_Decoder, data + span.offset, span.size);
        if (result != PYROWAVE_SUCCESS) {
            m_LastError = std::string("decoder rejected a packet: ") + resultString(result);
            return false;
        }
    }

    if (!m_Parsed.partial) {
        if (!pyrowave_decoder_decode_is_ready(m_Decoder, false)) {
            m_LastError = "frame is incomplete";
            return false;
        }
    }
    else {
        // A partial frame decodes if its coarsest wavelet level is intact and
        // more than 90% of its blocks arrived; missing detail decodes as blur.
        // The parser checks the coarsest level: PyroWave's own check cannot tell
        // a lost block from an all-zero one that was never sent.
        if (!m_Parsed.coarseLevelIntact) {
            m_LastError = "part of the coarsest wavelet level was lost";
            return false;
        }
        if (!pyrowave_decoder_decode_is_ready_with_sideband(m_Decoder, true, 0, 0.9f, nullptr, 0)) {
            m_LastError = "too little of the frame arrived (" + std::to_string(m_Parsed.blockRecords) +
                          " of " + std::to_string(m_Parsed.announcedBlocks) + " blocks)";
            return false;
        }
    }
    m_LastFramePartial = m_Parsed.partial;

    const Surface& surface = m_Surfaces[size_t(surfaceIndex)];
    pyrowave_gpu_buffers buffers = {};
    for (int plane = 0; plane < 3; plane++) {
        pyrowave_image_view& view = buffers.planes[plane];
        view.image = surface.images[plane];
        view.width = surface.extents[plane].width;
        view.height = surface.extents[plane].height;
        view.image_format = m_Config.tenBit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
        view.view_format = view.image_format;
        view.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        view.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // The images are ours and stay in GENERAL, so no image ownership is
    // exchanged; only the semaphores order this decode against rendering.
    pyrowave_gpu_sync_operation acquire = {};
    acquire.sync.semaphore = waitSemaphore;
    acquire.sync.value = waitValue;

    const uint64_t nextValue = m_DecodeValue + 1;
    pyrowave_gpu_sync_operation release = {};
    release.sync.semaphore = m_DecodeSemaphore;
    release.sync.value = nextValue;

    const pyrowave_result result = pyrowave_decoder_decode_gpu_buffer(
        m_Decoder, waitSemaphore != VK_NULL_HANDLE ? &acquire : nullptr, &release, &buffers);
    if (result != PYROWAVE_SUCCESS) {
        m_LastError = std::string("decode submission failed: ") + resultString(result);
        return false;
    }

    m_DecodeValue = nextValue;
    decodeValue = nextValue;
    return true;
}
