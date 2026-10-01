#pragma once

// C API of the PyroWave renderer, called from Artemide's JNI glue
// (moonlight-core). One stream at a time.
//
// moonlight-common-c's decoder thread hands each frame to
// PwRendererSubmitDecodeUnit(), which copies it and returns at once. A render
// thread decodes the newest frame on the GPU and presents it on a Vulkan
// swapchain on the stream view's window. PyroWave frames are independent, so
// a frame that arrives while the previous one is still waiting replaces it.

#include <Limelight.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ANativeWindow;

typedef struct _PW_RENDERER_STATS {
    uint32_t receivedFrames;  // handed over by moonlight-common-c
    uint32_t replacedFrames;  // superseded by a newer frame before decoding
    uint32_t rejectedFrames;  // failed parsing or could not be decoded
    uint32_t partialFrames;   // decoded despite lost packets (blurred areas)
    uint32_t decodedFrames;
    uint32_t presentedFrames;
    uint32_t noWindowFrames;  // decoded nothing because no window was attached
    uint64_t totalDecodeUs;   // CPU time parsing and submitting decodes
    uint64_t totalPresentUs;  // CPU time acquiring, recording and presenting
    uint32_t outputWidth;     // swapchain size (0 without a window)
    uint32_t outputHeight;
    bool fragmentPath;        // decoder uses PyroWave's mobile (fragment shader) path
    bool mailbox;             // swapchain presents in mailbox mode (else FIFO)
    bool hdr;                 // swapchain is HDR10 (10-bit, PQ)

    // Detail for the overlay. Totals are cumulative like the counters above; the two
    // peaks (maxArrivalGapUs, maxFrameUs) cover the time since the previous call.
    uint64_t receivedBytes;   // PyroWave payload handed over, in bytes
    uint64_t packetsTotal;    // RTP packets in those frames
    uint64_t packetsLost;     // of which never arrived (after error correction)
    uint64_t arrivalCount;    // intervals between frames
    uint64_t arrivalSumUs;    // sum of those intervals, each capped at 100 ms
    uint64_t arrivalSqSumUs;  // sum of their squares, for the jitter
    uint32_t maxArrivalGapUs; // longest gap between two frames
    uint32_t maxFrameUs;      // slowest decode-and-present on the render thread (CPU wall time)
    uint32_t colorspace;      // COLORSPACE_* of the latest frame
    uint32_t swapchainFormat; // VkFormat of the swapchain images (0 without a window)
    uint32_t swapchainImages;
    uint32_t hintKind;        // performance hint session: 0 none, 1 plain, 2 graphics pipeline
    uint32_t framesInFlight;
    uint32_t surfaceCount;    // decoded surfaces
    bool gpuTimingEnabled;
    uint64_t gpuDecodeUs;     // GPU time of the decode, summed over the timed frames
    uint64_t gpuConvertUs;    // GPU time of the colour conversion and draw
    uint64_t gpuSamples;      // number of timed frames

    // Latency from the frame being assembled by moonlight-common-c to the GPU finishing it
    uint64_t assemblySumMs;   // first to last packet of each frame, summed over receivedFrames
    bool clientTiming;        // the GPU clock can be placed on CLOCK_MONOTONIC
    uint64_t clientSumUs;     // assembled -> GPU done, summed over clientSamples
    uint64_t clientSamples;
    uint32_t clientMaxUs;     // the largest since the previous call
} PW_RENDERER_STATS;

// Whether this device can decode and present PyroWave (Vulkan 1.3 and the
// features the decoder needs). Probes once and caches the answer; reason
// receives a description when it cannot.
bool PwIsAvailable(char* reason, size_t reasonSize);

// Prepares for a stream. videoFormat is the negotiated VIDEO_FORMAT_PYROWAVE*,
// fullRange the colour range requested from the host. Returns 0 on success.
int PwRendererSetup(int videoFormat, int width, int height, int frameRate, bool fullRange);

// Attaches the window to present to (the renderer takes its own reference),
// or detaches it with NULL. May be called at any time, before or after setup;
// on return the renderer no longer uses the previous window.
void PwRendererSetWindow(struct ANativeWindow* window);

void PwRendererStart(void);
void PwRendererStop(void);
void PwRendererCleanup(void);

// DECODER_RENDERER_CALLBACKS.submitDecodeUnit for PyroWave streams
int PwRendererSubmitDecodeUnit(PDECODE_UNIT decodeUnit);

void PwRendererGetStats(PW_RENDERER_STATS* stats);

#ifdef __cplusplus
}
#endif
