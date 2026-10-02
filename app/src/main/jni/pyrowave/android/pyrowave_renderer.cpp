#include "pyrowave_renderer.h"

#include "pw_decoder.h"
#include "pw_presenter.h"
#include "pw_swapchain.h"
#include "pw_vulkan.h"

#include <android/log.h>
#include <android/native_window.h>

#include <dlfcn.h>
#include <sys/resource.h>
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
        m_Spike = fn<int (*)(APerformanceHintSession*, bool, bool, const char*)>(lib, "APerformanceHint_notifyWorkloadSpike");
        m_SetPower = fn<int (*)(APerformanceHintSession*, bool)>(lib, "APerformanceHint_setPreferPowerEfficiency");

        const pid_t tid = pid_t(syscall(SYS_gettid));

        // Android 16: a graphics pipeline session is what the system uses to drive the GPU clock
        auto cfgCreate = fn<ASessionCreationConfig* (*)()>(lib, "ASessionCreationConfig_create");
        auto cfgRelease = fn<void (*)(ASessionCreationConfig*)>(lib, "ASessionCreationConfig_release");
        auto cfgTids = fn<void (*)(ASessionCreationConfig*, const pid_t*, size_t)>(lib, "ASessionCreationConfig_setTids");
        auto cfgTarget = fn<void (*)(ASessionCreationConfig*, int64_t)>(lib, "ASessionCreationConfig_setTargetWorkDurationNanos");
        auto cfgGraphics = fn<void (*)(ASessionCreationConfig*, bool)>(lib, "ASessionCreationConfig_setGraphicsPipeline");
        auto cfgPower = fn<void (*)(ASessionCreationConfig*, bool)>(lib, "ASessionCreationConfig_setPreferPowerEfficiency");
        auto createUsingConfig = fn<int (*)(APerformanceHintManager*, ASessionCreationConfig*, APerformanceHintSession**)>(
            lib, "APerformanceHint_createSessionUsingConfig");

        const char* kind = "plain";
        if (cfgCreate && cfgRelease && cfgTids && cfgTarget && cfgGraphics && createUsingConfig) {
            ASessionCreationConfig* config = cfgCreate();
            cfgTids(config, &tid, 1);
            cfgTarget(config, targetNs);
            cfgGraphics(config, true);
            // Latency matters more than battery here: do not let the system pick efficiency
            if (cfgPower != nullptr) {
                cfgPower(config, false);
            }
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
        bool powerSet = false;
        if (m_SetPower != nullptr) {
            powerSet = m_SetPower(m_Session, false) == 0;
        }
        if (m_Notify != nullptr) {
            m_Notify(m_Session, true, true, "pyrowave");
        }
        if (m_Spike != nullptr) {
            m_Spike(m_Session, true, true, "pyrowave");
        }
        pwLog(ANDROID_LOG_INFO, "Performance hints: %s session, target %.2f ms, CPU/GPU timing %s, prefer performance %s, spike notice %s",
              kind, double(targetNs) / 1e6, m_Report2 != nullptr ? "reported" : "total only",
              powerSet ? "set" : "not available", m_Spike != nullptr ? "available" : "not available");
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
                if (m_Spike != nullptr) {
                    m_Spike(m_Session, true, true, "pyrowave");
                }
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
    int (*m_Spike)(APerformanceHintSession*, bool, bool, const char*) = nullptr;
    int (*m_SetPower)(APerformanceHintSession*, bool) = nullptr;
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
    // Smoothed GPU time of one frame's decode plus colour conversion, 0 until the first sample
    uint32_t frameEmaUs() const { return m_FrameEmaUs.load(std::memory_order_relaxed); }

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
            // Smoothed GPU time per frame, for the render loop's pacing
            const double frameUs = double((data[4] - data[0]) & m_Mask) * toUs;
            const uint32_t previous = m_FrameEmaUs.load(std::memory_order_relaxed);
            m_FrameEmaUs.store(previous == 0 ? uint32_t(frameUs) : uint32_t(double(previous) * 0.8 + frameUs * 0.2),
                               std::memory_order_relaxed);

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
    std::atomic<uint32_t> m_FrameEmaUs { 0 };
};

