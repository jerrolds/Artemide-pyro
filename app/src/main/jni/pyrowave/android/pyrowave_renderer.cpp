#include "pyrowave_renderer.h"

#include "pw_decoder.h"
#include "pw_presenter.h"
#include "pw_swapchain.h"
#include "pw_vulkan.h"

#include <android/log.h>
#include <android/native_window.h>

#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace {

void pwLog(int priority, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    __android_log_vprint(priority, "PyroWave", format, args);
    va_end(args);
}

uint64_t nowUs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Android's performance hint API. Decoding runs on the GPU, and the system's GPU
// governor can leave the clock low under a high load: at 120 fps the stream slid
// to 75 fps with the GPU at 220 MHz while it was 85% busy. Telling the system our
// frame deadline, and how long each frame really took, lets it raise the clocks.
// The entry points are resolved at run time: the NDK this builds against predates
// the newest ones, and older devices lack them all.
struct APerformanceHintManager;
struct APerformanceHintSession;
struct AWorkDuration;
struct ASessionCreationConfig;

class PerfHint {
public:
    // Must be called on the thread whose frames are reported
    void start(int64_t targetNs)
    {
        if (m_Session != nullptr) {
            return;
        }
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (lib == nullptr) {
            return;
        }

        auto getManager = fn<APerformanceHintManager* (*)()>(lib, "APerformanceHint_getManager");
        APerformanceHintManager* manager = getManager != nullptr ? getManager() : nullptr;
        if (manager == nullptr) {
            pwLog(ANDROID_LOG_INFO, "Performance hints: not available on this device");
            return;
        }

        m_Close = fn<void (*)(APerformanceHintSession*)>(lib, "APerformanceHint_closeSession");
        m_Report = fn<int (*)(APerformanceHintSession*, int64_t)>(lib, "APerformanceHint_reportActualWorkDuration");
        m_Report2 = fn<int (*)(APerformanceHintSession*, AWorkDuration*)>(lib, "APerformanceHint_reportActualWorkDuration2");
        m_DurCreate = fn<AWorkDuration* (*)()>(lib, "AWorkDuration_create");
        m_DurRelease = fn<void (*)(AWorkDuration*)>(lib, "AWorkDuration_release");
        m_DurStart = fn<void (*)(AWorkDuration*, int64_t)>(lib, "AWorkDuration_setWorkPeriodStartTimestampNanos");
        m_DurTotal = fn<void (*)(AWorkDuration*, int64_t)>(lib, "AWorkDuration_setActualTotalDurationNanos");
        m_DurCpu = fn<void (*)(AWorkDuration*, int64_t)>(lib, "AWorkDuration_setActualCpuDurationNanos");
        m_DurGpu = fn<void (*)(AWorkDuration*, int64_t)>(lib, "AWorkDuration_setActualGpuDurationNanos");
        m_Notify = fn<int (*)(APerformanceHintSession*, bool, bool, const char*)>(lib, "APerformanceHint_notifyWorkloadIncrease");

        const pid_t tid = pid_t(syscall(SYS_gettid));

        // Android 16: a graphics pipeline session is what the system uses to drive the GPU clock
        auto cfgCreate = fn<ASessionCreationConfig* (*)()>(lib, "ASessionCreationConfig_create");
        auto cfgRelease = fn<void (*)(ASessionCreationConfig*)>(lib, "ASessionCreationConfig_release");
        auto cfgTids = fn<void (*)(ASessionCreationConfig*, const pid_t*, size_t)>(lib, "ASessionCreationConfig_setTids");
        auto cfgTarget = fn<void (*)(ASessionCreationConfig*, int64_t)>(lib, "ASessionCreationConfig_setTargetWorkDurationNanos");
        auto cfgGraphics = fn<void (*)(ASessionCreationConfig*, bool)>(lib, "ASessionCreationConfig_setGraphicsPipeline");
        auto createUsingConfig = fn<int (*)(APerformanceHintManager*, ASessionCreationConfig*, APerformanceHintSession**)>(
            lib, "APerformanceHint_createSessionUsingConfig");

        const char* kind = "plain";
        if (cfgCreate && cfgRelease && cfgTids && cfgTarget && cfgGraphics && createUsingConfig) {
            ASessionCreationConfig* config = cfgCreate();
            cfgTids(config, &tid, 1);
            cfgTarget(config, targetNs);
            cfgGraphics(config, true);
            APerformanceHintSession* session = nullptr;
            if (createUsingConfig(manager, config, &session) == 0 && session != nullptr) {
                m_Session = session;
                kind = "graphics pipeline";
                m_Kind = 2;
            }
            cfgRelease(config);
        }
        if (m_Session == nullptr) {
            auto create = fn<APerformanceHintSession* (*)(APerformanceHintManager*, const int32_t*, size_t, int64_t)>(
                lib, "APerformanceHint_createSession");
            const int32_t id = tid;
            if (create != nullptr) {
                m_Session = create(manager, &id, 1, targetNs);
            }
        }
        if (m_Session == nullptr) {
            pwLog(ANDROID_LOG_INFO, "Performance hints: could not create a session");
            return;
        }
        if (m_Kind == 0) {
            m_Kind = 1;
        }

        m_TargetNs = targetNs;
        if (m_Notify != nullptr) {
            m_Notify(m_Session, true, true, "pyrowave");
        }
        pwLog(ANDROID_LOG_INFO, "Performance hints: %s session, target %.2f ms, CPU/GPU timing %s",
              kind, double(targetNs) / 1e6, m_Report2 != nullptr ? "reported" : "total only");
    }

