#include "pw_swapchain.h"
#include "pw_presenter.h"

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

    // The shader writes gamma-encoded values, so use a UNORM (not sRGB) format
    VkSurfaceFormatKHR chosen = {};
    for (VkFormat wanted : { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM }) {
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
    m_Extent = extent;
    m_Rotation = pwRotationFromTransform(transform);

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
