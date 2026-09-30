#include "pw_vulkan.h"

#include <cstring>

namespace {

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    for (const auto& ext : extensions) {
        if (std::strcmp(ext.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

}

PwVulkan::~PwVulkan()
{
    if (m_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_Device);
        vkDestroyDevice(m_Device, nullptr);
    }
    if (m_Instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_Instance, nullptr);
    }
}

bool PwVulkan::create(bool withSurface, std::string& error)
{
    if (volkInitialize() != VK_SUCCESS) {
        error = "no Vulkan loader";
        return false;
    }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        error = "Vulkan 1.3 instance support is required";
        return false;
    }

    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> instanceExtensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, instanceExtensions.data());

    if (withSurface) {
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        if (!hasExtension(instanceExtensions, VK_KHR_SURFACE_EXTENSION_NAME) ||
                !hasExtension(instanceExtensions, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
            error = "Vulkan surface extensions are unavailable";
            return false;
        }
        m_InstanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        m_InstanceExtensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#else
        error = "no surface platform in this build";
        return false;
#endif
    }

    m_AppInfo.pApplicationName = "Artemide PyroWave";
    m_AppInfo.pEngineName = "Granite";
    m_AppInfo.apiVersion = VK_API_VERSION_1_3;
    m_InstanceInfo.pApplicationInfo = &m_AppInfo;
    m_InstanceInfo.enabledExtensionCount = uint32_t(m_InstanceExtensions.size());
    m_InstanceInfo.ppEnabledExtensionNames = m_InstanceExtensions.data();

    if (vkCreateInstance(&m_InstanceInfo, nullptr, &m_Instance) != VK_SUCCESS) {
        m_Instance = VK_NULL_HANDLE;
        error = "vkCreateInstance failed";
        return false;
    }
    volkLoadInstance(m_Instance);

    vkEnumeratePhysicalDevices(m_Instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_Instance, &count, devices.data());

    // Mobile devices have one GPU; on desktop test machines prefer real hardware
    int bestScore = -1;
    for (auto candidate : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }

        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        for (uint32_t i = 0; i < count; i++) {
            const VkQueueFlags wanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((families[i].queueFlags & wanted) != wanted) {
                continue;
            }
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
            if (score > bestScore) {
                bestScore = score;
                m_PhysicalDevice = candidate;
                m_QueueFamily = i;
            }
            break;
        }
    }
    if (m_PhysicalDevice == VK_NULL_HANDLE) {
        error = "no Vulkan 1.3 device with a graphics queue";
        return false;
    }

    vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &m_Properties);
    vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &m_MemoryProperties);

    // Enable everything the device supports, as PyroWave's standalone device
    // does, except robustness features that only cost performance.
    m_Features.pNext = &m_Features11;
    m_Features11.pNext = &m_Features12;
    m_Features12.pNext = &m_Features13;
    m_Features13.pNext = nullptr;
    vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &m_Features);
    m_Features.features.robustBufferAccess = VK_FALSE;
    m_Features13.robustImageAccess = VK_FALSE;

    if (!m_Features12.timelineSemaphore || !m_Features13.synchronization2 ||
            !m_Features13.dynamicRendering || !m_Features13.subgroupSizeControl) {
        error = std::string(deviceName()) + " lacks timeline semaphores, synchronization2, "
                "dynamic rendering or subgroup size control";
        return false;
    }

    vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> deviceExtensions(count);
    vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &count, deviceExtensions.data());
    if (withSurface) {
        if (!hasExtension(deviceExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            error = "VK_KHR_swapchain is unavailable";
            return false;
        }
        m_DeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        m_HasSwapchain = true;
    }

    m_QueueInfo.queueFamilyIndex = m_QueueFamily;
    m_QueueInfo.queueCount = 1;
    m_QueueInfo.pQueuePriorities = &m_QueuePriority;

    m_DeviceInfo.pNext = &m_Features;
    m_DeviceInfo.queueCreateInfoCount = 1;
    m_DeviceInfo.pQueueCreateInfos = &m_QueueInfo;
    m_DeviceInfo.enabledExtensionCount = uint32_t(m_DeviceExtensions.size());
    m_DeviceInfo.ppEnabledExtensionNames = m_DeviceExtensions.data();

    if (vkCreateDevice(m_PhysicalDevice, &m_DeviceInfo, nullptr, &m_Device) != VK_SUCCESS) {
        m_Device = VK_NULL_HANDLE;
        error = "vkCreateDevice failed";
        return false;
    }
    volkLoadDevice(m_Device);
    vkGetDeviceQueue(m_Device, m_QueueFamily, 0, &m_Queue);
    return true;
}

uint32_t PwVulkan::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const
{
    for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (m_MemoryProperties.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}