// Measures "frame assembled by moonlight-common-c" -> "GPU finished the frame" (the client latency
// that HEVC's overlay calls Delay). The earlier version placed GPU timestamps on CLOCK_MONOTONIC with
// calibrated timestamps and never produced a sample on the Adreno driver, so this one needs no GPU clock
// at all: a helper thread waits on the render timeline for a sampled frame and notes the time the wait
// returns, on the clock the library stamps frames with.
class LatencyProbe {
public:
    ~LatencyProbe() { stop(); }

    void start(VkDevice device, VkSemaphore timeline)
    {
        if (m_Running.load() || timeline == VK_NULL_HANDLE) {
            return;
        }
        m_Device = device;
        m_Timeline = timeline;
        m_Stop = false;
        m_Head = 0;
        m_Count = 0;
        m_Running = true;
        m_Thread = std::thread(&LatencyProbe::run, this);
    }

    void stop()
    {
        if (!m_Running.load()) {
            return;
        }
        {
            std::lock_guard<std::mutex> guard(m_Lock);
            m_Stop = true;
        }
        m_Wake.notify_all();
        if (m_Thread.joinable()) {
            m_Thread.join();
        }
        m_Running = false;
    }

    bool enabled() const { return m_Running.load(); }

    // Render thread: the frame whose render finishes at timeline value "value" was assembled at
    // enqueueUs (CLOCK_MONOTONIC, microseconds). Best effort: a full queue just skips the sample.
    void sample(uint64_t value, uint64_t enqueueUs)
    {
        std::lock_guard<std::mutex> guard(m_Lock);
        if (m_Count == kQueue) {
            return;
        }
        m_Ring[(m_Head + m_Count) % kQueue] = { value, enqueueUs };
        m_Count++;
        m_Wake.notify_one();
    }

    uint64_t sumUs() const { return m_SumUs; }
    uint64_t samples() const { return m_Samples; }
    // The largest since the previous call
    uint32_t takeMaxUs() const { return m_MaxUs.exchange(0); }

private:
    static constexpr uint32_t kQueue = 16;

    struct Item {
        uint64_t value = 0;
        uint64_t enqueueUs = 0;
    };

    void run()
    {
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> lock(m_Lock);
                m_Wake.wait(lock, [this]() { return m_Stop.load() || m_Count > 0; });
                if (m_Stop.load()) {
                    return;
                }
                item = m_Ring[m_Head];
                m_Head = (m_Head + 1) % kQueue;
                m_Count--;
            }

            VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
            waitInfo.semaphoreCount = 1;
            waitInfo.pSemaphores = &m_Timeline;
            waitInfo.pValues = &item.value;
            VkResult result;
            do {
                // A short timeout so that stop() is noticed promptly
                result = vkWaitSemaphores(m_Device, &waitInfo, 20000000ull);
            } while (result == VK_TIMEOUT && !m_Stop.load());
            if (result != VK_SUCCESS) {
                continue;
            }

            const uint64_t done = nowUs();
            const int64_t clientUs = int64_t(done) - int64_t(item.enqueueUs);
            if (clientUs > 0 && clientUs < 1000000) {
                m_SumUs += uint64_t(clientUs);
                m_Samples++;
                uint32_t peak = m_MaxUs.load(std::memory_order_relaxed);
                while (uint32_t(clientUs) > peak && !m_MaxUs.compare_exchange_weak(peak, uint32_t(clientUs))) {
                }
            }
            else if (done - m_LastRejectLogUs >= 5000000) {
                // An implausible value means the frame stamp is on another clock: say so once in a while
                m_LastRejectLogUs = done;
                pwLog(ANDROID_LOG_INFO, "Client latency sample rejected: %lld us (assembled at %llu us, now %llu us)",
                      (long long)clientUs, (unsigned long long)item.enqueueUs, (unsigned long long)done);
            }
        }
    }

    VkDevice m_Device = VK_NULL_HANDLE;
    VkSemaphore m_Timeline = VK_NULL_HANDLE;
    std::thread m_Thread;
    std::atomic<bool> m_Running { false };
    std::atomic<bool> m_Stop { false };
    std::mutex m_Lock;
    std::condition_variable m_Wake;
    Item m_Ring[kQueue];
    uint32_t m_Head = 0, m_Count = 0;
    uint64_t m_LastRejectLogUs = 0; // probe thread only
    std::atomic<uint64_t> m_SumUs { 0 }, m_Samples { 0 };
    mutable std::atomic<uint32_t> m_MaxUs { 0 };
};

