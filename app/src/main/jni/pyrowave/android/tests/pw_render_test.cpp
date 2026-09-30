// Desktop check of the Android PyroWave path on a PwVulkan device.
//
// Always: uploads known YCbCr planes into PwDecoder's output surfaces and
// converts them with PwPresenter into an offscreen RGBA image, checking the
// colours, letterboxing, rotation and semaphore ordering. Runs on any Vulkan
// 1.3 driver, including Mesa's lavapipe.
//
// With PW_TEST_GPU_DECODE=1: also encodes the same image with the vendored
// encoder, frames it the way Vibepollo does (record framing aligned to RTP
// payloads), decodes it with PwDecoder (whole, and with a lost payload) and
// checks the rendered result. Needs a real GPU: the encoder wants 16-wide
// subgroups, and lavapipe (Mesa 25, LLVM 20) corrupts its stack running
// PyroWave's decode shaders, in upstream's own decode path as well.
//
// Exits 0 without checks when no Vulkan 1.3 device exists.

#include "../pw_decoder.h"
#include "../pw_presenter.h"
#include "../pw_vulkan.h"

#include <pyrowave.h>
#include <Limelight.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_Failures = 0;

void expect(bool condition, const std::string& description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description.c_str());
        ++g_Failures;
    }
}

// Four flat quadrants, limited-range BT.709 YCbCr, with their expected RGB
struct Quadrant {
    uint8_t y, cb, cr;
    uint8_t r, g, b;
};

const Quadrant k_Quadrants[4] = {
    { 235, 128, 128, 255, 255, 255 }, // white
    { 63, 102, 240, 255, 0, 0 },      // red
    { 173, 42, 26, 0, 255, 0 },       // green
    { 32, 240, 118, 0, 0, 255 },      // blue
};

struct Planes {
    int width = 0, height = 0;
    std::vector<uint8_t> y, cb, cr;

    void fill(int w, int h)
    {
        width = w;
        height = h;
        y.assign(size_t(w) * h, 0);
        cb.assign(size_t(w / 2) * (h / 2), 0);
        cr.assign(size_t(w / 2) * (h / 2), 0);
        for (int yy = 0; yy < h; yy++) {
            for (int xx = 0; xx < w; xx++) {
                const Quadrant& q = k_Quadrants[(yy >= h / 2 ? 2 : 0) + (xx >= w / 2 ? 1 : 0)];
                y[size_t(yy) * w + xx] = q.y;
                if (!(xx & 1) && !(yy & 1)) {
                    cb[size_t(yy / 2) * (w / 2) + xx / 2] = q.cb;
                    cr[size_t(yy / 2) * (w / 2) + xx / 2] = q.cr;
                }
            }
        }
    }

    pyrowave_cpu_buffer buffer()
    {
        pyrowave_cpu_buffer buf = {};
        buf.data[0] = y.data();
        buf.data[1] = cb.data();
        buf.data[2] = cr.data();
        buf.row_stride_in_bytes[0] = size_t(width);
        buf.row_stride_in_bytes[1] = size_t(width / 2);
        buf.row_stride_in_bytes[2] = size_t(width / 2);
        buf.plane_size_in_bytes[0] = y.size();
        buf.plane_size_in_bytes[1] = cb.size();
        buf.plane_size_in_bytes[2] = cr.size();
        buf.width = width;
        buf.height = height;
        buf.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
        return buf;
    }
};

void putU32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 0; i < 4; i++) {
        out.push_back(uint8_t(value >> (8 * i)));
    }
}

void putPadding(std::vector<uint8_t>& out, size_t bytes)
{
    putU32(out, 0xFFFFFFFFu);
    putU32(out, uint32_t((bytes - 8) / 4));
    out.resize(out.size() + (bytes - 8), 0);
}

