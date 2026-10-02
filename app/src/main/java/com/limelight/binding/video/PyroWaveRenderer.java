package com.limelight.binding.video;

import android.content.Context;
import android.net.TrafficStats;
import android.os.Process;
import android.view.Display;
import android.view.WindowManager;

import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.nvstream.jni.MoonBridge;
import com.limelight.preferences.PreferenceConfiguration;
import com.limelight.utils.TrafficStatsHelper;

import java.util.Locale;

// Java side of the native PyroWave renderer (app/src/main/jni/pyrowave). Frames
// never pass through here: moonlight-core's callbacks.c hands them to the
// native renderer, which decodes and presents them with Vulkan on the stream
// view's surface. This class only drives its lifecycle and reports its stats
// to the performance overlay.
//
// MediaCodecDecoderRenderer owns an instance while a PyroWave stream is active.
public class PyroWaveRenderer {
    private final Context context;
    private final PreferenceConfiguration prefs;
    private final PerfOverlayListener perfListener;
    // Previous-window state for the overlay, only touched by the stats thread
    private long lastHostLatencySum, lastHostLatencyCount, lastNetBytes;

    private int width;
    private int height;
    // The negotiated format; whether the screen shows 10-bit as HDR is a stat
    private boolean tenBit;
    private boolean chroma444;
    private boolean fullRange;
    // One refresh period of the display: the estimate for the last step to the screen
    private float displayPeriodMs = 1000f / 60f;

    private Thread statsThread;
    private volatile boolean statsRunning;

    public PyroWaveRenderer(Context context, PreferenceConfiguration prefs, PerfOverlayListener perfListener) {
        this.context = context;
        this.prefs = prefs;
        this.perfListener = perfListener;
    }

    public static boolean isAvailable() {
        return MoonBridge.isPyroWaveAvailable();
    }

    public int setup(int videoFormat, int width, int height, int frameRate, boolean fullRange) {
        this.width = width;
        this.height = height;
        this.tenBit = (videoFormat & MoonBridge.VIDEO_FORMAT_MASK_10BIT) != 0;
        this.chroma444 = (videoFormat & (MoonBridge.VIDEO_FORMAT_PYROWAVE_444 | MoonBridge.VIDEO_FORMAT_PYROWAVE_HDR10_444)) != 0;
        this.fullRange = fullRange;
        try {
            WindowManager windowManager = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
            Display display = windowManager != null ? windowManager.getDefaultDisplay() : null;
            if (display != null && display.getRefreshRate() > 1f) {
                displayPeriodMs = 1000f / display.getRefreshRate();
            }
        } catch (RuntimeException e) {
            // Keep the default
        }
        LimeLog.info("PyroWave: " + width + "x" + height + "@" + frameRate +
                (fullRange ? " full range" : " limited range") + (tenBit ? ", 10-bit HDR10" : ", 8-bit") +
                ", " + MoonBridge.getPyroWaveStatus());
        MoonBridge.pyroWaveSetKeepWarm(prefs.pyroWaveKeepWarm, Math.round(1000f / displayPeriodMs));
        MoonBridge.pyroWaveSetPacing(prefs.pyroWaveJitPacing);
        MoonBridge.pyroWaveSetPreParse(prefs.pyroWavePreParse);
        return MoonBridge.pyroWaveSetup(videoFormat, width, height, frameRate, fullRange);
    }

    public void start() {
        MoonBridge.pyroWaveStart();
        // Always run: the overlay can be toggled mid-stream, and the loop checks the flag each tick
        statsRunning = true;
        statsThread = new Thread(this::statsLoop, "PyroWave stats");
        statsThread.start();
    }

