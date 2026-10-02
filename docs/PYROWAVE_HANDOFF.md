# PyroWave on Artemide: handoff (2026-10-01)

For an agent picking up the PyroWave work on this repo. It separates what was
**measured on a device** from what is **only compiled**, because the tablet was
disconnected for the last stretch of work and several features have never run
(HDR10, the detailed overlay, GPU timing, client and end-to-end latency).

Read `docs/PYROWAVE_DECODE_PATH.md` first for the design (how the native renderer
maps onto Moonlight-Qt's). This file covers what happened after it.

Revision note: written at `fd537a0c`, then reviewed and corrected after `695c576d`
(a later session, see section 3) changed the frame queue, the default bitrate and the
game-mode settings. Where this file and the code disagree, the code wins.

## 1. Status in one screen

- PyroWave streaming works end to end on a Galaxy Tab S9 Ultra against a Windows
  host. Branch `ccr-ea2408c2-mx41mg`, pushed; HEAD was `695c576d` when this was reviewed.
- **Stutter was Wi-Fi loss from far too high a bitrate**, not the tablet. Setting a bitrate of
  about 100 Mbps fixed it (verified, section 5.1). `695c576d` now also caps the *default*
  PyroWave bitrate at 120 Mbps on Wi-Fi; that cap is derived from the same measurements but the
  commit does not say it was run on a device.
- **The remaining limit is the Adreno GPU clock**: fps tracks the clock almost step
  for step, and Android's governor leaves it low (verified, section 5.2). A
  performance-hint session was added to ask for more clock. Version 1 only delayed
  the drop. **Version 2 (tighter target, repeated hints) was installed but never
  evaluated.** `695c576d` adds further attempts (game category and game mode, sustained
  performance mode on Android 12+, render thread priority); their effect is not recorded.
- Added but **never run on a device**: 10-bit / HDR10 output, the detailed overlay
  with GPU timing, and client / end-to-end latency (section 6).
- Nothing here has been tested on the other two intended devices (Poco F6 Pro,
  Galaxy Tab S8 Ultra).

## 2. Environment

| Thing | Detail |
|---|---|
| Dev machine | Windows 11, repo `C:\Users\jerro\Development\Artemide-pyro`, PowerShell 5.1 and Git Bash |
| Remote / branch | `origin` = `github.com/jerrolds/Artemide-pyro`; work branch `ccr-ea2408c2-mx41mg`; main branch `moonlight-noir`. The user has authorised every push so far to the work branch. |
| Tablet | Galaxy Tab S9 Ultra `SM-X910`, Android 16, One UI 8.0.5, Snapdragon 8 Gen 2, Adreno 740, panel 2960x1848 at 120 Hz (modes 120/60/30 only) |
| Host | Windows PC `Jerrold-PC`, 192.168.2.161. The host software was never identified: it speaks the Sunshine protocol with a PyroWave extension, the launch request carries `virtualDisplay=1`, and the 503 error text talks about streamed virtual displays, which suggests a Sunshine fork with virtual-display support (inferred). Other hosts seen: THEATER-PC 192.168.2.130, CONSOLE 192.168.2.20. |
| Network | 5 GHz Wi-Fi 6, tablet RSSI -50 to -67 dBm, receive rate 288-1080 Mbps, transmit rate often far lower |
| Other targets | Poco F6 Pro (8 Gen 2, same GPU) and Galaxy Tab S8 Ultra (8 Gen 1, Adreno 730, same 2960x1848 panel). Same arm64 APK; untested. |

Wireless adb: the port **changes** on every reconnect (35467, then 41459, later gone).
Run `adb devices -l`; if empty, `adb connect <ip>:<port from the Wireless debugging screen>`.

## 3. Commits (newest first)

| Commit | What |
|---|---|
| `695c576d` | By a later session (the user, co-authored by Claude), after this handoff: default PyroWave bitrate capped at 120 Mbps on Wi-Fi (`PYROWAVE_WIFI_DEFAULT_KBPS`, `isOnWifi()` in `Game.java`, with a toast and log line); `android:appCategory="game"` and `supportsPerformanceGameMode="true"`; `setSustainedPerformanceMode` moved out of the Android 11 branch so it runs on Android 12+ when `isSustainedPerformanceModeSupported()`; **queue reverted to 3 surfaces, 2 in flight, 3 swapchain images**; `waitForFreeSlot()` before taking a frame so the newest is decoded; render thread `setpriority(-8)` |
| `fd537a0c` | This handoff |
| `39f43a6f` | Client latency (assembled to GPU done) and an end-to-end estimate in the overlay |
| `ee6b31b3` | 10-bit HDR10 output, and the detailed stats overlay with GPU timing |
| `5d227203` | Performance-hint session; also frames in flight 2 to 3, surfaces 3 to 4, swapchain images 3 to 4 (**those three queue sizes were reverted by `695c576d`**) |
| `372432e1` | Overlay: bandwidth and host processing latency (the user's own work, committed as-is). It equals the working tree at the start of the session (`4b53647b` plus those three uncommitted files), so it is the best reconstruction of "the build that worked the night before"; that equivalence is inferred, not proven. |
| `4b53647b` | Wire the native renderer into the app |
| `ab3e5e92` | Vendor the codec and add the native decoder/renderer. **PyroWave is not selectable in the app at this commit.** |

`372432e1` and `5d227203` have an invisible BOM character at the start of the commit
subject (a PowerShell here-string artefact). Fixing it needs a force-push, so it was left.
Write commit messages to a file and use `git commit -F file`; do not pipe a PowerShell
here-string.

Another remote branch, `origin/sync/derflacco-async`, exists with rewritten copies of the
commits above (different hashes, for example `aa43a879` is this handoff) plus unrelated work
("pyroart: latency options, PyroWave bitrate slider, optimized release build"). It has
diverged a long way from this branch (436 commits there, 40 here as of the fetch). It was only
looked at, not merged; ask the user before touching it.

## 4. Building, signing, installing (tool traps included)

- **JDK**: Gradle 8.13 with AGP 8.12 fails on the default JDK 25 and on Android
  Studio's JBR (also 25) with `Type T not present`. Use JDK 21:
  `-Dorg.gradle.java.home=C:\Users\jerro\.jdks\jbr-21.0.11`.
- **Task**: `.\gradlew.bat :app:assembleNonRoot_gameRelease`. The release type has no
  signing config, so the output is unsigned. Sign with build-tools 36.1.0:
  `zipalign -p -f 4 in.apk aligned.apk`, then `apksigner sign --ks %USERPROFILE%\.android\debug.keystore
  --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey --out out.apk aligned.apk`.
- **Fast arm64-only build**: add `-Pandroid.injected.build.abi=arm64-v8a` (1-2 minutes,
  output under `app\build\intermediates\apk\nonRoot_game\release\`). It **marks the APK
  test-only**: install with `adb install -r -t`, and it cannot be installed from a file
  manager. For anything to hand to a person, build without it; the four APKs then appear in
  `app\build\outputs\apk\nonRoot_game\release\`.
- **Packages**: release = `com.limelight.noir` ("Artemis"); debug = `com.limelight.noirdebug`
  ("Diana"). They install side by side with separate settings and pairing. `adb shell run-as`
  only works for the debug package, so release settings must be changed in the UI.
- **Windows 260-character limit**: the repo has 130-character relative paths. To build another
  commit use a short folder and
  `git -c core.longpaths=true worktree add --detach C:\Users\jerro\w\x <rev>`. The submodule
  `moonlight-common-c` is at the same commit throughout; copy the checked-out folder in
  instead of initialising it (no network). Copy `local.properties` too. Remove the worktree
  afterwards with `git worktree remove --force`.
- **Built APKs** live in `F:\test\Artemis-release-arm64-<hash>.apk` for `39f43a6f`,
  `372432e1` and `ab3e5e92`, all signed with the debug key and not test-only. `F:\test` also
  holds unrelated projects: only add files, never delete there.
- **Shell quirks**: the PowerShell tool shows native stderr as `NativeCommandError` even on
  success; put `$(...)` inside single-quoted strings when passing commands to `adb shell`; the
  tool's safety filter once misread the literal text `lib/*` near `Remove-Item`, so avoid
  `Remove-Item` in the same command as such strings.

## 5. Verified findings (measured on the tablet)

### 5.1 Stutter was network loss from the default bitrate
- With PyroWave and an unchanged bitrate, `Game.java` replaces it with
  `getDefaultPyroWaveBitrate` = 1.6 bits per pixel x width x height x fps, clamped to
  20-900 Mbps: about 900 Mbps at 2960x1848x120, 398 Mbps at 1080p120, 199 Mbps at 1080p60.
  That was the whole problem. Since `695c576d`, `Game.java` also caps this default at
  `PYROWAVE_WIFI_DEFAULT_KBPS` (120 Mbps) when the active network is Wi-Fi (`isOnWifi()`;
  Ethernet is not capped) and shows a toast and a log line `PyroWave default bitrate: ...`.
  `bitrateIsDefault` is true when the saved value equals the default for the current
  resolution and fps, in which case a user value is ignored, so never set exactly the default.
- The real rate follows the picture: about 57 Mbps on a static desktop, 220-500+ Mbps on video
  or a game.
- Symptoms: moonlight-common-c logs `Unrecoverable frame`, `Network dropped`, `Delivering
  frame ... without N of M data packets`; PyroWave logs `Dropped frame ...`; overlay shows
  hundreds to thousands of undecodable frames per 10 s. The tablet's UDP receive buffer was not
  the cause (`/proc/net/snmp` `RcvbufErrors` went from 0 to only 14 over the whole session, while
  the loss was thousands of frames); packets were lost before reaching the tablet.
- A bitrate of about 100 Mbps (saved as 108,500 kbps) gave zero undecodable frames at 2960x1848
  while a game ran, and 162-230 Mbps on the wire. The on-the-wire rate is **about twice the
  nominal setting** (error correction and overshoot; not investigated further). The link carried
  about 200 Mbps cleanly and fell apart above about 250.

### 5.2 fps is set by the Adreno clock, and the governor leaves it low
Time-aligned samples (GPU `cur_freq` against the app's per-second perf log line):

| GPU clock | Presented fps |
|---|---|
| 220 MHz | about 75 |
| 295 MHz | about 91 |
| 401 MHz | about 105 |
| 475 MHz | about 117-129 |

- A fresh stream settles at 475 MHz (120+ fps). After a quiet spell (alt-tab to the desktop, a
  static scene) the governor drops to 220 MHz and, when the load returns, climbs only to
  295 MHz at about 85% busy and stays: fps falls to about 90. Reconnecting restores it.
- Not thermal: `dumpsys thermalservice` status 0; GPU zones 48-50 C; `kgsl` `throttling=0`;
  `thermal_pwrlevel=1` is a 680 MHz cap, above the clocks in use. Governor is `msm-adreno-tz`;
  levels 719/680/615/550/475/401/348/295/220/124.8 MHz.
- The render loop is saturated whenever the host sends more frames than the tablet can show:
  presented fps is about 1000 / (decode + present) in ms. The extra frames are the
  "superseded" count (a received frame replaced in the single pending slot before decoding).
- The host was sending 170-235 fps for a 120 Hz panel. Capping the game at 120 fps on the PC
  would save about 30% of the bandwidth; this was suggested, not tested.

### 5.3 What the overlay numbers mean
- **CPU decode** is the CPU time to parse, push packets and record/submit the decode (about
  1.6 ms in the release build at 2960x1848; 15.8 ms in the very first overlay photo, which was a
  debug build at a much higher bitrate). The decode itself runs later on the GPU.
- **Present** is CPU wall time in `present()`, which blocks (`UINT64_MAX`) on the previous
  frame's GPU work and on a free swapchain image; it is mostly GPU wait, not CPU work.
- Neither equals HEVC's "Average decoding time", which is MediaCodec enqueue-to-dequeue
  latency. The new "GPU time: decode" line is the closest equivalent (untested, section 6).
- Debug versus release: the CPU decode figure dropped from 15.8 ms to about 1.6 ms when the user
  moved to the release build, but the build type, the bitrate and the content all changed at once,
  so how much of that is the build type is **unproven**. The expectation that `ndk-build` compiles a
  debuggable variant without optimisation was never confirmed (neither `Application.mk` nor
  `build.gradle` sets `APP_OPTIM`; the compiler flags actually used were not inspected). Profile
  release builds, and if the attribution matters, compare two builds with the same bitrate and content.

### 5.4 Other observations
- `Too much pending audio data: 40-65 ms` bursts (the audio queue drops packets above 40 ms)
  occur a few times a minute at a stable sub-second phase; cause not found. Not video loss.
- One launch attempt failed with `Resume failed: 503 "client display is not yet capture-ready"`,
  which looks like a host-side virtual-display state problem (the host answered the HTTP request;
  nothing on the tablet was wrong). A later launch started normally. Not investigated.
- Samsung settings found: `sem_enhanced_cpu_responsiveness=0` ("Enhanced processing" in
  Settings, off); Game Booster / `gametools` is installed. At the time the app was not registered as
  a game; `695c576d` has since added `android:appCategory="game"` and Game Mode support, which
  has not been evaluated here. Enhanced processing was never tried.

## 6. Features added and their state

| Feature | Where | State |
|---|---|---|
| Performance hint session (graphics pipeline on Android 16, plain on 13-15, none below 13), reports CPU/GPU time per frame; v2 uses a 7.5 ms target and re-sends the workload-increase notice while frames miss it | `PerfHint` in `pyrowave_renderer.cpp` | v1 measured: held 120+ for about 3 minutes instead of 1-2, then still fell to about 75 fps at 295 MHz. **v2 untested.** |
| Frame queue depth | `k_FramesInFlight`, `k_SurfaceCount`, `pw_swapchain.cpp`, `waitForFreeSlot()` | Tried 3 in flight / 4 surfaces / 4 images: measured no change to the clock or fps. **Reverted by `695c576d` to 2 / 3 / 3**, which also waits for a free slot before taking a frame (so the decoded frame is the newest) and sets the render thread to `setpriority(-8)`. The shallower queue is unmeasured; it should cost less latency. |
| Game category, Game Mode, sustained performance mode (Android 12+) | `AndroidManifest.xml`, `game_mode_config.xml`, `Game.java` `surfaceChanged` | Added by `695c576d`; **no measurement recorded.** The sustained-performance call previously never ran on Android 12+ because it sat inside the Android 11 branch. Samsung may or may not honour it for GPU clocks. |
| 10-bit / HDR10 | `Game.java` requests `VIDEO_FORMAT_PYROWAVE_HDR10` when Enable HDR is on and PyroWave is forced; `NvConnection` counts `SCM_PYROWAVE_HDR10` in the host check; swapchain follows each frame's `hdrActive` (A2B10G10R10 + `HDR10_ST2084`, metadata from `LiGetHdrMetadata`), falling back to SDR | **Compiled only.** Needs a host that offers PyroWave HDR10. The overlay Output line should read `HDR10`, or `10-bit SDR` if it fell back. In the protocol, 10-bit and HDR are one switch (as in Moonlight-Qt, which filters its format list). 4:4:4 constants exist but there is no setting. |
| Detailed overlay and log line (format, GPU, pipeline, video data, packets, arrival jitter, slowest frame, GPU time) | `PyroWaveRenderer.java`, `strings.xml`, stats array indices 0-31 | **Compiled only.** Log line `PyroWave detail:` follows the existing perf line. |
| GPU timing | `GpuTimer`: tiny command buffers on the same queue write timestamps before the decode, after it, and after the colour conversion, on 1 frame in 8 | **Compiled only.** Needs `hostQueryReset` and `timestampValidBits >= 32`. Check the decode figure is not about 0 (that would mean the library's submit is deferred past our second stamp). |
| Client latency and end-to-end estimate | calibrated timestamps (KHR or EXT) place the GPU stamp on `CLOCK_MONOTONIC`, the clock of `DECODE_UNIT.enqueueTimeMs`; estimate = host + RTT/2 + assembly + client + one refresh period | **Compiled only.** Falls back to a "not available" message if the driver lacks the time domain. Network and display terms are estimates; time before capture and input latency are not covered. |

Stats cross to Java through `PW_RENDERER_STATS` (`pyrowave_renderer.h`), the `values[]` array in
`pyrowave_jni.cpp`, and the `PYROWAVE_STAT_*` indices in `MoonBridge.java` (currently 37).
Keep all three in step.

## 6a. Key files

- Java: `Game.java` (line numbers drift; search for `FORCE_PYROWAVE` for the format and bitrate
  setup, `willStreamHdr` for HDR, `isOnWifi` for the Wi-Fi cap), `PyroWaveRenderer.java`
  (overlay and stats thread), `MediaCodecDecoderRenderer.java` (owns the renderer),
  `MoonBridge.java`, `NvConnection.java` (HDR negotiation, ~line 256),
  `PreferenceConfiguration.java` (default PyroWave bitrate).
- Native, `app/src/main/jni/pyrowave/android/`: `pyrowave_renderer.cpp` (render thread, `PerfHint`,
  `GpuTimer`), `pw_decoder.*`, `pw_presenter.*`, `pw_swapchain.*`, `pw_vulkan.*`,
  `pyrowaveframing.*`. Glue in `moonlight-core/pyrowave_jni.cpp` and `callbacks.c`.
- Codec: `app/src/main/jni/pyrowave/pyrowave` (vendored, see `VENDOR.txt`; do not edit by hand).
- Reference: `Nonary/moonlight-qt` branch `pyrowave` at `5f9ce4a`. Its `session.cpp` requests all four
  PyroWave variants and removes the 10-bit ones when HDR is off and the 4:4:4 ones when that
  setting is off.

## 7. Measuring on the device

- Turn on **Settings, Performance Monitor, Enable Performance Logging** (needs the overlay on).
  It logs one line a second under the `LimeLog` tag:
  `PyroWave: <in> fps in, <shown> fps presented, <n> replaced, <n> rejected, <n> partial, CPU <a> ms decode + <b> ms present, ...`
- GPU, no root needed: `cat /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq` (Hz) and `.../gpubusy`
  (busy and total cycles; divide). Also `devfreq/max_freq`, `devfreq/governor`, `thermal_pwrlevel`.
- Traffic: `grep wlan0 /proc/net/dev` twice; Wi-Fi: `dumpsys wifi | grep mWifiInfo`.
- Frame loss: `adb logcat -v time moonlight-common-c:I PyroWave:I com.limelight.LimeLog:I '*:S'`.
- `dumpsys SurfaceFlinger --latency <layer>` returns nothing on Android 16; do not rely on it.
- Do not trust a monitor that reports all zeros: verify the capture file is non-empty. An early
  monitor wrote to a folder that did not exist and silently reported "no loss".

## 8. Suggested next steps, in order

1. Reconnect the tablet and install a build of the current HEAD (the last build the previous session
   installed predates the HDR / overlay / latency work; it had hint v2 but none of section 6's
   compiled-only features). Package `com.limelight.noir`. Check `git log -1` and what `695c576d`
   changed before assuming anything about the queue depth.
2. **Smoke test the compiled-only features**: app starts, overlay shows the new lines, no Vulkan
   validation errors in logcat, GPU time and client latency are plausible, and fps and present time
   are no worse than the baseline (about 122 fps, present about 6.3 ms at 475 MHz).
3. **Evaluate the clock work**: stream a game for 5+ minutes with an alt-tab out and back; does the
   clock hold 475 MHz? The game category, Game Mode, sustained-performance and render-priority changes
   in `695c576d` and hint v2 are all untested, so test them together first and then bisect if it
   still falls. If it does, remaining options: the Samsung "Enhanced processing" setting, `GameManager`
   (the app can report its state), then lowering the stream resolution (2560x1600 or 1920x1200) to cut
   GPU work per frame.
4. Test HDR10 against a host that advertises `SCM_PYROWAVE_HDR10`; check the Output line and the
   tablet's HDR indicator.
5. Cap the game at 120 fps on the host and re-measure bandwidth, superseded count and fps.
6. Open questions: why the wire rate is about twice the setting; the audio-backlog bursts; whether the
   120 Mbps Wi-Fi default is the right value for the tablet and for the other two devices; a 4:4:4
   setting if wanted.
7. Test the same APK on the Poco F6 Pro (set battery to No restrictions) and Tab S8 Ultra (slower
   Adreno 730; needs a Vulkan 1.3 driver, Android 13+ for the hint API).

## 9. Working with this user

Terse and technical. They test on real hardware and report by message or overlay photo. They
dislike being asked for a go-ahead on routine, reversible steps ("what do you need the go-ahead for"):
state what you are doing and do it, but confirm before force-pushes, deleting anything outside
scratch folders, or changing their git config. Commit and push only when asked; they have asked
each time so far. End commit messages with the co-author trailer given in the session.