// Record framing with the host's layout guarantees (from moonlight-qt's
// tst_pyrowaveroundtrip): sequence header, coarsest level, then the rest, no
// record crossing a payload boundary except oversized ones placed first.
std::vector<uint8_t> recordFrame(const std::vector<uint8_t>& bitstream,
                                 const std::vector<pyrowave_packet>& packets,
                                 size_t shard, uint32_t coarseBlocks, size_t& criticalBytes)
{
    struct Record {
        size_t offset;
        size_t size;
        bool coarse;
    };
    std::vector<Record> records;
    for (const auto& packet : packets) {
        for (size_t pos = packet.offset; pos < packet.offset + packet.size;) {
            uint32_t word0, word1;
            std::memcpy(&word0, bitstream.data() + pos, 4);
            std::memcpy(&word1, bitstream.data() + pos + 4, 4);
            const bool header = (word0 & 0x80000000u) != 0;
            const size_t size = header ? 8 : size_t((word0 >> 16) & 0xFFF) * 4;
            records.push_back({pos, size, header || (word1 >> 8) < coarseBlocks});
            pos += size;
        }
    }

    std::vector<uint8_t> out;
    auto remaining = [&]() { return shard - (out.size() + 8) % shard; };
    auto place = [&](const Record& record) {
        out.insert(out.end(), bitstream.begin() + record.offset,
                   bitstream.begin() + record.offset + record.size);
    };

    place(records.front());
    for (bool coarse : {true, false}) {
        for (size_t i = 1; i < records.size(); i++) {
            if (records[i].coarse == coarse && records[i].size + 8 > shard) {
                if (records[i].size % shard == remaining() - 4) {
                    putPadding(out, 8);
                }
                place(records[i]);
            }
        }
        for (size_t i = 1; i < records.size(); i++) {
            if (records[i].coarse == coarse && records[i].size + 8 <= shard) {
                if (records[i].size > remaining() || remaining() - records[i].size == 4) {
                    putPadding(out, remaining());
                }
                place(records[i]);
            }
        }
        if (coarse) {
            criticalBytes = out.size();
        }
    }
    return out;
}

// Splits a framed frame into RTP payloads as moonlight-common-c delivers them,
// flagging record starts; lostPayload (if in range) is zero-filled and marked lost.
std::vector<PyroWaveFraming::Segment> segmentFrame(std::vector<uint8_t>& framed, size_t shard, size_t lostPayload)
{
    std::vector<bool> recordStart(framed.size(), false);
    for (size_t pos = 0; pos + 8 <= framed.size();) {
        recordStart[pos] = true;
        uint32_t word0, word1;
        std::memcpy(&word0, framed.data() + pos, 4);
        std::memcpy(&word1, framed.data() + pos + 4, 4);
        pos += word0 == 0xFFFFFFFFu ? 8 + size_t(word1) * 4 :
               (word0 & 0x80000000u) ? 8 : size_t((word0 >> 16) & 0xFFF) * 4;
    }

    std::vector<PyroWaveFraming::Segment> segments;
    for (size_t offset = 0, index = 0; offset < framed.size(); index++) {
        const size_t size = std::min(framed.size() - offset, index == 0 ? shard - 8 : shard);
        segments.push_back({offset, size, index == lostPayload, bool(recordStart[offset])});
        if (index == lostPayload) {
            std::fill(framed.begin() + offset, framed.begin() + offset + size, uint8_t(0));
        }
        offset += size;
    }
    return segments;
}

struct Offscreen {
    PwVulkan* vulkan = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    VkExtent2D extent {};

    bool create(PwVulkan& vk, uint32_t width, uint32_t height)
    {
        vulkan = &vk;
        extent = { width, height };
        VkDevice device = vk.device();

        VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, image, &req);
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = vk.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &alloc, nullptr, &memory);
        vkBindImageMemory(device, image, memory, 0);

        VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(device, &viewInfo, nullptr, &view);

        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = VkDeviceSize(width) * height * 4;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);
        vkGetBufferMemoryRequirements(device, buffer, &req);
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = vk.findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &alloc, nullptr, &bufferMemory);
        vkBindBufferMemory(device, buffer, bufferMemory, 0);
        return true;
    }

    void destroy()
    {
        VkDevice device = vulkan->device();
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, bufferMemory, nullptr);
        vkDestroyImageView(device, view, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }

    const uint8_t* pixel(const uint8_t* data, uint32_t x, uint32_t y) const
    {
        return data + (size_t(y) * extent.width + x) * 4;
    }
};

class Harness {
public:
    PwVulkan vulkan;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkSemaphore renderTimeline = VK_NULL_HANDLE;
    uint64_t renderValue = 0;
    // Stands in for the decoder's semaphore when planes are uploaded
    VkSemaphore uploadTimeline = VK_NULL_HANDLE;
    uint64_t uploadValue = 0;