// Enough for the decode of one frame to overlap the render of the previous
// one while a third waits for the display. Each extra frame in flight is a frame
// of latency once the GPU is the bottleneck, and 3 deep did not raise the GPU
// clock (measured), so the queue is kept shallow.
constexpr int k_SurfaceCount = 3;
constexpr int k_FramesInFlight = 2;

// Keep-warm (see Renderer::renderLoop), set from Java before a stream. Mobile GPU clock governors step
// down when the load dips and then climb back only partway, which costs frames. While the host sends
// fewer frames than the display refreshes, the last frame is decoded again at the display's rate so the
// GPU never looks idle. Real frames always take priority.
std::atomic<bool> g_KeepWarm { false };
// Parse each frame on a helper thread as soon as it arrives (see Renderer::prepLoop), so the render thread
// only has to submit it. Experimental, off by default.
std::atomic<bool> g_PreParse { false };
std::atomic<uint32_t> g_WarmPeriodUs { 8333 };
// Stop warming when the host has sent nothing for this long, so a paused stream does not burn power
constexpr uint64_t k_WarmIdleLimitUs = 2000000;

// Just-in-time pacing (see Renderer::renderLoop). When the GPU is the bottleneck, a frame taken the
// moment a slot frees waits behind the frame being decoded, so it is a whole GPU frame old by the time
// its own decode starts. With this on, the render thread takes the newest frame only when the GPU is
// about to be free. Experimental, off by default.
std::atomic<bool> g_JitPacing { false };
// Safety margin before the predicted end of the GPU's current work. A frame taken too late leaves the
// GPU idle for the difference, which costs throughput; one taken too early only waits a little longer.
// Measured: a 0.5 ms margin lost 10-25 fps when the GPU was the bottleneck, so lean towards early.
constexpr uint64_t k_JitMarginUs = 1500;
// The smoothed GPU time per frame includes some waiting, so assume the GPU is a little faster than measured
constexpr double k_JitGpuScale = 0.92;

struct Frame {
    std::vector<uint8_t> data;
    std::vector<PyroWaveFraming::Segment> segments;
    size_t criticalPackets = 0;
    int colorspace = COLORSPACE_REC_709;
    int frameNumber = 0;
    bool hdr = false; // HDR10: PQ-encoded, normally BT.2020
    bool partial = false; // set by the parse thread: the frame was missing records
    // When moonlight-common-c finished assembling the frame, on CLOCK_MONOTONIC in
    // microseconds. The library's stamp has 1 ms resolution, so add the average rounding.
    uint64_t enqueueUs = 0;
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
    void renderLoopPreparsed();
    void prepLoop();
    void paceForGpu();
    void reportDecoderStages();
    void waitForFreeSlot();
    // decoderLock: when not null, the caller holds the decoder lock and process() releases it as soon as the
    // decoder has been used. prepared: the frame was already parsed into the decoder (pre-parse mode).
    void process(const Frame& frame, bool warm, std::unique_lock<std::mutex>* decoderLock = nullptr, bool prepared = false);
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
    std::atomic<bool> m_Stopping { false };
    std::thread m_Thread;

    // Pre-parse mode: a helper thread parses frames into the decoder while the GPU works on the previous one
    std::thread m_PrepThread;
    std::mutex m_DecoderLock; // serialises prepare() and submit() on the decoder, and guards the flags below
    std::condition_variable m_PreparedReady;
    Frame m_PrepWork;         // prep thread only: the frame being parsed
    Frame m_PreparedFrame;    // the frame whose packets are in the decoder, with its metadata
    bool m_PreparedValid = false;
    std::atomic<double> m_ParseUs { 2000.0 };
    PerfHint m_PerfHint; // only touched by the render thread

    // Held while the render thread uses the swapchain, and while it is replaced
    std::mutex m_RenderLock;

    std::atomic<uint32_t> m_Received { 0 }, m_Replaced { 0 }, m_Rejected { 0 }, m_Partial { 0 };
    std::atomic<uint32_t> m_Decoded { 0 }, m_Presented { 0 }, m_NoWindow { 0 };
    std::atomic<uint64_t> m_DecodeUs { 0 }, m_PresentUs { 0 };
    std::atomic<uint32_t> m_OutputWidth { 0 }, m_OutputHeight { 0 };
    std::atomic<bool> m_Mailbox { false };
    std::atomic<bool> m_HdrOutput { false };
    std::atomic<uint64_t> m_LastErrorLogUs { 0 };