    // Times are nanoseconds on CLOCK_MONOTONIC (std::chrono::steady_clock on Android)
    void report(int64_t startNs, int64_t totalNs, int64_t cpuNs, int64_t gpuNs)
    {
        if (m_Session == nullptr || totalNs <= 0) {
            return;
        }
        if (m_Report2 && m_DurCreate && m_DurStart && m_DurTotal && m_DurCpu && m_DurGpu) {
            if (m_Duration == nullptr) {
                m_Duration = m_DurCreate();
            }
            m_DurStart(m_Duration, startNs);
            m_DurTotal(m_Duration, totalNs);
            m_DurCpu(m_Duration, cpuNs);
            m_DurGpu(m_Duration, gpuNs);
            m_Report2(m_Session, m_Duration);
        }
        else if (m_Report) {
            m_Report(m_Session, totalNs);
        }

        // The governor can settle one step short of what the load needs. While frames keep
        // missing the deadline, say so again, at most twice a second.
        if (totalNs > m_TargetNs) {
            if (++m_Over >= 10 && m_Notify != nullptr && startNs - m_LastNotifyNs >= 500000000LL) {
                m_Notify(m_Session, true, true, "pyrowave");
                m_LastNotifyNs = startNs;
            }
        }
        else {
            m_Over = 0;
        }
    }

    void stop()
    {
        if (m_Duration != nullptr && m_DurRelease != nullptr) {
            m_DurRelease(m_Duration);
        }
        m_Duration = nullptr;
        if (m_Session != nullptr && m_Close != nullptr) {
            m_Close(m_Session);
        }
        m_Session = nullptr;
        m_Kind = 0;
    }

    // 0: no session, 1: plain session, 2: graphics pipeline session
    int kind() const { return m_Kind; }

private:
    template <typename F>
    static F fn(void* lib, const char* name)
    {
        return reinterpret_cast<F>(dlsym(lib, name));
    }

    APerformanceHintSession* m_Session = nullptr;
    AWorkDuration* m_Duration = nullptr;
    int64_t m_TargetNs = 0;
    int64_t m_LastNotifyNs = 0;
    int m_Over = 0;
    int m_Kind = 0;
    void (*m_Close)(APerformanceHintSession*) = nullptr;
    int (*m_Report)(APerformanceHintSession*, int64_t) = nullptr;
    int (*m_Report2)(APerformanceHintSession*, AWorkDuration*) = nullptr;
    int (*m_Notify)(APerformanceHintSession*, bool, bool, const char*) = nullptr;
    AWorkDuration* (*m_DurCreate)() = nullptr;
    void (*m_DurRelease)(AWorkDuration*) = nullptr;
    void (*m_DurStart)(AWorkDuration*, int64_t) = nullptr;
    void (*m_DurTotal)(AWorkDuration*, int64_t) = nullptr;
    void (*m_DurCpu)(AWorkDuration*, int64_t) = nullptr;
    void (*m_DurGpu)(AWorkDuration*, int64_t) = nullptr;
};

// Times the GPU side of a frame. The decode is submitted by PyroWave itself and cannot carry
// timestamps, so tiny command buffers on the same queue write one before the decode, one
// after it and one after the colour conversion; queue order makes the differences the GPU time
// of each stage. Only one frame in kSampleEvery is timed, so the extra submissions cost almost
// nothing, and a slot's results are read only once all three timestamps are available.
class GpuTimer {
public:
    static constexpr int kSampleEvery = 8;
    static constexpr int kStamps = 3; // before decode, after decode, after colour conversion
    static constexpr int kMaxSlots = 4;

    bool init(PwVulkan& vulkan, VkCommandPool pool, int slots)
    {
        m_Vulkan = &vulkan;
        if (!vulkan.canTimestamp() || slots <= 0 || slots > kMaxSlots) {
            return false;
        }
        VkDevice device = vulkan.device();
        const uint32_t count = uint32_t(slots * kStamps);

        VkQueryPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        poolInfo.queryCount = count;
        if (vkCreateQueryPool(device, &poolInfo, nullptr, &m_Pool) != VK_SUCCESS) {
            m_Pool = VK_NULL_HANDLE;
            return false;
        }
        vkResetQueryPool(device, m_Pool, 0, count);

        VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = count;
        if (vkAllocateCommandBuffers(device, &allocInfo, m_Commands) != VK_SUCCESS) {
            destroy();
            return false;
        }
        // Each command buffer only writes its own timestamp, so it is recorded once and reused
        for (uint32_t i = 0; i < count; i++) {
            VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            vkBeginCommandBuffer(m_Commands[i], &beginInfo);
            vkCmdWriteTimestamp2(m_Commands[i], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_Pool, i);
            vkEndCommandBuffer(m_Commands[i]);
        }

        const uint32_t bits = vulkan.timestampValidBits();
        m_Mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
        m_PeriodNs = double(vulkan.timestampPeriodNs());
        m_Enabled = true;
        return true;
    }