    bool init(std::string& error)
    {
        if (!vulkan.create(false, error)) {
            return false;
        }
        VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = vulkan.queueFamily();
        vkCreateCommandPool(vulkan.device(), &poolInfo, nullptr, &pool);

        VkSemaphoreTypeCreateInfo timelineInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        semInfo.pNext = &timelineInfo;
        vkCreateSemaphore(vulkan.device(), &semInfo, nullptr, &renderTimeline);
        vkCreateSemaphore(vulkan.device(), &semInfo, nullptr, &uploadTimeline);
        return true;
    }

    ~Harness()
    {
        if (vulkan.device() != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vulkan.device());
            vkDestroySemaphore(vulkan.device(), renderTimeline, nullptr);
            vkDestroySemaphore(vulkan.device(), uploadTimeline, nullptr);
            vkDestroyCommandPool(vulkan.device(), pool, nullptr);
        }
    }

    // Copies planes into a decoder surface as a decode would write them: after
    // the surface's last render (waitValue on renderTimeline), signalling
    // uploadTimeline. Returns the value to wait for.
    uint64_t upload(const PwDecoder::Surface& surface, const Planes& planes, uint64_t waitValue)
    {
        const std::vector<uint8_t>* data[3] = { &planes.y, &planes.cb, &planes.cr };
        VkDeviceSize total = 0;
        for (auto* plane : data) {
            total += plane->size();
        }

        VkBuffer staging;
        VkDeviceMemory stagingMemory;
        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = total;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(vulkan.device(), &bufferInfo, nullptr, &staging);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vulkan.device(), staging, &req);
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = vulkan.findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(vulkan.device(), &alloc, nullptr, &stagingMemory);
        vkBindBufferMemory(vulkan.device(), staging, stagingMemory, 0);
        uint8_t* mapped = nullptr;
        vkMapMemory(vulkan.device(), stagingMemory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&mapped));

        VkCommandBufferAllocateInfo cmdAlloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cmdAlloc.commandPool = pool;
        cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAlloc.commandBufferCount = 1;
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(vulkan.device(), &cmdAlloc, &cmd);
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        VkDeviceSize offset = 0;
        for (int plane = 0; plane < 3; plane++) {
            std::memcpy(mapped + offset, data[plane]->data(), data[plane]->size());
            VkBufferImageCopy region = {};
            region.bufferOffset = offset;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { surface.extents[plane].width, surface.extents[plane].height, 1 };
            vkCmdCopyBufferToImage(cmd, staging, surface.images[plane], VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            offset += data[plane]->size();
        }
        vkEndCommandBuffer(cmd);

        VkSemaphoreSubmitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        wait.semaphore = renderTimeline;
        wait.value = waitValue;
        wait.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        VkSemaphoreSubmitInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        signal.semaphore = uploadTimeline;
        signal.value = ++uploadValue;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = cmd;
        VkSubmitInfo2 submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        {
            std::lock_guard<std::mutex> guard(vulkan.queueLock());
            vkQueueSubmit2(vulkan.queue(), 1, &submit, VK_NULL_HANDLE);
        }

        VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &uploadTimeline;
        waitInfo.pValues = &uploadValue;
        vkWaitSemaphores(vulkan.device(), &waitInfo, UINT64_MAX);
        vkFreeCommandBuffers(vulkan.device(), pool, 1, &cmd);
        vkDestroyBuffer(vulkan.device(), staging, nullptr);
        vkFreeMemory(vulkan.device(), stagingMemory, nullptr);
        return uploadValue;
    }

    // Renders a decoded surface into target and reads it back, the way the
    // Android renderer submits: wait for the decode, signal the render timeline.
    std::vector<uint8_t> render(PwPresenter& presenter, int surface, uint64_t decodeValue,
                                VkSemaphore decodeSemaphore, Offscreen& target, int rotation)
    {
        VkCommandBufferAllocateInfo alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(vulkan.device(), &alloc, &cmd);
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        PwColor color;
        color.colorspace = COLORSPACE_REC_709;
        color.fullRange = false;
        presenter.record(cmd, surface, color, target.image, target.view, target.extent, rotation,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT);

        VkBufferImageCopy region = {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { target.extent.width, target.extent.height, 1 };
        vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.buffer, 1, &region);
        vkEndCommandBuffer(cmd);

        VkSemaphoreSubmitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        wait.semaphore = decodeSemaphore;
        wait.value = decodeValue;
        wait.stageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        VkSemaphoreSubmitInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        signal.semaphore = renderTimeline;
        signal.value = ++renderValue;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = cmd;
        VkSubmitInfo2 submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        {
            std::lock_guard<std::mutex> guard(vulkan.queueLock());
            vkQueueSubmit2(vulkan.queue(), 1, &submit, VK_NULL_HANDLE);
        }

        VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &renderTimeline;
        waitInfo.pValues = &renderValue;
        vkWaitSemaphores(vulkan.device(), &waitInfo, UINT64_MAX);
        vkFreeCommandBuffers(vulkan.device(), pool, 1, &cmd);

        std::vector<uint8_t> pixels(size_t(target.extent.width) * target.extent.height * 4);
        void* mapped = nullptr;
        vkMapMemory(vulkan.device(), target.bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
        std::memcpy(pixels.data(), mapped, pixels.size());
        vkUnmapMemory(vulkan.device(), target.bufferMemory);
        return pixels;
    }
};

