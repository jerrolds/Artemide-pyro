package com.limelight.binding.video;

import android.content.Context;
import android.net.TrafficStats;
import android.os.Process;

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
        LimeLog.info("PyroWave: " + width + "x" + height + "@" + frameRate +
                (fullRange ? " full range" : " limited range") + ", " + MoonBridge.getPyroWaveStatus());
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

        // Bandwidth is app traffic (video + audio + control) over the window
        String bandwidth = null;
        if (TrafficStatsHelper.getPackageRxBytes(Process.myUid()) != TrafficStats.UNSUPPORTED) {
            long netBytes = TrafficStatsHelper.getPackageRxBytes(Process.myUid()) + TrafficStatsHelper.getPackageTxBytes(Process.myUid());
            if (lastNetBytes != 0) {
                double mbps = (netBytes - lastNetBytes) * 8 / seconds / 1e6;
                bandwidth = String.format(Locale.ROOT, "%.1f Mbps", mbps);
            }
            lastNetBytes = netBytes;
        }

        long[] host = new long[4];
        MoonBridge.pyroWaveGetHostLatency(host);
        long hostFrames = host[1] - lastHostLatencyCount;
        float hostAvg = hostFrames > 0 ? (float) (host[0] - lastHostLatencySum) / hostFrames : 0;
        lastHostLatencySum = host[0];
        lastHostLatencyCount = host[1];

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
            sb.append(context.getString(R.string.perf_overlay_streamdetails, width + "x" + height, presentedFps)).append('\n');
            sb.append(context.getString(R.string.perf_overlay_decoder, "PyroWave (Vulkan, " + path + " path)")).append('\n');
            sb.append(context.getString(R.string.perf_overlay_pyrowave_output,
                    cur[MoonBridge.PYROWAVE_STAT_OUTPUT_WIDTH] + "x" + cur[MoonBridge.PYROWAVE_STAT_OUTPUT_HEIGHT], mode)).append('\n');
            sb.append(context.getString(R.string.perf_overlay_incomingfps, receivedFps)).append('\n');
            sb.append(context.getString(R.string.perf_overlay_renderingfps, presentedFps)).append('\n');
            sb.append(context.getString(R.string.perf_overlay_pyrowave_frames, replaced, rejected, partial)).append('\n');
            if (bandwidth != null) {
                sb.append(context.getString(R.string.perf_overlay_lite_bandwidth)).append(": ").append(bandwidth).append('\n');
            }
            sb.append(context.getString(R.string.perf_overlay_netlatency, (int) (rttInfo >> 32), (int) rttInfo)).append('\n');
            if (hostFrames > 0 && host[2] != 0xFFFFFFFFL) {
                sb.append(context.getString(R.string.perf_overlay_hostprocessinglatency,
                        host[2] / 10f, host[3] / 10f, hostAvg / 10f)).append('\n');
            }
            sb.append(context.getString(R.string.perf_overlay_pyrowave_cputime, decodeMs, presentMs));
        }

        if (prefs.enablePerfLogging) {
            LimeLog.info(String.format(Locale.ROOT,
                    "PyroWave: %.1f fps in, %.1f fps presented, %d replaced, %d rejected, %d partial, " +
                            "CPU %.2f ms decode + %.2f ms present, output %dx%d %s",
                    receivedFps, presentedFps, replaced, rejected, partial, decodeMs, presentMs,
                    cur[MoonBridge.PYROWAVE_STAT_OUTPUT_WIDTH], cur[MoonBridge.PYROWAVE_STAT_OUTPUT_HEIGHT], mode));
        }
        return sb.toString();
    }
}