    void destroy()
    {
        m_Enabled = false;
        if (m_Pool != VK_NULL_HANDLE && m_Vulkan != nullptr) {
            vkDestroyQueryPool(m_Vulkan->device(), m_Pool, nullptr);
        }
        m_Pool = VK_NULL_HANDLE;
    }

    bool enabled() const { return m_Enabled; }

    // Starts timing the frame that uses this slot, if it is one of the sampled frames
    bool begin(int slot)
    {
        if (!m_Enabled || ++m_Counter % kSampleEvery != 0) {
            return false;
        }
        Slot& s = m_Slots[slot];
        if (s.pending && !harvest(slot)) {
            return false; // the GPU has not finished the previous sample of this slot
        }
        s.pending = true;
        s.valid = true;
        stamp(slot, 0);
        return true;
    }

    // ok is false if the stage failed: the remaining timestamps are still written so
    // that the slot can be reused, but the sample is discarded
    void decoded(int slot, bool ok)
    {
        m_Slots[slot].valid = m_Slots[slot].valid && ok;
        stamp(slot, 1);
    }

    void presented(int slot, bool ok)
    {
        m_Slots[slot].valid = m_Slots[slot].valid && ok;
        stamp(slot, 2);
    }

    uint64_t decodeUs() const { return m_DecodeUs; }
    uint64_t convertUs() const { return m_ConvertUs; }
    uint64_t samples() const { return m_Samples; }

private:
    struct Slot {
        bool pending = false;
        bool valid = false;
    };

    void stamp(int slot, int which)
    {
        VkCommandBufferSubmitInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = m_Commands[slot * kStamps + which];
        VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &cmdInfo;
        std::lock_guard<std::mutex> guard(m_Vulkan->queueLock());
        vkQueueSubmit2(m_Vulkan->queue(), 1, &submitInfo, VK_NULL_HANDLE);
    }

    // Reads and clears a slot's timestamps; false if they are not all available yet
    bool harvest(int slot)
    {
        VkDevice device = m_Vulkan->device();
        uint64_t data[kStamps * 2] = {}; // value, availability for each timestamp
        const VkResult result = vkGetQueryPoolResults(device, m_Pool, uint32_t(slot * kStamps), kStamps,
                                                      sizeof(data), data, 2 * sizeof(uint64_t),
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS) {
            return false;
        }
        for (int i = 0; i < kStamps; i++) {
            if (data[i * 2 + 1] == 0) {
                return false;
            }
        }
        if (m_Slots[slot].valid) {
            const double toUs = m_PeriodNs / 1000.0;
            m_DecodeUs += uint64_t(double((data[2] - data[0]) & m_Mask) * toUs);
            m_ConvertUs += uint64_t(double((data[4] - data[2]) & m_Mask) * toUs);
            m_Samples++;
        }
        vkResetQueryPool(device, m_Pool, uint32_t(slot * kStamps), kStamps);
        m_Slots[slot].pending = false;
        return true;
    }

    PwVulkan* m_Vulkan = nullptr;
    VkQueryPool m_Pool = VK_NULL_HANDLE;
    VkCommandBuffer m_Commands[kMaxSlots * kStamps] = {};
    Slot m_Slots[kMaxSlots];
    uint64_t m_Mask = 0;
    double m_PeriodNs = 0.0;
    uint32_t m_Counter = 0;
    bool m_Enabled = false;
    std::atomic<uint64_t> m_DecodeUs { 0 }, m_ConvertUs { 0 }, m_Samples { 0 };
};

// Enough for the decode of one frame to overlap the render of the previous
// one while a third waits for the display. A deeper queue keeps the GPU busy
// across the CPU's per-frame work, so mobile clock governors see a full load.
constexpr int k_SurfaceCount = 4;
constexpr int k_FramesInFlight = 3;

struct Frame {
    std::vector<uint8_t> data;
    std::vector<PyroWaveFraming::Segment> segments;
    size_t criticalPackets = 0;
    int colorspace = COLORSPACE_REC_709;
    int frameNumber = 0;
    bool hdr = false; // HDR10: PQ-encoded, normally BT.2020
};

class Renderer {
public:
    ~Renderer();

