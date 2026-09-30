#pragma once

// Decodes PyroWave frames into a small ring of output surfaces on the shared
// Vulkan device. Adapted from moonlight-qt's PyroWaveDecoder: the framing,
// the decode sequence and the partial-frame policy are the same; the D3D11
// surface and fence import is replaced by images and a timeline semaphore
// created directly on the device the renderer presents with.
//
// Not thread safe: call everything from one thread.

#include "pyrowaveframing.h"
#include "pw_vulkan.h"

#include <array>
#include <string>
#include <vector>

struct pyrowave_device_opaque;
struct pyrowave_decoder_opaque;

class PwDecoder {
public:
    struct Config {
        int width = 0;
        int height = 0;
        bool chroma444 = false;
        bool tenBit = false;
    };

    struct Surface {
        std::array<VkImage, 3> images {};
        std::array<VkDeviceMemory, 3> memory {};
        std::array<VkImageView, 3> views {};
        std::array<VkExtent2D, 3> extents {};
    };

    PwDecoder() = default;
    ~PwDecoder();

    PwDecoder(const PwDecoder&) = delete;
    PwDecoder& operator=(const PwDecoder&) = delete;

    bool initialize(PwVulkan& vulkan, const Config& config, int surfaceCount);

    // Parses and decodes one frame into the given surface. The GPU waits for
    // waitSemaphore to reach waitValue (the renderer's last read of that
    // surface) before overwriting it; pass VK_NULL_HANDLE for no wait. The
    // work is only submitted: it signals decodeSemaphore() with the returned
    // decodeValue. packets maps the frame's RTP packets and which were lost
    // (empty for a frame that arrived whole); criticalPackets is the host's
    // count of packets holding the coarsest wavelet level (0 if unknown).
    bool decode(const uint8_t* data, size_t size,
                const std::vector<PyroWaveFraming::Segment>& packets, size_t criticalPackets,
                int surface, VkSemaphore waitSemaphore, uint64_t waitValue, uint64_t& decodeValue);

    VkSemaphore decodeSemaphore() const { return m_DecodeSemaphore; }
    const Surface& surface(int index) const { return m_Surfaces[index]; }
    int surfaceCount() const { return int(m_Surfaces.size()); }
    const Config& config() const { return m_Config; }
    bool fragmentPath() const { return m_FragmentPath; }

    // Why the last call failed, for logging.
    const std::string& lastError() const { return m_LastError; }

    // Whether the last decoded frame was missing records
    bool lastFramePartial() const { return m_LastFramePartial; }

    // Framing seen in the most recent successfully parsed frame.
    PyroWaveFraming::Framing lastFraming() const { return m_LastFraming; }

private:
    bool createSurface(Surface& surface);
    void destroySurface(Surface& surface);

    PwVulkan* m_Vulkan = nullptr;
    Config m_Config;
    PyroWaveFraming::StreamGeometry m_Geometry {};
    bool m_FragmentPath = false;

    pyrowave_device_opaque* m_Device = nullptr;
    pyrowave_decoder_opaque* m_Decoder = nullptr;
    VkSemaphore m_DecodeSemaphore = VK_NULL_HANDLE;
    uint64_t m_DecodeValue = 0;
    std::vector<Surface> m_Surfaces;

    PyroWaveFraming::Frame m_Parsed;
    std::string m_LastError;
    PyroWaveFraming::Framing m_LastFraming = PyroWaveFraming::Framing::Records;
    bool m_LastFramePartial = false;
};
