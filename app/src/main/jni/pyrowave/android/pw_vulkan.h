#pragma once

// One Vulkan instance and device shared by the PyroWave decoder and the
// renderer that presents its output. PyroWave borrows the device through
// pyrowave_create_device(), which needs the create infos to stay valid for the
// device's lifetime, so this object owns them and must not move.

#include <volk.h>

#include <mutex>
#include <string>
#include <vector>

class PwVulkan {
public:
    PwVulkan() = default;
    ~PwVulkan();

    PwVulkan(const PwVulkan&) = delete;
    PwVulkan& operator=(const PwVulkan&) = delete;

    // withSurface enables the Android surface and swapchain extensions.
    bool create(bool withSurface, std::string& error);

    VkInstance instance() const { return m_Instance; }
    VkPhysicalDevice physicalDevice() const { return m_PhysicalDevice; }
    VkDevice device() const { return m_Device; }
    VkQueue queue() const { return m_Queue; }
    uint32_t queueFamily() const { return m_QueueFamily; }
    const char* deviceName() const { return m_Properties.properties.deviceName; }
    bool hasSwapchain() const { return m_HasSwapchain; }

    const VkInstanceCreateInfo* instanceCreateInfo() const { return &m_InstanceInfo; }
    const VkDeviceCreateInfo* deviceCreateInfo() const { return &m_DeviceInfo; }

    // PyroWave and the renderer submit to the same queue
    std::mutex& queueLock() { return m_QueueLock; }

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;

private:
    VkInstance m_Instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
    VkQueue m_Queue = VK_NULL_HANDLE;
    uint32_t m_QueueFamily = 0;
    bool m_HasSwapchain = false;

    VkApplicationInfo m_AppInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo m_InstanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    std::vector<const char*> m_InstanceExtensions;

    float m_QueuePriority = 1.0f;
    VkDeviceQueueCreateInfo m_QueueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo m_DeviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    std::vector<const char*> m_DeviceExtensions;
    VkPhysicalDeviceFeatures2 m_Features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan11Features m_Features11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features m_Features12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features m_Features13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };

    VkPhysicalDeviceProperties2 m_Properties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    VkPhysicalDeviceMemoryProperties m_MemoryProperties = {};

    std::mutex m_QueueLock;
};