    public void stop() {
        statsRunning = false;
        if (statsThread != null) {
            statsThread.interrupt();
            try {
                statsThread.join();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            statsThread = null;
        }
        MoonBridge.pyroWaveStop();
    }

    public void cleanup() {
        MoonBridge.pyroWaveCleanup();
    }

    // Reports the last second's counters, like MediaCodecDecoderRenderer's stats window
    private void statsLoop() {
        long[] previous = new long[MoonBridge.PYROWAVE_STAT_COUNT];
        long[] current = new long[MoonBridge.PYROWAVE_STAT_COUNT];
        long previousTime = System.nanoTime();
        MoonBridge.pyroWaveGetStats(previous);

        while (statsRunning) {
            try {
                Thread.sleep(1000);
            } catch (InterruptedException e) {
                return;
            }

            MoonBridge.pyroWaveGetStats(current);
            long now = System.nanoTime();
            double seconds = (now - previousTime) / 1e9;

            String text = formatStats(previous, current, seconds);
            if (prefs.enablePerfOverlay && perfListener != null) {
                perfListener.onPerfUpdate(text);
            }

            long[] swap = previous;
            previous = current;
            current = swap;
            previousTime = now;
        }
    }

    private String formatStats(long[] prev, long[] cur, double seconds) {
        long received = cur[MoonBridge.PYROWAVE_STAT_RECEIVED] - prev[MoonBridge.PYROWAVE_STAT_RECEIVED];
        long replaced = cur[MoonBridge.PYROWAVE_STAT_REPLACED] - prev[MoonBridge.PYROWAVE_STAT_REPLACED];
        long rejected = cur[MoonBridge.PYROWAVE_STAT_REJECTED] - prev[MoonBridge.PYROWAVE_STAT_REJECTED];
        long partial = cur[MoonBridge.PYROWAVE_STAT_PARTIAL] - prev[MoonBridge.PYROWAVE_STAT_PARTIAL];
        long decoded = cur[MoonBridge.PYROWAVE_STAT_DECODED] - prev[MoonBridge.PYROWAVE_STAT_DECODED];
        long presented = cur[MoonBridge.PYROWAVE_STAT_PRESENTED] - prev[MoonBridge.PYROWAVE_STAT_PRESENTED];
        long decodeUs = cur[MoonBridge.PYROWAVE_STAT_DECODE_US] - prev[MoonBridge.PYROWAVE_STAT_DECODE_US];
        long presentUs = cur[MoonBridge.PYROWAVE_STAT_PRESENT_US] - prev[MoonBridge.PYROWAVE_STAT_PRESENT_US];

        float receivedFps = (float) (received / seconds);
        float presentedFps = (float) (presented / seconds);
        float decodeMs = decoded > 0 ? decodeUs / 1000f / decoded : 0;
        float presentMs = presented > 0 ? presentUs / 1000f / presented : 0;
        long rttInfo = MoonBridge.getEstimatedRttInfo();

        String path = cur[MoonBridge.PYROWAVE_STAT_FRAGMENT_PATH] != 0 ? "fragment" : "compute";
        String mode = cur[MoonBridge.PYROWAVE_STAT_MAILBOX] != 0 ? "mailbox" : "FIFO";
        // What the screen is really showing: HDR10 output, or a 10-bit stream shown in SDR
        if (cur[MoonBridge.PYROWAVE_STAT_HDR] != 0) {
            mode += ", HDR10";
        }
        else if (tenBit) {
            mode += ", 10-bit SDR";
        }

        // Bandwidth is app traffic (video + audio + control) over the window
        String bandwidth = null;
        if (TrafficStatsHelper.getPackageRxBytes(Process.myUid()) != TrafficStats.UNSUPPORTED) {
            long netBytes = TrafficStatsHelper.getPackageRxBytes(Process.myUid()) + TrafficStatsHelper.getPackageTxBytes(Process.myUid());
            if (lastNetBytes != 0) {
                // Formatted like the HEVC/AV1 overlay: K/s below 1000, M/s above
                float kbPerSecond = (float) ((netBytes - lastNetBytes) / 1024.0 / seconds);
                bandwidth = kbPerSecond >= 1000
                        ? String.format(Locale.ROOT, "%.2fM/s", kbPerSecond / 1024f)
                        : String.format(Locale.ROOT, "%.2fK/s", kbPerSecond);
            }
            lastNetBytes = netBytes;
        }

        long[] host = new long[4];
        MoonBridge.pyroWaveGetHostLatency(host);
        long hostFrames = host[1] - lastHostLatencyCount;
        float hostAvg = hostFrames > 0 ? (float) (host[0] - lastHostLatencySum) / hostFrames : 0;
        lastHostLatencySum = host[0];
        lastHostLatencyCount = host[1];

        // Detail over the window: what the host sends, how evenly it arrives and what the GPU spends
        long bytes = cur[MoonBridge.PYROWAVE_STAT_BYTES] - prev[MoonBridge.PYROWAVE_STAT_BYTES];
        long packets = cur[MoonBridge.PYROWAVE_STAT_PACKETS] - prev[MoonBridge.PYROWAVE_STAT_PACKETS];
        long lostPackets = cur[MoonBridge.PYROWAVE_STAT_PACKETS_LOST] - prev[MoonBridge.PYROWAVE_STAT_PACKETS_LOST];
        long arrivals = cur[MoonBridge.PYROWAVE_STAT_ARRIVALS] - prev[MoonBridge.PYROWAVE_STAT_ARRIVALS];
        long arrivalSumUs = cur[MoonBridge.PYROWAVE_STAT_ARRIVAL_SUM_US] - prev[MoonBridge.PYROWAVE_STAT_ARRIVAL_SUM_US];
        long arrivalSqUs = cur[MoonBridge.PYROWAVE_STAT_ARRIVAL_SQ_SUM_US] - prev[MoonBridge.PYROWAVE_STAT_ARRIVAL_SQ_SUM_US];
        double gapMeanUs = arrivals > 0 ? (double) arrivalSumUs / arrivals : 0;
        double gapVarianceUs = arrivals > 0 ? (double) arrivalSqUs / arrivals - gapMeanUs * gapMeanUs : 0;
        float gapAvgMs = (float) (gapMeanUs / 1000.0);
        float jitterMs = (float) (Math.sqrt(Math.max(0, gapVarianceUs)) / 1000.0);
        float worstGapMs = cur[MoonBridge.PYROWAVE_STAT_MAX_GAP_US] / 1000f;
        float worstFrameMs = cur[MoonBridge.PYROWAVE_STAT_MAX_FRAME_US] / 1000f;
        float kbPerFrame = received > 0 ? bytes / 1024f / received : 0;
        float videoMbps = (float) (bytes * 8 / seconds / 1e6);
        float bitsPerPixel = (received > 0 && width * height > 0) ? (float) (bytes * 8.0 / received / ((double) width * height)) : 0;
        float lossPercent = packets > 0 ? 100f * lostPackets / packets : 0;
        long gpuSamples = cur[MoonBridge.PYROWAVE_STAT_GPU_SAMPLES] - prev[MoonBridge.PYROWAVE_STAT_GPU_SAMPLES];
        float gpuDecodeMs = gpuSamples > 0 ?
                (cur[MoonBridge.PYROWAVE_STAT_GPU_DECODE_US] - prev[MoonBridge.PYROWAVE_STAT_GPU_DECODE_US]) / 1000f / gpuSamples : 0;
        float gpuConvertMs = gpuSamples > 0 ?
                (cur[MoonBridge.PYROWAVE_STAT_GPU_CONVERT_US] - prev[MoonBridge.PYROWAVE_STAT_GPU_CONVERT_US]) / 1000f / gpuSamples : 0;
        boolean gpuTiming = cur[MoonBridge.PYROWAVE_STAT_GPU_TIMING] != 0;

        // Host to screen. Measured: the host's own processing time, how long the frame took to
        // arrive, and assembled -> GPU done (the GPU clock is placed on the library's clock).
        // Estimated: the one-way network trip as half the round trip, and the last step to the
        // panel as one refresh period (wait for the next refresh plus scan-out). Not covered:
        // anything before the host captures the frame, and input latency.
        float assemblyMs = received > 0 ?
                (float) (cur[MoonBridge.PYROWAVE_STAT_ASSEMBLY_SUM_MS] - prev[MoonBridge.PYROWAVE_STAT_ASSEMBLY_SUM_MS]) / received : 0;
        boolean clientTiming = cur[MoonBridge.PYROWAVE_STAT_CLIENT_TIMING] != 0;
        long clientSamples = cur[MoonBridge.PYROWAVE_STAT_CLIENT_SAMPLES] - prev[MoonBridge.PYROWAVE_STAT_CLIENT_SAMPLES];
        float clientAvgMs = clientSamples > 0 ?
                (cur[MoonBridge.PYROWAVE_STAT_CLIENT_SUM_US] - prev[MoonBridge.PYROWAVE_STAT_CLIENT_SUM_US]) / 1000f / clientSamples : 0;
        float clientMaxMs = cur[MoonBridge.PYROWAVE_STAT_CLIENT_MAX_US] / 1000f;
        float hostMs = hostFrames > 0 ? hostAvg / 10f : 0;
        float networkMs = (rttInfo >> 32) / 2f;
        float displayMs = displayPeriodMs;
        float endToEndMs = hostMs + networkMs + assemblyMs + clientAvgMs + displayMs;

        boolean hdrOutput = cur[MoonBridge.PYROWAVE_STAT_HDR] != 0;
        String format = (tenBit ? "10-bit" : "8-bit") + (chroma444 ? " 4:4:4" : " 4:2:0") +
                (fullRange ? ", full range" : ", limited range") + ", " +
                colorspaceName(cur[MoonBridge.PYROWAVE_STAT_COLORSPACE]) +
                (hdrOutput ? ", HDR10 (PQ) output" : ", SDR output");
        String hint;
        switch ((int) cur[MoonBridge.PYROWAVE_STAT_HINT_KIND]) {
            case 2: hint = "graphics pipeline"; break;
            case 1: hint = "plain session"; break;
            default: hint = "none"; break;
        }

        StringBuilder sb = new StringBuilder();
        if (prefs.enablePerfOverlayLite) {
            sb.append("PyroWave\t FPS: ").append(context.getString(R.string.perf_overlay_lite_fps, presentedFps));
            sb.append("\t ").append(context.getString(R.string.perf_overlay_lite_net, (int) (rttInfo >> 32)));
            sb.append("\t ").append(context.getString(R.string.perf_overlay_pyrowave_lite_drops, replaced + rejected));
            if (bandwidth != null) {
                sb.append("\t ").append(context.getString(R.string.perf_overlay_lite_bandwidth)).append(": ").append(bandwidth);
            }
        }
        else {
            // The block the HEVC/AV1 overlay shows, in the same order and wording, so the codecs can be
            // read side by side. Everything else about the stream goes to the performance log.
            sb.append(context.getString(R.string.perf_overlay_streamdetails, width + "x" + height, presentedFps)).append('\n');
            sb.append(context.getString(R.string.perf_overlay_decoder, "PyroWave (Vulkan, " + path + " path)")).append('\n');
            sb.append(context.getString(R.string.perf_overlay_incomingfps, receivedFps)).append('\n');
            sb.append(context.getString(R.string.perf_overlay_renderingfps, presentedFps)).append('\n');
            // Frames the host sent that could not be decoded at all
            float droppedPercent = received > 0 ? 100f * rejected / received : 0f;
            sb.append(context.getString(R.string.perf_overlay_netdrops, droppedPercent)).append('\n');
            if (bandwidth != null) {
                sb.append(context.getString(R.string.perf_overlay_lite_bandwidth)).append(": ").append(bandwidth).append('\n');
            }
            sb.append(context.getString(R.string.perf_overlay_netlatency, (int) (rttInfo >> 32), (int) rttInfo)).append('\n');
            if (hostFrames > 0 && host[2] != 0xFFFFFFFFL) {
                sb.append(context.getString(R.string.perf_overlay_hostprocessinglatency,
                        host[2] / 10f, host[3] / 10f, hostAvg / 10f)).append('\n');
            }
            // Average decoding time is the GPU decode stage; Delay runs from the frame being assembled
            // to the GPU finishing it (the HEVC/AV1 figure ends when the frame is released to the display)
            if (gpuTiming && gpuSamples > 0) {
                sb.append(context.getString(R.string.perf_overlay_dectime, gpuDecodeMs));
                if (clientTiming && clientSamples > 0) {
                    sb.append(context.getString(R.string.perf_overlay_lite_e2e, clientAvgMs));
                }
            }
            else if (gpuTiming) {
                sb.append(context.getString(R.string.perf_overlay_pyrowave_gputime_waiting));
            }
            else {
                sb.append(context.getString(R.string.perf_overlay_pyrowave_gputime_unavailable));
            }

            // What only PyroWave has: its format and size, the GPU split, and the frames it had to skip
            sb.append('\n').append(context.getString(R.string.perf_overlay_pyrowave_summary, format, bitsPerPixel, kbPerFrame)).append('\n');
            if (gpuTiming && gpuSamples > 0) {
                sb.append(context.getString(R.string.perf_overlay_pyrowave_gputime, gpuDecodeMs, gpuConvertMs, gpuSamples)).append('\n');
            }
            sb.append(context.getString(R.string.perf_overlay_pyrowave_frames, replaced, rejected, partial));
            if (prefs.pyroWaveKeepWarm) {
                float warmPerSecond = (float) ((cur[MoonBridge.PYROWAVE_STAT_WARM_FRAMES] - prev[MoonBridge.PYROWAVE_STAT_WARM_FRAMES]) / seconds);
                sb.append('\n').append(context.getString(R.string.perf_overlay_pyrowave_warm, warmPerSecond));
            }

            // The original PyroWave detail, for comparing with the block above: what the stream is, how the
            // pipeline is set up, where the time goes on the CPU and GPU, and the estimated total
            if (prefs.pyroWaveDetailedOverlay) {
                sb.append('\n').append(context.getString(R.string.perf_overlay_pyrowave_detail_header)).append('\n');
                sb.append(context.getString(R.string.perf_overlay_pyrowave_format, format)).append('\n');
                sb.append(context.getString(R.string.perf_overlay_pyrowave_gpu, MoonBridge.getPyroWaveStatus())).append('\n');
                sb.append(context.getString(R.string.perf_overlay_pyrowave_output,
                        cur[MoonBridge.PYROWAVE_STAT_OUTPUT_WIDTH] + "x" + cur[MoonBridge.PYROWAVE_STAT_OUTPUT_HEIGHT], mode)).append('\n');
                sb.append(context.getString(R.string.perf_overlay_pyrowave_pipeline,
                        (int) cur[MoonBridge.PYROWAVE_STAT_FRAMES_IN_FLIGHT], (int) cur[MoonBridge.PYROWAVE_STAT_SURFACES],
                        (int) cur[MoonBridge.PYROWAVE_STAT_SWAPCHAIN_IMAGES],
                        swapchainFormatName(cur[MoonBridge.PYROWAVE_STAT_SWAPCHAIN_FORMAT]), hint)).append('\n');
                sb.append(context.getString(R.string.perf_overlay_pyrowave_videodata, kbPerFrame, videoMbps, bitsPerPixel)).append('\n');
                sb.append(context.getString(R.string.perf_overlay_pyrowave_packets, packets, lostPackets, lossPercent)).append('\n');
                sb.append(context.getString(R.string.perf_overlay_pyrowave_arrival, gapAvgMs, jitterMs, worstGapMs)).append('\n');
                sb.append(context.getString(R.string.perf_overlay_pyrowave_cputime, decodeMs, presentMs)).append('\n');
                sb.append(context.getString(R.string.perf_overlay_pyrowave_worstframe, worstFrameMs)).append('\n');
                if (clientTiming && clientSamples > 0) {
                    sb.append(context.getString(R.string.perf_overlay_pyrowave_client, clientAvgMs, clientMaxMs)).append('\n');
                    sb.append(context.getString(R.string.perf_overlay_pyrowave_e2e,
                            endToEndMs, hostMs, networkMs, assemblyMs, clientAvgMs, displayMs));
                }
                else if (clientTiming) {
                    sb.append(context.getString(R.string.perf_overlay_pyrowave_e2e_waiting));
                }
                else {
                    sb.append(context.getString(R.string.perf_overlay_pyrowave_e2e_unavailable));
                }
            }
        }

        if (prefs.enablePerfLogging) {
            LimeLog.info(String.format(Locale.ROOT,
                    "PyroWave: %.1f fps in, %.1f fps presented, %d replaced, %d rejected, %d partial, " +
                            "CPU %.2f ms decode + %.2f ms present, output %dx%d %s",
                    receivedFps, presentedFps, replaced, rejected, partial, decodeMs, presentMs,
                    cur[MoonBridge.PYROWAVE_STAT_OUTPUT_WIDTH], cur[MoonBridge.PYROWAVE_STAT_OUTPUT_HEIGHT], mode));
            // The detail on a second line so that tools reading the first one are unaffected
            LimeLog.info(String.format(Locale.ROOT,
                    "PyroWave detail: %s; GPU %.2f ms decode + %.2f ms convert (%d timed); " +
                            "arrival %.2f ms avg, %.2f ms jitter, %.1f ms worst gap; %.0f KB/frame, %.1f Mbps video, " +
                            "%.2f bits/pixel; packets %d, lost %d (%.2f%%); slowest frame %.1f ms; " +
                            "%d in flight, %d surfaces, %d swapchain images (%s), hint %s; " +
                            "client %.2f ms avg, %.2f ms worst (%d timed); end-to-end %.1f ms = " +
                            "host %.1f + network %.1f + receive %.1f + client %.1f + display %.1f",
                    format, gpuDecodeMs, gpuConvertMs, gpuSamples, gapAvgMs, jitterMs, worstGapMs,
                    kbPerFrame, videoMbps, bitsPerPixel, packets, lostPackets, lossPercent, worstFrameMs,
                    cur[MoonBridge.PYROWAVE_STAT_FRAMES_IN_FLIGHT], cur[MoonBridge.PYROWAVE_STAT_SURFACES],
                    cur[MoonBridge.PYROWAVE_STAT_SWAPCHAIN_IMAGES],
                    swapchainFormatName(cur[MoonBridge.PYROWAVE_STAT_SWAPCHAIN_FORMAT]), hint,
                    clientAvgMs, clientMaxMs, clientSamples,
                    endToEndMs, hostMs, networkMs, assemblyMs, clientAvgMs, displayMs));
        }
        return sb.toString();
    }

    private static String colorspaceName(long colorspace) {
        switch ((int) colorspace) {
            case MoonBridge.COLORSPACE_REC_601: return "Rec.601";
            case MoonBridge.COLORSPACE_REC_709: return "Rec.709";
            case MoonBridge.COLORSPACE_REC_2020: return "Rec.2020";
            default: return "colorspace " + colorspace;
        }
    }

    // VkFormat values of the swapchain formats the renderer can pick
    private static String swapchainFormatName(long vkFormat) {
        switch ((int) vkFormat) {
            case 0: return "no window";
            case 37: return "R8G8B8A8";
            case 44: return "B8G8R8A8";
            case 58: return "A2R10G10B10";
            case 64: return "A2B10G10R10";
            default: return "VkFormat " + vkFormat;
        }
    }
}