    // Detail for the overlay. Counted on the submitting thread, read by the stats thread.
    std::atomic<uint64_t> m_ReceivedBytes { 0 };
    std::atomic<uint64_t> m_PacketsTotal { 0 }, m_PacketsLost { 0 };
    std::atomic<uint64_t> m_ArrivalCount { 0 }, m_ArrivalSumUs { 0 }, m_ArrivalSqSumUs { 0 };
    std::atomic<uint64_t> m_AssemblySumMs { 0 }; // first to last packet of each frame
    uint64_t m_LastArrivalUs = 0; // submitting thread only
    // Peaks since the last stats read
    mutable std::atomic<uint32_t> m_MaxArrivalGapUs { 0 }, m_MaxFrameUs { 0 };
    std::atomic<int> m_Colorspace { COLORSPACE_REC_709 };
    std::atomic<uint32_t> m_SwapchainFormat { 0 }, m_SwapchainImages { 0 };
    std::atomic<int> m_HintKind { 0 };
    GpuTimer m_GpuTimer; // only touched by the render thread, apart from its totals
    LatencyProbe m_LatencyProbe;
    uint32_t m_LatencySampleCounter = 0; // render thread only

    // Keep-warm state, render thread only
    bool m_HaveFrame = false;     // m_Working holds the last real frame
    std::atomic<bool> m_LastDecodeOk { false }; // and it decoded fine (the parse thread can clear it)
    uint64_t m_LastProcessUs = 0; // when the render thread last started a frame
    uint64_t m_LastRealFrameUs = 0;
    std::atomic<uint32_t> m_WarmFrames { 0 };

    // Just-in-time pacing state, render thread only
    uint64_t m_PredDoneUs = 0; // when the GPU is expected to finish what has been submitted
    uint64_t m_LastStageReportUs = 0;
    double m_CpuUs = 1800.0;   // smoothed CPU time from taking a frame to having its decode submitted
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
    if (g_PreParse.load()) {
        m_PrepThread = std::thread(&Renderer::prepLoop, this);
        m_Thread = std::thread(&Renderer::renderLoopPreparsed, this);
        pwLog(ANDROID_LOG_INFO, "Frames are parsed on arrival");
    }
    else {
        m_Thread = std::thread(&Renderer::renderLoop, this);
    }
    m_LatencyProbe.start(m_Vulkan.device(), m_RenderTimeline);
}

void Renderer::stop()
{
    {
        std::lock_guard<std::mutex> guard(m_FrameLock);
        m_Stopping = true;
        m_PendingValid = false;
    }
    m_FrameReady.notify_all();
    {
        // Take the decoder lock once so a wait on the render side cannot miss the wake-up
        std::lock_guard<std::mutex> guard(m_DecoderLock);
    }
    m_PreparedReady.notify_all();
    if (m_PrepThread.joinable()) {
        m_PrepThread.join();
    }
    if (m_Thread.joinable()) {
        m_Thread.join();
    }
    m_LatencyProbe.stop();
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
    frame.enqueueUs = du->enqueueTimeMs * 1000 + 500;
    // First packet to last packet: how long the frame took to arrive
    if (du->enqueueTimeMs >= du->receiveTimeMs) {
        m_AssemblySumMs += du->enqueueTimeMs - du->receiveTimeMs;
    }

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
    // The target is about half a frame at 120 Hz (8.33 ms): the GPU work takes longer than that, so the
    // system keeps seeing late work and keeps the clock up. A 7.5 ms target let the governor settle one
    // step short of what the load needs.
    m_PerfHint.start(4000000);
    m_HintKind = m_PerfHint.kind();

    // Same boost the MediaCodec renderer thread gets (THREAD_PRIORITY_URGENT_DISPLAY)
    if (setpriority(PRIO_PROCESS, 0, -8) != 0) {
        pwLog(ANDROID_LOG_INFO, "Render thread priority boost was refused");
    }

    for (;;) {
        // Wait for the GPU to have room before taking a frame, so the frame taken is the
        // newest one: frames arriving during the wait replace the pending one instead of
        // queueing behind a stale one
        paceForGpu();
        waitForFreeSlot();
        bool warm = false;
        {
            std::unique_lock<std::mutex> lock(m_FrameLock);
            const auto ready = [this]() { return m_Stopping || m_PendingValid; };

            // Keep-warm: if the host has not sent a new frame by the time one display refresh has passed
            // since the last frame started, decode the last one again. A real frame always wins, because
            // the wait ends as soon as one is pending.
            const bool canWarm = g_KeepWarm.load(std::memory_order_relaxed) && m_HaveFrame && m_LastDecodeOk &&
                                 nowUs() - m_LastRealFrameUs < k_WarmIdleLimitUs;
            if (canWarm) {
                const std::chrono::steady_clock::time_point deadline{
                    std::chrono::microseconds(m_LastProcessUs + g_WarmPeriodUs.load(std::memory_order_relaxed)) };
                warm = !m_FrameReady.wait_until(lock, deadline, ready);
            }
            else {
                m_FrameReady.wait(lock, ready);
            }

            if (m_Stopping) {
                m_PerfHint.stop();
                return;
            }
            if (!warm) {
                std::swap(m_Pending, m_Working);
                m_PendingValid = false;
                m_HaveFrame = true;
                m_LastRealFrameUs = nowUs();
            }
        }

        std::lock_guard<std::mutex> guard(m_RenderLock);
        process(m_Working, warm);
    }
}