    bool setup(int videoFormat, int width, int height, bool fullRange, std::string& error);
    void setWindow(ANativeWindow* window);
    void start();
    void stop();
    int submit(PDECODE_UNIT du);
    void stats(PW_RENDERER_STATS* out) const;

private:
    void renderLoop();
    void process(const Frame& frame);
    bool present(int surface, uint64_t decodeValue, int colorspace);
    void waitIdle();
    void publishOutput();

    PwVulkan m_Vulkan;
    PwDecoder m_Decoder;
    PwPresenter m_Presenter;
    PwSwapchain m_Swapchain;
    bool m_FullRange = false;

    VkCommandPool m_CommandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_Commands[k_FramesInFlight] = {};
    VkSemaphore m_AcquireSemaphores[k_FramesInFlight] = {};
    uint64_t m_FrameValues[k_FramesInFlight] = {};
    uint32_t m_FrameIndex = 0;
    VkSemaphore m_RenderTimeline = VK_NULL_HANDLE;
    uint64_t m_RenderValue = 0;
    // Render timeline value after which each surface may be overwritten
    uint64_t m_SurfaceReleased[k_SurfaceCount] = {};
    int m_NextSurface = 0;
    bool m_NeedRecreate = false;

    // Hand-over from moonlight-common-c's decoder thread to the render thread
    std::mutex m_FrameLock;
    std::condition_variable m_FrameReady;
    Frame m_Staging; // only touched by the submitting thread
    Frame m_Pending;
    Frame m_Working; // only touched by the render thread
    bool m_PendingValid = false;
    bool m_Stopping = false;
    std::thread m_Thread;
    PerfHint m_PerfHint; // only touched by the render thread

    // Held while the render thread uses the swapchain, and while it is replaced
    std::mutex m_RenderLock;

    std::atomic<uint32_t> m_Received { 0 }, m_Replaced { 0 }, m_Rejected { 0 }, m_Partial { 0 };
    std::atomic<uint32_t> m_Decoded { 0 }, m_Presented { 0 }, m_NoWindow { 0 };
    std::atomic<uint64_t> m_DecodeUs { 0 }, m_PresentUs { 0 };
    std::atomic<uint32_t> m_OutputWidth { 0 }, m_OutputHeight { 0 };
    std::atomic<bool> m_Mailbox { false };
    std::atomic<bool> m_HdrOutput { false };
    uint64_t m_LastErrorLogUs = 0;

    // Detail for the overlay. Counted on the submitting thread, read by the stats thread.
    std::atomic<uint64_t> m_ReceivedBytes { 0 };
    std::atomic<uint64_t> m_PacketsTotal { 0 }, m_PacketsLost { 0 };
    std::atomic<uint64_t> m_ArrivalCount { 0 }, m_ArrivalSumUs { 0 }, m_ArrivalSqSumUs { 0 };
    uint64_t m_LastArrivalUs = 0; // submitting thread only
    // Peaks since the last stats read
    mutable std::atomic<uint32_t> m_MaxArrivalGapUs { 0 }, m_MaxFrameUs { 0 };
    std::atomic<int> m_Colorspace { COLORSPACE_REC_709 };
    std::atomic<uint32_t> m_SwapchainFormat { 0 }, m_SwapchainImages { 0 };
    std::atomic<int> m_HintKind { 0 };
    GpuTimer m_GpuTimer; // only touched by the render thread, apart from its totals
};

Renderer::~Renderer()
{
    stop();
    if (m_Vulkan.device() == VK_NULL_HANDLE) {
        return;
    }
    waitIdle();
    m_Swapchain.destroy();
    m_GpuTimer.destroy();
    VkDevice device = m_Vulkan.device();
    for (int i = 0; i < k_FramesInFlight; i++) {
        if (m_AcquireSemaphores[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, m_AcquireSemaphores[i], nullptr);
        }
    }
    if (m_RenderTimeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(device, m_RenderTimeline, nullptr);
    }
    if (m_CommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, m_CommandPool, nullptr);
    }
    // m_Presenter, m_Decoder and then m_Vulkan are destroyed in reverse order
}

void Renderer::publishOutput()
{
    m_OutputWidth = m_Swapchain.valid() ? m_Swapchain.extent().width : 0;
    m_OutputHeight = m_Swapchain.valid() ? m_Swapchain.extent().height : 0;
    m_Mailbox = m_Swapchain.valid() && m_Swapchain.presentMode() == VK_PRESENT_MODE_MAILBOX_KHR;
    m_HdrOutput = m_Swapchain.valid() && m_Swapchain.hdr();
    m_SwapchainFormat = m_Swapchain.valid() ? uint32_t(m_Swapchain.format()) : 0;
    m_SwapchainImages = m_Swapchain.valid() ? m_Swapchain.imageCount() : 0;
}

void Renderer::waitIdle()
{
    std::lock_guard<std::mutex> guard(m_Vulkan.queueLock());
    vkQueueWaitIdle(m_Vulkan.queue());
}

