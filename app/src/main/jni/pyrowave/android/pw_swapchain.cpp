#include "pw_swapchain.h"
#include "pw_presenter.h"

#include <Limelight.h>

#include <android/log.h>
#include <android/native_window.h>

#include <algorithm>

PwSwapchain::~PwSwapchain()
{
    destroy();
}

bool PwSwapchain::create(PwVulkan& vulkan, ANativeWindow* window, std::string& error)
{
    destroy();
    m_Vulkan = &vulkan;
    m_Window = window;
    ANativeWindow_acquire(m_Window);

    VkAndroidSurfaceCreateInfoKHR surfaceInfo = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
    surfaceInfo.window = window;
    if (vkCreateAndroidSurfaceKHR(vulkan.instance(), &surfaceInfo, nullptr, &m_Surface) != VK_SUCCESS) {
        m_Surface = VK_NULL_HANDLE;
        error = "vkCreateAndroidSurfaceKHR failed";
        destroy();
        return false;
    }

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(vulkan.physicalDevice(), vulkan.queueFamily(), m_Surface, &supported);
    if (!supported) {
        error = "the graphics queue cannot present to this surface";
        destroy();
        return false;
    }

    if (!build(error)) {
        destroy();
        return false;
    }
    return true;
}

bool PwSwapchain::recreate(std::string& error)
{
    if (m_Surface == VK_NULL_HANDLE) {
        error = "no surface";
        return false;
    }
    return build(error);
}

void PwSwapchain::destroyImages()
{
    VkDevice device = m_Vulkan->device();
    for (auto view : m_Views) {
        vkDestroyImageView(device, view, nullptr);
    }
    for (auto semaphore : m_RenderDone) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    m_Views.clear();
    m_RenderDone.clear();
    m_Images.clear();
}

void PwSwapchain::destroy()
{
    if (m_Vulkan != nullptr) {
        destroyImages();
        if (m_Swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_Vulkan->device(), m_Swapchain, nullptr);
        }
        if (m_Surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_Vulkan->instance(), m_Surface, nullptr);
        }
    }
    m_Swapchain = VK_NULL_HANDLE;
    m_Surface = VK_NULL_HANDLE;
    if (m_Window != nullptr) {
        ANativeWindow_release(m_Window);
        m_Window = nullptr;
    }
}

// Describes the host's mastering display to the screen, so it can tone map to its own peak
void PwSwapchain::applyHdrMetadata()
{
    if (!m_Hdr || !m_Vulkan->hasHdrMetadata() || vkSetHdrMetadataEXT == nullptr) {
        return;
    }

    VkHdrMetadataEXT info = { VK_STRUCTURE_TYPE_HDR_METADATA_EXT };
    SS_HDR_METADATA host = {};
    if (LiGetHdrMetadata(&host)) {
        auto chromaticity = [](uint16_t value) { return float(value) / 50000.0f; };
        info.displayPrimaryRed = { chromaticity(host.displayPrimaries[0].x), chromaticity(host.displayPrimaries[0].y) };
        info.displayPrimaryGreen = { chromaticity(host.displayPrimaries[1].x), chromaticity(host.displayPrimaries[1].y) };
        info.displayPrimaryBlue = { chromaticity(host.displayPrimaries[2].x), chromaticity(host.displayPrimaries[2].y) };
        info.whitePoint = { chromaticity(host.whitePoint.x), chromaticity(host.whitePoint.y) };
        info.maxLuminance = float(host.maxDisplayLuminance);
        info.minLuminance = float(host.minDisplayLuminance) / 10000.0f;
        info.maxContentLightLevel = float(host.maxContentLightLevel);
        info.maxFrameAverageLightLevel = float(host.maxFrameAverageLightLevel);
    }
    else {
        // The host sent none: a BT.2020 / D65 mastering display with a 1000 nit peak
        info.displayPrimaryRed = { 0.708f, 0.292f };
        info.displayPrimaryGreen = { 0.170f, 0.797f };
        info.displayPrimaryBlue = { 0.131f, 0.046f };
        info.whitePoint = { 0.3127f, 0.3290f };
        info.maxLuminance = 1000.0f;
        info.minLuminance = 0.001f;
        info.maxContentLightLevel = 1000.0f;
        info.maxFrameAverageLightLevel = 400.0f;
    }
    vkSetHdrMetadataEXT(m_Vulkan->device(), 1, &m_Swapchain, &info);
}