// Just-in-time pacing: wait until the GPU is about to be free, so that the newest frame is taken as late as
// possible and its CPU work ends as the GPU becomes available
void Renderer::paceForGpu()
{
    if (!g_JitPacing.load(std::memory_order_relaxed)) {
        return;
    }
    const uint32_t gpuUs = m_GpuTimer.frameEmaUs();
    if (gpuUs == 0 || m_PredDoneUs == 0) {
        return;
    }
    const uint64_t lead = uint64_t(m_CpuUs) + k_JitMarginUs;
    const uint64_t target = m_PredDoneUs > lead ? m_PredDoneUs - lead : 0;
    const uint64_t now = nowUs();
    if (target > now) {
        const uint64_t sleepUs = std::min<uint64_t>(target - now, 20000);
        std::unique_lock<std::mutex> lock(m_FrameLock);
        m_FrameReady.wait_for(lock, std::chrono::microseconds(sleepUs), [this]() { return m_Stopping.load(); });
    }
}

void Renderer::reportDecoderStages()
{
    // The decoder is not thread safe: wait for any parse in progress
    std::lock_guard<std::mutex> guard(m_DecoderLock);
    m_Decoder.reportStages([](void*, const char* message) {
        pwLog(ANDROID_LOG_INFO, "Decoder stage: %s", message);
    });
}

// Pre-parse mode, helper thread: takes the newest arrived frame and parses it into the decoder. The library
// copies the decoder's CPU buffers when the GPU work is recorded, so this can overlap the GPU decoding the
// previous frame. A frame parsed but not yet submitted when a newer one arrives is simply parsed over.
void Renderer::prepLoop()
{
    if (setpriority(PRIO_PROCESS, 0, -4) != 0) {
        pwLog(ANDROID_LOG_INFO, "Parse thread priority boost was refused");
    }
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_FrameLock);
            m_FrameReady.wait(lock, [this]() { return m_Stopping.load() || m_PendingValid; });
            if (m_Stopping) {
                return;
            }
            std::swap(m_Pending, m_PrepWork);
            m_PendingValid = false;
        }

        std::unique_lock<std::mutex> decoderLock(m_DecoderLock);
        if (m_Stopping) {
            return;
        }
        if (m_PreparedValid) {
            // The frame parsed before was never submitted: this newer one replaces it
            m_Replaced++;
            m_PreparedValid = false;
        }
        const uint64_t start = nowUs();
        if (!m_Decoder.prepare(m_PrepWork.data.data(), m_PrepWork.data.size(), m_PrepWork.segments, m_PrepWork.criticalPackets)) {
            m_Rejected++;
            m_LastDecodeOk = false;
            const uint64_t now = nowUs();
            if (now - m_LastErrorLogUs.load() >= 1000000) {
                m_LastErrorLogUs = now;
                pwLog(ANDROID_LOG_WARN, "Dropped frame %d: %s (%u dropped so far)",
                      m_PrepWork.frameNumber, m_Decoder.lastError().c_str(), m_Rejected.load());
            }
            continue;
        }
        m_PrepWork.partial = m_Decoder.lastFramePartial();
        std::swap(m_PrepWork, m_PreparedFrame);
        m_PreparedValid = true;
        m_ParseUs = m_ParseUs.load() * 0.9 + double(nowUs() - start) * 0.1;
        decoderLock.unlock();
        m_PreparedReady.notify_one();
    }
}