bool Renderer::setup(int videoFormat, int width, int height, bool fullRange, std::string& error)
{
    if (!m_Vulkan.create(true, error)) {
        return false;
    }

    PwDecoder::Config config;
    config.width = width;
    config.height = height;
    config.chroma444 = (videoFormat & VIDEO_FORMAT_MASK_YUV444) != 0;
    config.tenBit = (videoFormat & VIDEO_FORMAT_MASK_10BIT) != 0;
    if (!m_Decoder.initialize(m_Vulkan, config, k_SurfaceCount)) {
        error = m_Decoder.lastError();
        return false;
    }
    if (!m_Presenter.initialize(m_Vulkan, m_Decoder, error)) {
        return false;
    }
    m_FullRange = fullRange;

    VkDevice device = m_Vulkan.device();
    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_Vulkan.queueFamily();
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &m_CommandPool) != VK_SUCCESS) {
        m_CommandPool = VK_NULL_HANDLE;
        error = "vkCreateCommandPool failed";
        return false;
    }
    VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = m_CommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = k_FramesInFlight;
    if (vkAllocateCommandBuffers(device, &allocInfo, m_Commands) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return false;
    }
    // Optional: without it the overlay simply has no GPU timings
    if (!m_GpuTimer.init(m_Vulkan, m_CommandPool, k_FramesInFlight)) {
        pwLog(ANDROID_LOG_INFO, "GPU timing is not available on this device");
    }

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (auto& semaphore : m_AcquireSemaphores) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
            semaphore = VK_NULL_HANDLE;
            error = "vkCreateSemaphore failed";
            return false;
        }
    }
    VkSemaphoreTypeCreateInfo timelineInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semaphoreInfo.pNext = &timelineInfo;
    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_RenderTimeline) != VK_SUCCESS) {
        m_RenderTimeline = VK_NULL_HANDLE;
        error = "vkCreateSemaphore failed";
        return false;
    }

    pwLog(ANDROID_LOG_INFO, "Ready on %s: %dx%d %s %d-bit, %s decode path, bitstream %s",
          m_Vulkan.deviceName(), width, height, config.chroma444 ? "4:4:4" : "4:2:0",
          config.tenBit ? 10 : 8, m_Decoder.fragmentPath() ? "fragment" : "compute", PYROWAVE_BITSTREAM_ID);
    return true;
}

void Renderer::setWindow(ANativeWindow* window)
{
    std::lock_guard<std::mutex> guard(m_RenderLock);
    // The swapchain's images may still be in use by queued work
    waitIdle();
    m_Swapchain.destroy();
    m_NeedRecreate = false;
    publishOutput();
    if (window == nullptr) {
        return;
    }

    std::string error;
    if (!m_Swapchain.create(m_Vulkan, window, error) ||
            !m_Presenter.setTargetFormat(m_Swapchain.format(), error)) {
        pwLog(ANDROID_LOG_ERROR, "Cannot present to the window: %s", error.c_str());
        m_Swapchain.destroy();
        return;
    }
    publishOutput();
    pwLog(ANDROID_LOG_INFO, "Presenting %ux%u, %s, rotation %d%s",
          m_Swapchain.extent().width, m_Swapchain.extent().height,
          m_Swapchain.presentMode() == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "FIFO",
          m_Swapchain.rotation() * 90, m_Swapchain.hdr() ? ", HDR10" : "");
}

void Renderer::start()
{
    std::lock_guard<std::mutex> guard(m_FrameLock);
    if (m_Thread.joinable()) {
        return;
    }
    m_Stopping = false;
    m_Thread = std::thread(&Renderer::renderLoop, this);
}

void Renderer::stop()
{
    {
        std::lock_guard<std::mutex> guard(m_FrameLock);
        m_Stopping = true;
        m_PendingValid = false;
    }
    m_FrameReady.notify_all();
    if (m_Thread.joinable()) {
        m_Thread.join();
    }
}

