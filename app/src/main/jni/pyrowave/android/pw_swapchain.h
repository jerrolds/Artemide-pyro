#pragma once

// Vulkan swapchain on the stream view's ANativeWindow.

#include "pw_vulkan.h"

#include <string>
#include <vector>

struct ANativeWindow;

class PwSwapchain {
public:
    PwSwapchain() = default;
    ~PwSwapchain();

    PwSwapchain(const PwSwapchain&) = delete;
    PwSwapchain& operator=(const PwSwapchain&) = delete;

    // Takes its own reference to window. The device must be idle when a
    // swapchain is replaced or destroyed.
    bool create(PwVulkan& vulkan, ANativeWindow* window, std::string& error);
    // Rebuilds the swapchain for the same window (size, transform or HDR mode changed)
    bool recreate(std::string& error);
    void destroy();

    // Asks for an HDR10 (10-bit, PQ) swapchain at the next build. If the device or
    // surface cannot offer one the swapchain falls back to SDR: see hdr().
    void setHdr(bool wanted) { m_WantHdr = wanted; }
    bool wantsHdr() const { return m_WantHdr; }
    // Whether the current swapchain really is HDR10
    bool hdr() const { return m_Hdr; }

    bool valid() const { return m_Swapchain != VK_NULL_HANDLE; }
    VkSwapchainKHR handle() const { return m_Swapchain; }
    VkFormat format() const { return m_Format; }
    VkExtent2D extent() const { return m_Extent; }
    VkPresentModeKHR presentMode() const { return m_PresentMode; }
    // Surface transform in quarter turns, for PwPresenter::record()
    int rotation() const { return m_Rotation; }
    uint32_t imageCount() const { return uint32_t(m_Images.size()); }
    VkImage image(uint32_t index) const { return m_Images[index]; }
    VkImageView view(uint32_t index) const { return m_Views[index]; }
    // Signalled by the render submission, waited on by the present of that image
    VkSemaphore renderDone(uint32_t index) const { return m_RenderDone[index]; }

private:
    bool build(std::string& error);
    void destroyImages();
    void applyHdrMetadata();

    bool m_WantHdr = false;
    bool m_Hdr = false;
    PwVulkan* m_Vulkan = nullptr;
    ANativeWindow* m_Window = nullptr;
    VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
    VkFormat m_Format = VK_FORMAT_UNDEFINED;
    VkExtent2D m_Extent {};
    VkPresentModeKHR m_PresentMode = VK_PRESENT_MODE_FIFO_KHR;
    int m_Rotation = 0;
    std::vector<VkImage> m_Images;
    std::vector<VkImageView> m_Views;
    std::vector<VkSemaphore> m_RenderDone;
};