// Pre-parse mode, render thread: submits whatever the parse thread has prepared. Waiting for the decoder lock
// also waits for a parse in progress, so the frame submitted is the newest one that has arrived.
void Renderer::renderLoopPreparsed()
{
    m_PerfHint.start(4000000);
    m_HintKind = m_PerfHint.kind();
    if (setpriority(PRIO_PROCESS, 0, -8) != 0) {
        pwLog(ANDROID_LOG_INFO, "Render thread priority boost was refused");
    }

    for (;;) {
        paceForGpu();
        waitForFreeSlot();

        std::unique_lock<std::mutex> decoderLock(m_DecoderLock);
        const auto ready = [this]() { return m_Stopping.load() || m_PreparedValid; };
        const bool canWarm = g_KeepWarm.load(std::memory_order_relaxed) && m_HaveFrame && m_LastDecodeOk &&
                             nowUs() - m_LastRealFrameUs < k_WarmIdleLimitUs;
        bool warm = false;
        if (canWarm) {
            const std::chrono::steady_clock::time_point deadline{
                std::chrono::microseconds(m_LastProcessUs + g_WarmPeriodUs.load(std::memory_order_relaxed)) };
            warm = !m_PreparedReady.wait_until(decoderLock, deadline, ready);
        }
        else {
            m_PreparedReady.wait(decoderLock, ready);
        }
        if (m_Stopping) {
            m_PerfHint.stop();
            return;
        }

        std::lock_guard<std::mutex> guard(m_RenderLock);
        if (warm) {
            // Nothing new has been parsed: decode the last real frame again (this parses it itself)
            process(m_Working, true, &decoderLock, false);
        }
        else {
            std::swap(m_PreparedFrame, m_Working);
            m_PreparedValid = false;
            m_HaveFrame = true;
            m_LastRealFrameUs = nowUs();
            process(m_Working, false, &decoderLock, true);
        }
    }
}

// Blocks until the submission that last used the next frame slot has finished on the GPU
void Renderer::waitForFreeSlot()
{
    const uint32_t slot = m_FrameIndex % k_FramesInFlight;
    if (m_RenderTimeline == VK_NULL_HANDLE || m_FrameValues[slot] == 0) {
        return;
    }
    VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &m_RenderTimeline;
    waitInfo.pValues = &m_FrameValues[slot];
    vkWaitSemaphores(m_Vulkan.device(), &waitInfo, UINT64_MAX);
}