int Renderer::submit(PDECODE_UNIT du)
{
    m_Received++;

    // Time between frames reaching us: how evenly the network and the host deliver them
    const uint64_t arrival = nowUs();
    if (m_LastArrivalUs != 0) {
        const uint64_t gap = std::min<uint64_t>(arrival - m_LastArrivalUs, 100000);
        m_ArrivalCount++;
        m_ArrivalSumUs += gap;
        m_ArrivalSqSumUs += gap * gap;
        uint32_t peak = m_MaxArrivalGapUs.load(std::memory_order_relaxed);
        while (gap > peak && !m_MaxArrivalGapUs.compare_exchange_weak(peak, uint32_t(gap))) {
        }
    }
    m_LastArrivalUs = arrival;

    // Copy the frame out of moonlight-common-c's buffers, one segment per RTP
    // packet so the parser knows what was lost and where records start
    Frame& frame = m_Staging;
    frame.data.resize(size_t(du->fullLength));
    frame.segments.clear();
    size_t offset = 0;
    uint32_t packets = 0, lostPackets = 0;
    for (PLENTRY entry = du->bufferList; entry != nullptr; entry = entry->next) {
        packets++;
        if (entry->bufferType == BUFFER_TYPE_LOST) {
            lostPackets++;
        }
        const size_t length = size_t(entry->length);
        if (offset + length > frame.data.size()) {
            frame.data.resize(offset + length);
        }
        std::memcpy(frame.data.data() + offset, entry->data, length);
        frame.segments.push_back({ offset, length, entry->bufferType == BUFFER_TYPE_LOST,
                                   entry->bufferType == BUFFER_TYPE_RECORD_START });
        offset += length;
    }
    frame.data.resize(offset);
    m_ReceivedBytes += offset;
    m_PacketsTotal += packets;
    m_PacketsLost += lostPackets;
    m_Colorspace = du->colorspace;
    frame.criticalPackets = du->pyrowaveCriticalPackets;
    frame.colorspace = du->colorspace;
    frame.frameNumber = du->frameNumber;
    frame.hdr = du->hdrActive;

    {
        std::lock_guard<std::mutex> guard(m_FrameLock);
        if (m_PendingValid) {
            // Frames are independent: the newer one simply replaces it
            m_Replaced++;
        }
        std::swap(m_Staging, m_Pending);
        m_PendingValid = true;
    }
    m_FrameReady.notify_one();

    // Nothing to request from the host: every frame is a keyframe
    return DR_OK;
}

void Renderer::renderLoop()
{
    // The deadline is a little tighter than one frame at the tablet's 120 Hz (8.33 ms),
    // so a frame that only just fits still asks the system for a faster GPU clock
    m_PerfHint.start(7500000);
    m_HintKind = m_PerfHint.kind();
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_FrameLock);
            m_FrameReady.wait(lock, [this]() { return m_Stopping || m_PendingValid; });
            if (m_Stopping) {
                m_PerfHint.stop();
                return;
            }
            std::swap(m_Pending, m_Working);
            m_PendingValid = false;
        }

        std::lock_guard<std::mutex> guard(m_RenderLock);
        process(m_Working);
    }
}

void Renderer::process(const Frame& frame)
{
    if (!m_Swapchain.valid()) {
        m_NoWindow++;
        return;
    }

    // Follow the stream: HDR10 frames need an HDR swapchain and SDR frames an ordinary
    // one. The rebuild happens in present(), which waits for the GPU to go idle first.
    if (frame.hdr != m_Swapchain.wantsHdr()) {
        m_Swapchain.setHdr(frame.hdr);
        m_NeedRecreate = true;
    }

    // The slot of the frames in flight this frame will use, as present() picks it. One frame
    // in a few has its GPU stages timed: begin() writes the first timestamp.
    const int slot = int(m_FrameIndex % k_FramesInFlight);
    const bool timed = m_GpuTimer.begin(slot);

    const uint64_t start = nowUs();
    const int surface = m_NextSurface;
    uint64_t decodeValue = 0;
    const uint64_t released = m_SurfaceReleased[surface];
    if (!m_Decoder.decode(frame.data.data(), frame.data.size(), frame.segments, frame.criticalPackets, surface,
                          released != 0 ? m_RenderTimeline : VK_NULL_HANDLE, released, decodeValue)) {
        if (timed) {
            m_GpuTimer.decoded(slot, false);
            m_GpuTimer.presented(slot, false);
        }
        m_Rejected++;
        // The next frame replaces this one; log at most once a second
        const uint64_t now = nowUs();
        if (now - m_LastErrorLogUs >= 1000000) {
            m_LastErrorLogUs = now;
            pwLog(ANDROID_LOG_WARN, "Dropped frame %d: %s (%u dropped so far)",
                  frame.frameNumber, m_Decoder.lastError().c_str(), m_Rejected.load());
        }
        return;
    }
    m_NextSurface = (m_NextSurface + 1) % k_SurfaceCount;
    if (m_Decoder.lastFramePartial()) {
        m_Partial++;
    }
    m_Decoded++;
    if (timed) {
        m_GpuTimer.decoded(slot, true);
    }

    const uint64_t decoded = nowUs();
    m_DecodeUs += decoded - start;

    const bool presented = present(surface, decodeValue, frame.colorspace);
    if (presented) {
        m_Presented++;
    }
    if (timed) {
        m_GpuTimer.presented(slot, presented);
    }
    const uint64_t end = nowUs();
    m_PresentUs += end - decoded;

    // Slowest frame since the stats were last read
    const uint32_t frameUs = uint32_t(std::min<uint64_t>(end - start, 1000000));
    uint32_t peak = m_MaxFrameUs.load(std::memory_order_relaxed);
    while (frameUs > peak && !m_MaxFrameUs.compare_exchange_weak(peak, frameUs)) {
    }

    // Present mostly waits for the GPU to finish earlier frames, so it stands in for GPU time
    m_PerfHint.report(int64_t(start) * 1000, int64_t(end - start) * 1000,
                      int64_t(decoded - start) * 1000, int64_t(end - decoded) * 1000);
}

