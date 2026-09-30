// Granite's volk, built with the Android surface entry points enabled for this
// translation unit only (the renderer creates its own VkSurfaceKHR).
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#endif

#include "pyrowave/Granite/third_party/volk/volk.c"