bool near(const uint8_t* px, const Quadrant& q, int tolerance)
{
    return std::abs(int(px[0]) - q.r) <= tolerance && std::abs(int(px[1]) - q.g) <= tolerance &&
           std::abs(int(px[2]) - q.b) <= tolerance;
}

std::string describe(const uint8_t* px)
{
    return "(" + std::to_string(px[0]) + "," + std::to_string(px[1]) + "," + std::to_string(px[2]) + ")";
}

// Checks each quadrant's centre inside the video rectangle [x0, x0 + w) x [y0, y0 + h)
void checkQuadrants(const Offscreen& target, const std::vector<uint8_t>& pixels,
                    uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, bool flipped, const std::string& name)
{
    for (int i = 0; i < 4; i++) {
        uint32_t qx = (i & 1) ? x0 + 3 * w / 4 : x0 + w / 4;
        uint32_t qy = (i & 2) ? y0 + 3 * h / 4 : y0 + h / 4;
        if (flipped) {
            qx = target.extent.width - 1 - qx;
            qy = target.extent.height - 1 - qy;
        }
        const uint8_t* px = target.pixel(pixels.data(), qx, qy);
        expect(near(px, k_Quadrants[i], 6), name + ": quadrant " + std::to_string(i) + " is " + describe(px));
    }
}

// Encodes planes with the vendored encoder into a bitstream and its packets
bool encode(const Planes& source, size_t shard, std::vector<uint8_t>& bitstream,
            std::vector<pyrowave_packet>& packets)
{
    pyrowave_device device = nullptr;
    if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS) {
        return false;
    }
    pyrowave_encoder_create_info info = {};
    info.device = device;
    info.width = source.width;
    info.height = source.height;
    info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    pyrowave_encoder encoder = nullptr;
    bool ok = pyrowave_encoder_create(&info, &encoder) == PYROWAVE_SUCCESS;
    if (ok) {
        Planes copy = source;
        auto input = copy.buffer();
        const size_t budget = 400 * 1024;
        pyrowave_rate_control rate = { budget };
        size_t count = 0, written = 0;
        ok = pyrowave_encoder_encode_cpu_synchronous(encoder, &input, &rate) == PYROWAVE_SUCCESS &&
             pyrowave_encoder_compute_num_packets_with_padding(encoder, shard, 8, &count) == PYROWAVE_SUCCESS;
        if (ok) {
            packets.resize(count);
            bitstream.resize(budget + 1024 * 1024);
            ok = pyrowave_encoder_packetize_with_padding(encoder, packets.data(), shard, 8, &written,
                                                         bitstream.data(), bitstream.size()) == PYROWAVE_SUCCESS;
            packets.resize(written);
        }
        pyrowave_encoder_destroy(encoder);
    }
    pyrowave_device_destroy(device);
    return ok;
}

}