bool Renderer::present(int surface, uint64_t decodeValue, int colorspace)
{
    VkDevice device = m_Vulkan.device();
    std::string error;

    if (m_NeedRecreate) {
        waitIdle();
        if (!m_Swapchain.recreate(error) || !m_Presenter.setTargetFormat(m_Swapchain.format(), error)) {
            pwLog(ANDROID_LOG_ERROR, "Recreating the swapchain failed: %s", error.c_str());
            m_Swapchain.destroy();
            publishOutput();
            return false;
        }
        m_NeedRecreate = false;
        publishOutput();
        pwLog(ANDROID_LOG_INFO, "Swapchain rebuilt: %s (%s requested)",
              m_Swapchain.hdr() ? "HDR10" : "SDR", m_Swapchain.wantsHdr() ? "HDR" : "SDR");
    }

    // Reuse this slot's command buffer and acquire semaphore only once the
    // submission that last used them has finished
    const uint32_t slot = m_FrameIndex % k_FramesInFlight;
    if (m_FrameValues[slot] != 0) {
        VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &m_RenderTimeline;
        waitInfo.pValues = &m_FrameValues[slot];
        vkWaitSemaphores(device, &waitInfo, UINT64_MAX);
    }

    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(device, m_Swapchain.handle(), UINT64_MAX,
                                            m_AcquireSemaphores[slot], VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // The decoded surface was never read; it stays free as it was
        m_NeedRecreate = true;
        return false;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        pwLog(ANDROID_LOG_ERROR, "vkAcquireNextImageKHR failed: %d", int(result));
        return false;
    }

    VkCommandBuffer cmd = m_Commands[slot];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    PwColor color;
    color.colorspace = colorspace;
    color.fullRange = m_FullRange;
    m_Presenter.record(cmd, surface, color, m_Swapchain.image(imageIndex), m_Swapchain.view(imageIndex),
                       m_Swapchain.extent(), m_Swapchain.rotation(),
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, 0);
    vkEndCommandBuffer(cmd);

    VkSemaphoreSubmitInfo waits[2] = {};
    waits[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[0].semaphore = m_Decoder.decodeSemaphore();
    waits[0].value = decodeValue;
    waits[0].stageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    waits[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[1].semaphore = m_AcquireSemaphores[slot];
    waits[1].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    const uint64_t renderValue = m_RenderValue + 1;
    VkSemaphoreSubmitInfo signals[2] = {};
    signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[0].semaphore = m_RenderTimeline;
    signals[0].value = renderValue;
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[1].semaphore = m_Swapchain.renderDone(imageIndex);
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cmdInfo.commandBuffer = cmd;
    VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submitInfo.waitSemaphoreInfoCount = 2;
    submitInfo.pWaitSemaphoreInfos = waits;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdInfo;
    submitInfo.signalSemaphoreInfoCount = 2;
    submitInfo.pSignalSemaphoreInfos = signals;

    VkSemaphore renderDone = m_Swapchain.renderDone(imageIndex);
    VkSwapchainKHR swapchain = m_Swapchain.handle();
    VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderDone;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    VkResult presentResult;
    {
        std::lock_guard<std::mutex> guard(m_Vulkan.queueLock());
        result = vkQueueSubmit2(m_Vulkan.queue(), 1, &submitInfo, VK_NULL_HANDLE);
        if (result != VK_SUCCESS) {
            pwLog(ANDROID_LOG_ERROR, "vkQueueSubmit2 failed: %d", int(result));
            return false;
        }
        presentResult = vkQueuePresentKHR(m_Vulkan.queue(), &presentInfo);
    }

    m_RenderValue = renderValue;
    m_FrameValues[slot] = renderValue;
    m_SurfaceReleased[surface] = renderValue;
    m_FrameIndex++;

    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        m_NeedRecreate = true;
    }
    else if (presentResult != VK_SUCCESS) {
        pwLog(ANDROID_LOG_ERROR, "vkQueuePresentKHR failed: %d", int(presentResult));
    }
    return presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR;
}

void Renderer::stats(PW_RENDERER_STATS* out) const
{
    out->receivedFrames = m_Received;
    out->replacedFrames = m_Replaced;
    out->rejectedFrames = m_Rejected;
    out->partialFrames = m_Partial;
    out->decodedFrames = m_Decoded;
    out->presentedFrames = m_Presented;
    out->noWindowFrames = m_NoWindow;
    out->totalDecodeUs = m_DecodeUs;
    out->totalPresentUs = m_PresentUs;
    out->outputWidth = m_OutputWidth;
    out->outputHeight = m_OutputHeight;
    out->fragmentPath = m_Decoder.fragmentPath();
    out->mailbox = m_Mailbox;
    out->hdr = m_HdrOutput;

    out->receivedBytes = m_ReceivedBytes;
    out->packetsTotal = m_PacketsTotal;
    out->packetsLost = m_PacketsLost;
    out->arrivalCount = m_ArrivalCount;
    out->arrivalSumUs = m_ArrivalSumUs;
    out->arrivalSqSumUs = m_ArrivalSqSumUs;
    out->maxArrivalGapUs = m_MaxArrivalGapUs.exchange(0);
    out->maxFrameUs = m_MaxFrameUs.exchange(0);
    out->colorspace = uint32_t(m_Colorspace.load());
    out->swapchainFormat = m_SwapchainFormat;
    out->swapchainImages = m_SwapchainImages;
    out->hintKind = uint32_t(m_HintKind.load());
    out->framesInFlight = k_FramesInFlight;
    out->surfaceCount = k_SurfaceCount;
    out->gpuTimingEnabled = m_GpuTimer.enabled();
    out->gpuDecodeUs = m_GpuTimer.decodeUs();
    out->gpuConvertUs = m_GpuTimer.convertUs();
    out->gpuSamples = m_GpuTimer.samples();
}

// Serializes the API entry points; the renderer exists between setup and cleanup
std::mutex g_ApiLock;
std::unique_ptr<Renderer> g_Renderer;
// The window outlives renderers: Java may attach it before setup
ANativeWindow* g_Window = nullptr;

}

