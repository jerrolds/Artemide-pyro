#include "pyrowave_renderer.h"

#include "pw_decoder.h"
#include "pw_presenter.h"
#include "pw_swapchain.h"
#include "pw_vulkan.h"

#include <android/log.h>
#include <android/native_window.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
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

// Enough for the decode of one frame to overlap the render of the previous
// one while a third waits for the display.
constexpr int k_SurfaceCount = 3;
constexpr int k_FramesInFlight = 2;

struct Frame {
    std::vector<uint8_t> data;
    std::vector<PyroWaveFraming::Segment> segments;
    size_t criticalPackets = 0;
    int colorspace = COLORSPACE_REC_709;
    int frameNumber = 0;
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

    // Held while the render thread uses the swapchain, and while it is replaced
    std::mutex m_RenderLock;

    std::atomic<uint32_t> m_Received { 0 }, m_Replaced { 0 }, m_Rejected { 0 }, m_Partial { 0 };
    std::atomic<uint32_t> m_Decoded { 0 }, m_Presented { 0 }, m_NoWindow { 0 };
    std::atomic<uint64_t> m_DecodeUs { 0 }, m_PresentUs { 0 };
    uint64_t m_LastErrorLogUs = 0;
};

Renderer::~Renderer()
{
    stop();
    if (m_Vulkan.device() == VK_NULL_HANDLE) {
        return;
    }
    waitIdle();
    m_Swapchain.destroy();
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
    pwLog(ANDROID_LOG_INFO, "Presenting %ux%u, %s, rotation %d",
          m_Swapchain.extent().width, m_Swapchain.extent().height,
          m_Swapchain.presentMode() == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "FIFO",
          m_Swapchain.rotation() * 90);
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

    // Copy the frame out of moonlight-common-c's buffers, one segment per RTP
    // packet so the parser knows what was lost and where records start
    Frame& frame = m_Staging;
    frame.data.resize(size_t(du->fullLength));
    frame.segments.clear();
    size_t offset = 0;
    for (PLENTRY entry = du->bufferList; entry != nullptr; entry = entry->next) {
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
    frame.criticalPackets = du->pyrowaveCriticalPackets;
    frame.colorspace = du->colorspace;
    frame.frameNumber = du->frameNumber;

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
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_FrameLock);
            m_FrameReady.wait(lock, [this]() { return m_Stopping || m_PendingValid; });
            if (m_Stopping) {
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

    const uint64_t start = nowUs();
    const int surface = m_NextSurface;
    uint64_t decodeValue = 0;
    const uint64_t released = m_SurfaceReleased[surface];
    if (!m_Decoder.decode(frame.data.data(), frame.data.size(), frame.segments, frame.criticalPackets, surface,
                          released != 0 ? m_RenderTimeline : VK_NULL_HANDLE, released, decodeValue)) {
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

    const uint64_t decoded = nowUs();
    m_DecodeUs += decoded - start;

    if (present(surface, decodeValue, frame.colorspace)) {
        m_Presented++;
    }
    m_PresentUs += nowUs() - decoded;
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
            return false;
        }
        m_NeedRecreate = false;
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