void Renderer::process(const Frame& frame, bool warm, std::unique_lock<std::mutex>* decoderLock, bool prepared)
{
    if (!m_Swapchain.valid()) {
        if (!warm) {
            m_NoWindow++;
        }
        return;
    }

    // Follow the stream: HDR10 frames need an HDR swapchain and SDR frames an ordinary
    // one. The rebuild happens in present(), which waits for the GPU to go idle first.
    if (frame.hdr != m_Swapchain.wantsHdr()) {
        m_Swapchain.setHdr(frame.hdr);
        m_NeedRecreate = true;
    }

    // The slot of the frames in flight this frame will use, as present() picks it. One real frame
    // in a few has its GPU stages timed: begin() writes the first timestamp.
    const int slot = int(m_FrameIndex % k_FramesInFlight);
    const bool timed = !warm && m_GpuTimer.begin(slot);

    const uint64_t start = nowUs();
    m_LastProcessUs = start;
    const int surface = m_NextSurface;
    uint64_t decodeValue = 0;
    const uint64_t released = m_SurfaceReleased[surface];
    const VkSemaphore waitSemaphore = released != 0 ? m_RenderTimeline : VK_NULL_HANDLE;
    const bool decodedOk = prepared ?
            m_Decoder.submit(surface, waitSemaphore, released, decodeValue) :
            m_Decoder.decode(frame.data.data(), frame.data.size(), frame.segments, frame.criticalPackets, surface,
                             waitSemaphore, released, decodeValue);
    // The decoder's state belongs to whoever holds the lock: read what is needed, then let the parse thread go on
    const bool partialFrame = decodedOk && m_Decoder.lastFramePartial();
    const std::string failure = decodedOk ? std::string() : m_Decoder.lastError();
    if (decoderLock != nullptr && decoderLock->owns_lock()) {
        decoderLock->unlock();
    }
    if (!decodedOk) {
        if (timed) {
            m_GpuTimer.decoded(slot, false);
            m_GpuTimer.presented(slot, false);
        }
        // Do not keep re-decoding a frame that failed
        m_LastDecodeOk = false;
        if (!warm) {
            m_Rejected++;
            // The next frame replaces this one; log at most once a second
            const uint64_t now = nowUs();
            if (now - m_LastErrorLogUs.load() >= 1000000) {
                m_LastErrorLogUs = now;
                pwLog(ANDROID_LOG_WARN, "Dropped frame %d: %s (%u dropped so far)",
                      frame.frameNumber, failure.c_str(), m_Rejected.load());
            }
        }
        return;
    }
    m_NextSurface = (m_NextSurface + 1) % k_SurfaceCount;
    if (warm) {
        m_WarmFrames++;
    }
    else {
        if (partialFrame) {
            m_Partial++;
        }
        m_Decoded++;
        m_LastDecodeOk = true;
    }
    if (timed) {
        m_GpuTimer.decoded(slot, true);
    }

    const uint64_t decoded = nowUs();

    // The GPU starts this frame's decode when it finishes the previous work, or now if it is idle
    const uint32_t gpuFrameUs = m_GpuTimer.frameEmaUs();
    if (gpuFrameUs != 0) {
        m_PredDoneUs = std::max<uint64_t>(decoded, m_PredDoneUs) + uint64_t(double(gpuFrameUs) * k_JitGpuScale);
    }
    if (!warm) {
        m_CpuUs = m_CpuUs * 0.9 + double(decoded - start) * 0.1;
    }

    const bool presented = present(surface, decodeValue, frame.colorspace);
    if (timed) {
        m_GpuTimer.presented(slot, presented);
    }
    const uint64_t end = nowUs();

    // Re-decodes are not frames the host sent: the stats and the latency probe only count real ones
    if (!warm) {
        m_DecodeUs += decoded - start;
        if (presented) {
            m_Presented++;
        }
        m_PresentUs += end - decoded;

        // One frame in four: how long from assembled to the GPU finishing it
        if (presented && frame.enqueueUs != 0 && (++m_LatencySampleCounter % 4) == 0) {
            m_LatencyProbe.sample(m_RenderValue, frame.enqueueUs);
        }

        // Slowest frame since the stats were last read
        const uint32_t frameUs = uint32_t(std::min<uint64_t>(end - start, 1000000));
        uint32_t peak = m_MaxFrameUs.load(std::memory_order_relaxed);
        while (frameUs > peak && !m_MaxFrameUs.compare_exchange_weak(peak, frameUs)) {
        }
    }

    // Every ten seconds, log the library's own per-stage GPU timings: where the decode time goes
    if (!warm && end - m_LastStageReportUs >= 10000000) {
        m_LastStageReportUs = end;
        reportDecoderStages();
    }

    // Present mostly waits for the GPU to finish earlier frames, so it stands in for GPU time
    m_PerfHint.report(int64_t(start) * 1000, int64_t(end - start) * 1000,
                      int64_t(decoded - start) * 1000,
                      gpuFrameUs != 0 ? int64_t(gpuFrameUs) * 1000 : int64_t(end - decoded) * 1000);
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

    out->assemblySumMs = m_AssemblySumMs;
    out->clientTiming = m_LatencyProbe.enabled();
    out->clientSumUs = m_LatencyProbe.sumUs();
    out->clientSamples = m_LatencyProbe.samples();
    out->clientMaxUs = m_LatencyProbe.takeMaxUs();
    out->warmFrames = m_WarmFrames;
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

extern "C" void PwRendererSetKeepWarm(bool enabled, int refreshHz)
{
    g_WarmPeriodUs = (refreshHz >= 30 && refreshHz <= 240) ? uint32_t(1000000 / refreshHz) : 8333;
    g_KeepWarm = enabled;
}

extern "C" void PwRendererSetPacing(bool justInTime)
{
    g_JitPacing = justInTime;
}

extern "C" void PwRendererSetPreParse(bool enabled)
{
    g_PreParse = enabled;
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