extern "C" bool PwIsAvailable(char* reason, size_t reasonSize)
{
    static std::mutex lock;
    static int cached = -1;
    static std::string cachedReason;

    std::lock_guard<std::mutex> guard(lock);
    if (cached < 0) {
        PwVulkan vulkan;
        PwDecoder decoder;
        PwDecoder::Config config;
        config.width = 256;
        config.height = 256;
        if (!vulkan.create(true, cachedReason)) {
            cached = 0;
        }
        else if (!decoder.initialize(vulkan, config, 1)) {
            cachedReason = decoder.lastError();
            cached = 0;
        }
        else {
            cachedReason = std::string("Vulkan device ") + vulkan.deviceName() +
                           (decoder.fragmentPath() ? " (fragment path)" : " (compute path)");
            cached = 1;
        }
        pwLog(cached ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "%s: %s",
              cached ? "Available" : "Unavailable", cachedReason.c_str());
    }
    if (reason != nullptr && reasonSize != 0) {
        std::snprintf(reason, reasonSize, "%s", cachedReason.c_str());
    }
    return cached == 1;
}

extern "C" int PwRendererSetup(int videoFormat, int width, int height, int frameRate, bool fullRange)
{
    std::lock_guard<std::mutex> guard(g_ApiLock);
    g_Renderer.reset();

    auto renderer = std::make_unique<Renderer>();
    std::string error;
    if (!renderer->setup(videoFormat, width, height, fullRange, error)) {
        pwLog(ANDROID_LOG_ERROR, "Setup for %dx%d@%d failed: %s", width, height, frameRate, error.c_str());
        return -1;
    }
    if (g_Window != nullptr) {
        renderer->setWindow(g_Window);
    }
    g_Renderer = std::move(renderer);
    return 0;
}

extern "C" void PwRendererSetWindow(ANativeWindow* window)
{
    std::lock_guard<std::mutex> guard(g_ApiLock);
    if (window != nullptr) {
        ANativeWindow_acquire(window);
    }
    if (g_Renderer) {
        g_Renderer->setWindow(window);
    }
    if (g_Window != nullptr) {
        ANativeWindow_release(g_Window);
    }
    g_Window = window;
}

extern "C" void PwRendererStart(void)
{
    std::lock_guard<std::mutex> guard(g_ApiLock);
    if (g_Renderer) {
        g_Renderer->start();
    }
}

extern "C" void PwRendererStop(void)
{
    std::lock_guard<std::mutex> guard(g_ApiLock);
    if (g_Renderer) {
        g_Renderer->stop();
    }
}

extern "C" void PwRendererCleanup(void)
{
    std::lock_guard<std::mutex> guard(g_ApiLock);
    if (g_Renderer) {
        PW_RENDERER_STATS stats;
        g_Renderer->stats(&stats);
        pwLog(ANDROID_LOG_INFO, "Session: %u received, %u replaced, %u rejected, %u partial, %u presented",
              stats.receivedFrames, stats.replacedFrames, stats.rejectedFrames, stats.partialFrames,
              stats.presentedFrames);
    }
    g_Renderer.reset();
}

extern "C" int PwRendererSubmitDecodeUnit(PDECODE_UNIT decodeUnit)
{
    // Called on moonlight-common-c's decoder thread between start and stop,
    // while setup and cleanup cannot run, so no lock is taken here
    Renderer* renderer = g_Renderer.get();
    return renderer != nullptr ? renderer->submit(decodeUnit) : DR_OK;
}

extern "C" void PwRendererGetStats(PW_RENDERER_STATS* stats)
{
    std::lock_guard<std::mutex> guard(g_ApiLock);
    if (g_Renderer) {
        g_Renderer->stats(stats);
    }
    else {
        *stats = PW_RENDERER_STATS();
    }
}