int main()
{
    Harness harness;
    std::string error;
    if (!harness.init(error)) {
        std::printf("No usable Vulkan 1.3 device (%s); skipping\n", error.c_str());
        return 0;
    }
    std::printf("Vulkan device: %s\n", harness.vulkan.deviceName());

    const int width = 1280, height = 720;
    const PyroWaveFraming::StreamGeometry geometry { width, height, false };

    PwDecoder decoder;
    PwDecoder::Config config;
    config.width = width;
    config.height = height;
    if (!decoder.initialize(harness.vulkan, config, 3)) {
        std::fprintf(stderr, "FAIL: decoder initialization: %s\n", decoder.lastError().c_str());
        return 1;
    }
    std::printf("Decoder ready (%s path)\n", decoder.fragmentPath() ? "fragment" : "compute");

    PwPresenter presenter;
    if (!presenter.initialize(harness.vulkan, decoder, error) ||
            !presenter.setTargetFormat(VK_FORMAT_R8G8B8A8_UNORM, error)) {
        std::fprintf(stderr, "FAIL: presenter initialization: %s\n", error.c_str());
        return 1;
    }

    Planes source;
    source.fill(width, height);

    Offscreen same, wide;
    expect(same.create(harness.vulkan, width, height), "offscreen target");
    expect(wide.create(harness.vulkan, 1600, 720), "wide offscreen target");

    // Presenter: cycle through the surfaces twice so each upload waits on the
    // surface's previous render, as decodes do
    uint64_t lastRead[3] = {};
    for (int frame = 0; frame < 6; frame++) {
        const int surface = frame % 3;
        const uint64_t ready = harness.upload(decoder.surface(surface), source, lastRead[surface]);
        const std::string name = "uploaded frame " + std::to_string(frame);
        if (frame == 1) {
            // 1600x720 target: 1280x720 video centred with 160-pixel black bars
            auto pixels = harness.render(presenter, surface, ready, harness.uploadTimeline, wide, 0);
            checkQuadrants(wide, pixels, 160, 0, 1280, 720, false, name + " letterboxed");
            expect(near(wide.pixel(pixels.data(), 40, 360), { 0, 0, 0, 0, 0, 0 }, 0), name + ": left bar is black");
            expect(near(wide.pixel(pixels.data(), 1560, 360), { 0, 0, 0, 0, 0, 0 }, 0), name + ": right bar is black");
        }
        else if (frame == 2) {
            auto pixels = harness.render(presenter, surface, ready, harness.uploadTimeline, same, 2);
            checkQuadrants(same, pixels, 0, 0, width, height, true, name + " rotated 180");
        }
        else {
            auto pixels = harness.render(presenter, surface, ready, harness.uploadTimeline, same, 0);
            checkQuadrants(same, pixels, 0, 0, width, height, false, name);
        }
        lastRead[surface] = harness.renderValue;
    }

    if (std::getenv("PW_TEST_GPU_DECODE") != nullptr) {
        const size_t shard = 1392 - 16;
        std::vector<uint8_t> bitstream;
        std::vector<pyrowave_packet> packets;
        if (!encode(source, shard, bitstream, packets)) {
            expect(false, "encoding the test image");
        }
        else {
            size_t criticalBytes = 0;
            const auto records = recordFrame(bitstream, packets, shard, PyroWaveFraming::coarseBlockCount(geometry),
                                             criticalBytes);
            const size_t criticalPackets = (criticalBytes + 8 + shard - 1) / shard;
            std::printf("Encoded frame: %zu bytes in %zu payloads, %zu critical\n",
                        records.size(), (records.size() + 8 + shard - 1) / shard, criticalPackets);

            for (int frame = 0; frame < 4; frame++) {
                const int surface = frame % 3;
                const bool lossy = frame == 3;
                std::vector<uint8_t> framed = records;
                // Lose a payload just past the critical ones on the last frame
                auto segments = segmentFrame(framed, shard, lossy ? criticalPackets + 1 : SIZE_MAX);

                const std::string name = "decoded frame " + std::to_string(frame);
                uint64_t decodeValue = 0;
                const bool decoded = decoder.decode(framed.data(), framed.size(), segments, criticalPackets, surface,
                                                    harness.renderTimeline, lastRead[surface], decodeValue);
                expect(decoded, name + " decodes: " + decoder.lastError());
                if (!decoded) {
                    continue;
                }
                expect(decoder.lastFramePartial() == lossy, name + " partial flag");

                auto pixels = harness.render(presenter, surface, decodeValue, decoder.decodeSemaphore(), same, 0);
                if (!lossy) {
                    checkQuadrants(same, pixels, 0, 0, width, height, false, name);
                }
                lastRead[surface] = harness.renderValue;
            }
        }
    }

    vkDeviceWaitIdle(harness.vulkan.device());
    same.destroy();
    wide.destroy();

    if (g_Failures == 0) {
        std::printf("PyroWave Android render path: all checks passed\n");
    }
    return g_Failures ? 1 : 0;
}