bool PwSwapchain::build(std::string& error)
{
    VkPhysicalDevice physicalDevice = m_Vulkan->physicalDevice();
    VkDevice device = m_Vulkan->device();

    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, m_Surface, &caps) != VK_SUCCESS) {
        error = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
        return false;
    }

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_Surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_Surface, &count, formats.data());

    // HDR10: the shader's output is already PQ-encoded BT.2020, which is what the
    // HDR10_ST2084 colour space expects, so the same shader serves both modes
    VkSurfaceFormatKHR chosen = {};
    bool hdr = false;
    if (m_WantHdr) {
        if (m_Vulkan->hasHdrColorSpace()) {
            for (VkFormat wanted : { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32 }) {
                for (const auto& format : formats) {
                    if (format.format == wanted && format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
                        chosen = format;
                        hdr = true;
                        break;
                    }
                }
                if (hdr) {
                    break;
                }
            }
        }
        if (!hdr) {
            __android_log_print(ANDROID_LOG_WARN, "PyroWave",
                                "HDR was requested but the surface offers no HDR10 swapchain format (%s)",
                                m_Vulkan->hasHdrColorSpace() ? "no matching format" : "VK_EXT_swapchain_colorspace is missing");
        }
    }

    // The shader writes gamma-encoded values, so use a UNORM (not sRGB) format
    for (VkFormat wanted : { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM }) {
        if (hdr) {
            break;
        }
        for (const auto& format : formats) {
            if (format.format == wanted && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = format;
                break;
            }
        }
        if (chosen.format != VK_FORMAT_UNDEFINED) {
            break;
        }
    }
    if (chosen.format == VK_FORMAT_UNDEFINED) {
        error = "no 8-bit UNORM surface format";
        return false;
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_Surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_Surface, &count, modes.data());
    // Mailbox never blocks the render thread and always shows the newest frame
    m_PresentMode = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end() ?
                    VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;

    // Android reports the buffer size in the display's native orientation
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = uint32_t(ANativeWindow_getWidth(m_Window));
        extent.height = uint32_t(ANativeWindow_getHeight(m_Window));
    }
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (extent.width == 0 || extent.height == 0) {
        error = "the window has no area";
        return false;
    }

    // Render pre-rotated so the compositor does not have to rotate each frame
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
    if (!(caps.supportedTransforms & transform)) {
        transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (VkCompositeAlphaFlagBitsKHR wanted : { VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR }) {
        if (caps.supportedCompositeAlpha & wanted) {
            alpha = wanted;
            break;
        }
    }

    uint32_t imageCount = m_PresentMode == VK_PRESENT_MODE_MAILBOX_KHR ?
                          std::max(caps.minImageCount, 4u) : caps.minImageCount;
    if (caps.maxImageCount != 0) {
        imageCount = std::min(imageCount, caps.maxImageCount);
    }

    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = m_Surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = transform;
    info.compositeAlpha = alpha;
    info.presentMode = m_PresentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = m_Swapchain;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const VkResult result = vkCreateSwapchainKHR(device, &info, nullptr, &swapchain);

    // The old swapchain is retired either way
    destroyImages();
    if (m_Swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
    }
    if (result != VK_SUCCESS) {
        error = "vkCreateSwapchainKHR failed";
        return false;
    }
    m_Swapchain = swapchain;
    m_Format = chosen.format;
    m_Hdr = hdr;
    m_Extent = extent;
    m_Rotation = pwRotationFromTransform(transform);
    applyHdrMetadata();

    vkGetSwapchainImagesKHR(device, m_Swapchain, &count, nullptr);
    m_Images.resize(count);
    vkGetSwapchainImagesKHR(device, m_Swapchain, &count, m_Images.data());

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (auto image : m_Images) {
        VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_Format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS ||
                vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, view, nullptr);
            }
            destroyImages();
            vkDestroySwapchainKHR(device, m_Swapchain, nullptr);
            m_Swapchain = VK_NULL_HANDLE;
            error = "creating swapchain image views failed";
            return false;
        }
        m_Views.push_back(view);
        m_RenderDone.push_back(semaphore);
    }
    return true;
}
