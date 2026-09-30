# PyroWave decoding and presentation for Artemide (see android/pyrowave_renderer.h).
#
# pyrowave-codec: the vendored upstream codec and the Granite subset it needs
# (see VENDOR.txt; codec_sources.txt lists the same sources as moonlight-qt).
# pyrowave-android: the framing parser, decoder, Vulkan presenter and C API.
#
# PyroWave needs Vulkan 1.3, which only 64-bit devices ship, so 32-bit ABIs
# get a stub that reports PyroWave as unavailable.

LOCAL_PATH := $(call my-dir)
PYROWAVE_ROOT := $(LOCAL_PATH)
PYROWAVE_GRANITE := $(PYROWAVE_ROOT)/pyrowave/Granite
PYROWAVE_COMMON_C := $(PYROWAVE_ROOT)/../moonlight-core/moonlight-common-c/src

ifneq ($(filter arm64-v8a x86_64,$(TARGET_ARCH_ABI)),)

include $(CLEAR_VARS)
LOCAL_MODULE := pyrowave-codec
LOCAL_SRC_FILES := $(strip $(file <$(PYROWAVE_ROOT)/codec_sources.txt)) volk_platform.c
LOCAL_C_INCLUDES := \
    $(PYROWAVE_ROOT)/pyrowave \
    $(PYROWAVE_ROOT)/pyrowave/shaders \
    $(PYROWAVE_GRANITE)/video \
    $(PYROWAVE_GRANITE)/vulkan \
    $(PYROWAVE_GRANITE)/util \
    $(PYROWAVE_GRANITE)/math \
    $(PYROWAVE_GRANITE)/application/global \
    $(PYROWAVE_GRANITE)/third_party/volk \
    $(PYROWAVE_GRANITE)/third_party/renderdoc \
    $(PYROWAVE_GRANITE)/third_party/khronos/vulkan-headers/include
LOCAL_EXPORT_C_INCLUDES := \
    $(PYROWAVE_ROOT)/pyrowave \
    $(PYROWAVE_GRANITE)/third_party/volk \
    $(PYROWAVE_GRANITE)/third_party/khronos/vulkan-headers/include
# Upstream's standalone configuration: FP32 math with reduced-range storage.
# Symbols stay hidden so volk's global vk* pointers never interpose anything.
LOCAL_CFLAGS := -DGRANITE_SHIPPING -DGRANITE_RENDERDOC_CAPTURE -DPYROWAVE_PRECISION=1 \
                -fvisibility=hidden -w
LOCAL_CPPFLAGS := -std=c++17 -fvisibility-inlines-hidden
LOCAL_CPP_FEATURES := exceptions rtti
LOCAL_EXPORT_LDLIBS := -ldl -llog
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := pyrowave-android
LOCAL_SRC_FILES := \
    android/pyrowaveframing.cpp \
    android/pw_vulkan.cpp \
    android/pw_decoder.cpp \
    android/pw_presenter.cpp \
    android/pw_swapchain.cpp \
    android/pyrowave_renderer.cpp
LOCAL_C_INCLUDES := $(PYROWAVE_ROOT)/android $(PYROWAVE_COMMON_C)
LOCAL_EXPORT_C_INCLUDES := $(PYROWAVE_ROOT)/android
LOCAL_CFLAGS := -DVK_USE_PLATFORM_ANDROID_KHR -fvisibility=hidden -Wall
LOCAL_CPPFLAGS := -std=c++17
LOCAL_STATIC_LIBRARIES := pyrowave-codec
LOCAL_EXPORT_LDLIBS := -landroid -llog
include $(BUILD_STATIC_LIBRARY)

else

include $(CLEAR_VARS)
LOCAL_MODULE := pyrowave-android
LOCAL_SRC_FILES := android/pyrowave_renderer_stub.c
LOCAL_C_INCLUDES := $(PYROWAVE_ROOT)/android $(PYROWAVE_COMMON_C)
LOCAL_EXPORT_C_INCLUDES := $(PYROWAVE_ROOT)/android
include $(BUILD_STATIC_LIBRARY)

endif
