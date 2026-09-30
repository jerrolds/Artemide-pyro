# PyroWave decode path: Moonlight-Qt → Artemide

Research only. No Artemide code is changed by this document.

## Reference sources

| Role | Repository | Branch / commit |
|---|---|---|
| Host | `Nonary/Vibepollo` | `master` (protocol: `docs/pyrowave-protocol.md`) |
| Reference client | `Nonary/moonlight-qt` | `pyrowave` @ `5f9ce4a` |
| Reference networking | `Nonary/moonlight-common-c` | `pyrowave` @ `d6a11bc` |
| Codec | `Themaister/pyrowave` | `186f0393` (vendored by both ends, with 3 local patches) |
| Target | `jerrolds/Artemide-pyro` (fork of `derflacco/moonlight-android`) | `moonlight-noir` |
| Target networking | `ClassicOldSong/moonlight-common-c` | `c999436` (Artemide's submodule) |

Note: `jerrolds/moonlight-qt-pyro` only carries `master`, which has no PyroWave
code. The PyroWave client lives on Nonary's `pyrowave` branches.

## What PyroWave is

A custom, **intra-only wavelet codec** that runs on the GPU in Vulkan (the plan's option D).

- Every frame is independent: no reference frames, no IDR requests (the host ignores them).
- Encode and decode each take well under 1 ms; the price is bitrate (~1.6 bits/pixel
  for clean 4:2:0 SDR).
- The decoder is upstream `pyrowave` + a trimmed Granite + volk, exposed through a C
  API (`pyrowave.h`). Shaders are precompiled SPIR-V (`shaders/slangmosh.hpp`).
- Upstream has a **fragment-shader iDWT path for mobile GPUs**
  (`pyrowave_decoder_create_info.fragment_path`,
  `pyrowave_decoder_device_prefers_fragment_path()`).
- Decoder requires Vulkan 1.1 subgroups + subgroup size control (Vulkan 1.3 core).

**MediaCodec is not part of the PyroWave path.** PyroWave needs its own decoder and renderer.

## PC client: one frame, packet to screen

```
UDP/RTP packets
  │  moonlight-common-c  RtpVideoQueue.c
  │    FEC repair; for PyroWave, an incomplete FEC block is NOT dropped:
  │    missing data packets are zero-filled and flagged
  ▼
VideoDepacketizer.c
  │  builds DECODE_UNIT: one buffer per RTP payload
  │    BUFFER_TYPE_PICDATA / BUFFER_TYPE_LOST / BUFFER_TYPE_RECORD_START
  │    DECODE_UNIT.pyrowaveCriticalPackets
  │    frameType always IDR; last payload trimmed to lastPayloadLen
  ▼
FFmpegVideoDecoder::submitDecodeUnit            app/streaming/video/ffmpeg.cpp:~2880
  │  copies buffers into m_DecodeBuffer, records a Segment {offset,size,lost,recordStart}
  ▼
PyroWaveDecoder::decode                          app/streaming/video/pyrowave/pyrowavedecoder.cpp
  │  PyroWaveFraming (pyrowaveframing.cpp): parse records (sequence header,
  │    block records, padding), validate, skip damaged records,
  │    decide whether the critical (coarsest) level is intact
  │  pyrowave_decoder_clear
  │  pyrowave_decoder_push_packet(record) …
  │  pyrowave_decoder_decode_is_ready(false)                       whole frame
  │    or decode_is_ready_with_sideband(true, 0, 0.9f, …)           partial frame
  │  pyrowave_decoder_decode_gpu_buffer → 3 planes (R8/R16: Y, Cb, Cr)
  ▼
D3D11 interop (Windows only)                     pyrowavesurfaces.h, d3d11pyrowave.cpp
  │  planes are D3D11 textures imported into a *private* Vulkan device;
  │  D3D11/D3D12 fence shared as a Vulkan semaphore
  ▼
D3D11VARenderer + d3d11_yuv_planar_pixel.hlsl   YCbCr → RGB, present
```

The PC client's PyroWave support is **Windows/D3D11 only**
(`initializePyroWave` is `#if defined(HAVE_PYROWAVE) && defined(Q_OS_WIN32)`).
The interop layer exists only because the renderer is D3D11. On Android the
renderer can be Vulkan itself, so this whole layer disappears.

## Component classification

| Source | Purpose | Android |
|---|---|---|
| `pyrowave/` (vendored codec, Granite subset, volk, 3 patches) | Decoder + GPU shaders | **Port directly** – build with the NDK, same commit and patches |
| `pyrowave/pyrowave.pri`, `pyrowave.pro`, `volk_platform.c` | qmake build glue | **Rewrite** as `Android.mk`/CMake (Vibepollo's `third-party/pyrowave/CMakeLists.txt` is a closer model) |
| `app/streaming/video/pyrowave/pyrowaveframing.{h,cpp}` (~570 lines) | Record parser, validation, partial-frame logic | **Port directly** – plain C++17, no Qt/SDL |
| `app/streaming/video/pyrowave/pyrowavedecoder.{h,cpp}` (~510 lines) | Wraps the C API: device, decoder, sync, decode call | **Port with changes** – keep the decode/partial logic; drop D3D11 import, `AVFrame`, SDL; share Artemide's own `VkDevice` via `pyrowave_create_device` |
| `pyrowavesurfaces.h` | Surface pool interface | **Port with changes** – pool of Vulkan images instead of D3D11 textures |
| `d3d11pyrowave.*`, `d3d11va.*` changes, `d3d11_yuv_planar_pixel.hlsl` | D3D11 surfaces and YCbCr → RGB | **Rewrite for Android** – one Vulkan full-screen pass (the matrix/range math in the HLSL is the reference) |
| `ffmpeg.cpp` PyroWave branches | Routing, stats, frame queue | **Rewrite** – becomes `PyroWaveDecoderRenderer` |
| `session.cpp` (codec selection, host-capability check, bitrate default) | Session setup | **Port logic** into `Game.java` / `PreferenceConfiguration` |
| `SettingsView.qml`, `streamingpreferences.*`, `commandlineparser.cpp`, `systemproperties.*` | UI / preferences | **Rewrite** as an Android preference ("PyroWave" video format) |
| `tests/pyrowave/tst_pyrowaveframing.cpp` | Framing unit tests | **Port directly** – useful as host-side NDK/desktop tests |
| `tests/pyrowave/tst_pyrowaved3d11.cpp`, `tst_pyrowaveroundtrip.cpp` | D3D11 / encode-decode tests | Not required (roundtrip is a useful model for an on-device self-test) |

### moonlight-common-c

Nonary's `pyrowave` branch adds 7 PyroWave commits on top of upstream `e41355e`:

| Commit | Change |
|---|---|
| `57def66` | Negotiate the codec: `VIDEO_FORMAT_PYROWAVE*`, `SCM_PYROWAVE*`, masks, `bitStreamFormat=3`, `pyrowaveFeatures`, bitstream-ID check |
| `5862e83` | Deliver PyroWave frames that lost packets (`BUFFER_TYPE_LOST`) |
| `927eba9` | Pass critical packet count and record-start flags |
| `dafbfdb` | Warn when the kernel caps the receive buffer |
| `c41f01a` | Deliver recoverable final blocks after packet silence |
| `992c287` | Release partial frames by an on-time deadline |
| `d6a11bc` | Wait for the final data packet before releasing a frame |

(The branch also carries unrelated haptics/crypto commits; don't port those.)

Trial cherry-pick onto Artemide's submodule (`c999436`): 3 commits apply cleanly;
`57def66`, `5862e83`, `927eba9` and `c41f01a` conflict in `RtspConnection.c`,
`VideoDepacketizer.c`, `Video.h` and `RtpVideoQueue.{c,h}`. The conflicts are small
(a few hunks each), but need resolving by hand, not by taking one side.
Artemide's submodule is 37 upstream commits behind `e41355e`; do not merge upstream
as part of this work.

This needs a fork of `ClassicOldSong/moonlight-common-c` and repointing Artemide's
submodule.

## Mapping onto Artemide

| Concern | Artemide today | PyroWave |
|---|---|---|
| Codec flags | `MoonBridge.java` `VIDEO_FORMAT_*`; `Game.java:697` builds `supportedVideoFormats` | Add `VIDEO_FORMAT_PYROWAVE` (8-bit 4:2:0 only for the PoC) and `SCM_PYROWAVE*`; add it only when the user picks PyroWave and the host advertises `SCM_PYROWAVE` |
| Host capability | `serverCodecModeSupport` already passed through `callbacks.c:457` | No change beyond the new bits |
| Decode-unit delivery | `callbacks.c` `BridgeDrSubmitDecodeUnit` copies all PICDATA into one Java `byte[]` and calls `MediaCodecDecoderRenderer` | **Intercept in C.** The Java path concatenates buffers and discards `BUFFER_TYPE_LOST`/`RECORD_START` and `pyrowaveCriticalPackets`, which partial-frame recovery needs. When the negotiated format is PyroWave, `BridgeDrSubmitDecodeUnit` calls the native PyroWave renderer directly; Java never sees the frame data |
| Decoder setup / teardown | `BridgeDrSetup` / `BridgeDrCleanup` → Java renderer | Same callbacks; Java creates `PyroWaveDecoderRenderer` instead of `MediaCodecDecoderRenderer` when the format is PyroWave |
| Output | `SurfaceView` (`R.id.surfaceView`) → MediaCodec | Same `SurfaceView`; pass its `Surface` to native code → `ANativeWindow` → `VkSurfaceKHR` → swapchain |
| 120 Hz / display mode / frame-rate hints | `Game.java` (`setFrameRate`, display-mode selection) | Unchanged; the swapchain presents into the same surface |
| Stats overlay | `MediaCodecDecoderRenderer` stats | New PyroWave stats (RX/decoded/presented FPS, partial and rejected frames, decode time) |
| Native build | `ndk-build` (`Android.mk`), NDK 27, `minSdk 21` | New static library for pyrowave + Granite subset; Vulkan needs API 24+ at runtime (check and fall back to H.264/HEVC) |

## Smallest architecture that displays one correct frame

```
Vibepollo ──RTP──▶ moonlight-common-c (+ 7 PyroWave commits)
                        │ DECODE_UNIT (per-packet buffers)
                        ▼
                 callbacks.c: BridgeDrSubmitDecodeUnit
                        │ format == PyroWave → native, no JNI copy
                        ▼
                 pyrowave_android.cpp  (new, ~few hundred lines)
                   ├─ PyroWaveFraming (ported as-is)
                   ├─ pyrowave C API on Artemide's own VkDevice
                   │    fragment_path = pyrowave_decoder_device_prefers_fragment_path()
                   │    decode_gpu_buffer → Y/Cb/Cr VkImages (R8)
                   └─ YCbCr→RGB full-screen pass → vkQueuePresentKHR
                        ▼
                 ANativeWindow from Game's SurfaceView
```

Java changes for this milestone: the new format constants, one preference, a
`PyroWaveDecoderRenderer` that only handles lifecycle (create, set surface,
destroy) and stats, and the renderer choice in `Game.java`.

For the first frame: 8-bit 4:2:0 SDR at 1920×1080, complete frames only
(`decode_is_ready(false)`), newest frame wins (no queue; frames are independent).

## Build order

1. Build pyrowave `186f0393` + patches for `arm64-v8a` and run its C test
   (`pyrowave_c_test.cpp`) on the S9 Ultra. Proves Vulkan 1.3/subgroup support and
   whether the fragment path is chosen on Adreno 740.
2. Fork `ClassicOldSong/moonlight-common-c`, port the 7 commits, repoint the submodule.
   Log `[PyroWave] RX frame N (bytes, packets, lost)`. Gate 3.
3. Port `PyroWaveFraming`; log parsed records per frame. Validate against the same
   stream in Moonlight-Qt.
4. Native decode + present at 1080p60. Gates 4–6.
5. Partial-frame recovery (`BUFFER_TYPE_LOST`, `RECORD_START`, `_with_sideband`).
6. 2960×1848, then 120 FPS, over a wired link. Gates 7–8.

## Open questions and risks

- **Bandwidth.** ~1.6 bpp → 2960×1848@120 ≈ 1.05 Gbps; @60 ≈ 525 Mbps. Vibepollo's
  doc targets wired LANs. Expect a USB-C 2.5GbE adapter, or a lower bitrate over Wi‑Fi.
  Vibepollo exposes `/pyrowave-bandwidth-probe` for calibration.
- **Receive buffer.** Commit `dafbfdb` exists because the kernel caps UDP receive
  buffers; Android's cap may be low for ~1 MB bursts per frame.
- **Adreno path choice.** Unknown whether `prefers_fragment_path` returns true on
  Adreno 740; step 1 answers it.
- **Bitstream match.** Must vendor exactly `186f0393`; the bitstream has no version field.
- **Colour.** Match `encoderCscMode` range/matrix; the D3D11 HLSL shader is the reference.

## Implementation status

| Step | State |
|---|---|
| moonlight-common-c PyroWave commits | Ported (`jerrolds/moonlight-common-c`) |
| Vendored codec (`app/src/main/jni/pyrowave/`) | Same tree and patches as moonlight-qt's `pyrowave/`, built by `Android.mk` for arm64-v8a and x86_64 |
| Decoder (`android/pw_decoder.*`) | moonlight-qt's `PyroWaveDecoder`, decoding into Vulkan images on the renderer's device |
| Renderer (`android/pw_presenter.*`, `pw_swapchain.*`, `pyrowave_renderer.*`) | YCbCr→RGB pass, swapchain on the `ANativeWindow`, render thread, C API |
| JNI glue, `callbacks.c` routing, Java renderer, codec preference | Not started |

Desktop tests (`android/tests/`, lavapipe or any Vulkan 1.3 GPU):

- `tst_pyrowaveframing`: moonlight-qt's framing parser tests, unchanged.
- `pw_render_test`: colour conversion, letterboxing, rotation and semaphore
  ordering of the presenter. `PW_TEST_GPU_DECODE=1` adds a full
  encode → record framing → decode → render round trip, including a lost
  payload, but needs a real GPU: Mesa's lavapipe crashes running PyroWave's
  decode shaders, in upstream's own decode path too.
