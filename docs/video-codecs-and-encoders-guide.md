# Video Codecs and Encoding Tools on RDK X5

A practical state-of-the-art overview of **codecs**, **encoders**, and **media tools** as they apply to the **D-Robotics RDK X5** (Horizon X5 SoC, Ubuntu 22.04, aarch64). The goal is to understand **what exists on this board**, what each option can do, and the **trade-offs**   so you can choose a pipeline for live streaming, recording, or AI overlays.

For the historical trial-and-error log of one consumer app (Vigibot browser decoder), see [video-encoding.md](./video-encoding.md). This guide stays at the **platform / toolbox** level.

---

## 1. What the RDK X5 gives you

| Block | Role on X5 | Typical APIs / libs |
|-------|------------|---------------------|
| **CSI / ISP / VIO** | MIPI camera → NV12 frames (e.g. IMX219) | `libspdev` / `sp_vio`, `libsrcampy` (Python), Hobot camera samples |
| **VPU (Wave521)** | Hardware H.264 / H.265 encode & decode | `hb_mm_mc_*` (multimedia), HBN / `sample_codec`, vendor INI profiles |
| **BPU** | Neural inference (YOLO, pose, …) | `sp_bpu_*`, `hbDNN*`, `.hbm` models, `libpostprocess` |
| **CPU (8× A55-class)** | Software encode, overlays, Node/JS client | `libx264`, ffmpeg, OpenCV |
| **Memory** | Contiguous buffers for cam / DNN / codec | `hbmem`, `hbDNN` tensors |

```text
  IMX219 (CSI)
       │
       ▼
  ┌─────────────┐          ┌────────────┐
  │  SP / VIO   │─────────►│  Overlay   │
  │  NV12 frames│          │  (OpenCV)  │
  └─────────────┘          └─────┬──────┘
       │                         │
       ├─────────────────────────┤
       ▼                         ▼
  ┌─────────────┐         ┌─────────────┐
  │ Wave521 VPU │         │ libx264 (SW)│
  │ H.264/H.265 │         │ H.264 only  │
  └─────────────┘         └─────────────┘
```

**Pixel format that matters most:** **NV12** (semi-planar YUV 4:2:0). Almost every efficient X5 path starts or ends there. RGB/BGR is convenient for drawing but costs a conversion before H.264.

---

## 2. Concepts that matter on this board

### 2.1 Codec vs elementary stream vs container

| Term | Meaning | On X5 live paths |
|------|---------|------------------|
| **Codec** | Compression algorithm | H.264 most common; HEVC available on VPU |
| **Elementary stream** | Raw NAL units | Often **Annex-B** over TCP/UDP (no MP4 wrapper) |
| **Container** | Muxed file/stream | MP4/TS for recording; live teleop often skips it |

Remux (`-c:v copy`) never fixes an incompatible bitstream. Only **re-encoding** changes slice content.

### 2.2 Profile labels vs slice content (critical on Wave521)

H.264 **SPS** can claim Baseline (`profile_idc=66`) while the **VPU still emits tools** a strict decoder rejects. Patching SPS constraint flags or using ffmpeg bitstream filters **does not rewrite slices**.

On RDK X5 this showed up clearly with Wave521: offline SPS could be made to look like Constrained Baseline, while a picky browser decoder still showed black/gray. A tolerant player (e.g. VLC) is **not** a proof of compatibility.

### 2.3 Latency vs CPU vs compatibility

```text
          bitrate efficiency
                 ▲
                 │
   latency ◄─────┼─────► decoder compatibility
                 │
            CPU / heat / FPS
```

On X5 for **interactive live video**, H.264 + predictable Baseline often beats HEVC/AV1. For **local recording or NVR-style** use, Wave521 HEVC/H.264 can win on power and resolution.

---

## 3. Codecs: what is realistic on RDK X5

| Codec | On X5? | Strengths | Weaknesses | Typical fit |
|-------|--------|-----------|------------|-------------|
| **MJPEG** | Possible (SW / some cams) | Simple, low latency | Huge bitrate | Debug snapshots |
| **H.264 / AVC** | **Yes**   VPU + SW | Universal clients, mature tooling | Older compression | Live teleop, browsers, robots |
| **H.265 / HEVC** | **Yes**   mainly VPU | Better compression | Licensing; uneven browser decode | Local storage, RTSP to capable clients |
| **VP8 / VP9 / AV1** | SW only if you build it | Open ecosystems | Encode too heavy for real-time A55 at useful res | Rare on this board for live |

**Practical default for live + wide client support:** H.264.  
**Practical default for “save CPU / go higher res” on X5:** Wave521 **if** your decoder accepts the bitstream.

### H.264 profiles (what you will negotiate)

| Profile | Notes on X5 |
|---------|-------------|
| **Baseline / Constrained Baseline** | Safest for simple / browser decoders; easiest to get right with **libx264** |
| **Main / High** | Often what HW stacks prefer; better compression if clients are full-featured |
| **Vendor INI levels** (e.g. `@L3_1`, `@L5_2` in HBN samples) | Tune resolution/bitrate caps; do **not** guarantee browser-friendly slices |

---

## 4. Encoding backends available on RDK X5

### 4.1 Software: libx264

| | |
|--|--|
| **How** | In-process C/C++ (`x264_encoder_*`) or ffmpeg `-c:v libx264` |
| **Input** | NV12 (or convert from BGR after overlays) |
| **Advantages** | Full control of Constrained Baseline, `zerolatency`, `repeat-headers`, Annex-B; predictable for strict decoders; works after BPU overlays |
| **Disadvantages** | Burns CPU (~one core class load at 640×480@15); thermal/FPS ceiling; unused VPU |
| **Best when** | Client is picky (browser Broadway / WebCodecs Baseline), or you must draw HUD/YOLO/pose then encode |

### 4.2 Hardware: Wave521 via `hb_mm_mc_*` / multimedia

| | |
|--|--|
| **How** | Link `-lmultimedia -lhbmem …`; open encoder session; feed NV12; read bitstream |
| **Advantages** | Low CPU, higher FPS/resolution headroom, power-efficient |
| **Disadvantages** | Opaque bitstream; profile/tools constrained by firmware/`libspdev`; hard to force true Constrained Baseline **content**; debugging needs NAL dumps |
| **Best when** | Recording, RTSP to VLC/FFmpeg/NVR, or a decoder known to accept Wave521 output |

### 4.3 Hardware: HBN / `sample_codec` (INI-driven)

| | |
|--|--|
| **How** | Vendor multimedia samples under `/app/multimedia_samples/…`; codec config via `.ini` (resolution, bitrate, profile tokens) |
| **Advantages** | Official examples; fast to try HW profiles without writing all glue |
| **Disadvantages** | Sample-oriented; still Wave521 underneath   same compatibility caveats; packaging into a long-running service is extra work |
| **Best when** | Prototyping HW encode parameters before committing to a custom C++ service |

### 4.4 Camera → compressed passthrough

Some IP/USB cameras emit H.264 already. On the usual **IMX219 CSI** path on X5 you get **raw NV12**, so you still choose SW or HW encode on the SoC.

### 4.5 Hybrid patterns (all seen or natural on X5)

| Pattern | Pros | Cons | Verdict on X5 |
|---------|------|------|----------------|
| **Raw NV12 → libx264** | Compatible, controllable | CPU | **Go-to for strict live clients** |
| **Raw NV12 → Wave521** | Efficient | Client may reject stream | **Go-to for recording / tolerant clients** |
| **HW → ffmpeg BSF / SPS patch** | Cheap | Does not fix slices | **Does not solve Baseline compatibility** |
| **HW → decode → libx264** | Can fix compatibility | Double cost + latency | Usually worse than raw→x264 |
| **NV12 → BPU → draw → libx264** | AI overlays + live | CPU + BPU; CSI exclusive | Standard for YOLO/pose sources |
| **ffmpeg subprocess vs in-process x264** | ffmpeg easy to tweak | Extra process / pipes | In-process wins latency/CPU slightly |

---

## 5. Frameworks and tools on the board

### 5.1 FFmpeg


| Use | Role | Caveat on X5 |
|-----|------|--------------|
| `-c:v libx264` | SW encode from raw or files | CPU; good reference bitstream |
| `-c:v copy` + BSF | Remux / inject extradata | **Cannot** turn Wave521 into true Baseline |
| `tcp` / `udp` / `rtsp` | Glue for tests | Buffers dominate measured latency |
| Dump / analyze | Validate SPS/NAL offline | Always compare with real client |

**Advantage:** fastest way to A/B SW vs HW dumps.  
**Disadvantage:** easy to believe a metadata tweak “fixed” profile when it did not.

### 5.2 GStreamer


| Advantages | Disadvantages |
|------------|----------------|
| Nice for complex graphs if plugins exist | Steeper curve; vendor HW plugins may be missing or custom |
| Good for RTSP server experiments | Harder to debug than a small C++ binary on X5 |

On this platform, **SP + libx264 C++** or **multimedia sample_codec** are often simpler than inventing a full GStreamer Hobot path.

### 5.3 D-Robotics / Hobot SP stack (`libspdev`, `sp_vio`)

| Advantages | Disadvantages |
|------------|----------------|
| Canonical CSI capture; matches SDK samples | Tied to this SoC; ABI/version sensitive |
| Shared path for cam + BPU buffers | Do not casually rebuild/replace stock camera libs |

Typical capture: `sp_open_camera_v2` → `sp_vio_get_frame` → NV12.

### 5.4 Python `libsrcampy` / older scripts

| Advantages | Disadvantages |
|------------|----------------|
| Quick prototypes (`Camera.open_cam`, `get_img`) | GIL + process hops (e.g. pipe to ffmpeg) add overhead |
| Easy to script experiments | For production live encode, C++ in-process is cleaner |

### 5.5 BPU + postprocess (vision before encode)

| Piece | Role |
|-------|------|
| `.hbm` models | Compiled networks for X5 BPU |
| `sp_bpu_*` / `hbDNNInfer` | Run inference (watch **NV12 vs NV12_SEPARATE** input layouts) |
| OpenCV draw | Boxes / skeletons on BGR or NV12 plane |
| Then encode | Almost always **libx264** if the stream must stay Baseline-safe |

**Advantage:** real-time AI on-device without a PC.  
**Disadvantage:** CSI is typically **exclusive** (one consumer); wrong tensor layout → silent empty detections.

### 5.6 Direct libx264 C API (recommended production pattern)

Embed encode in the same process as capture (and optional BPU):

1. Grab NV12  
2. Optional infer + overlay  
3. `x264_encoder_encode` → Annex-B  
4. Push bytes to TCP/UDP/file  

**Advantage vs ffmpeg CLI:** no stdin pipe, fewer copies, one binary to systemd-manage.  
**Disadvantage:** you own rate control, IDR, back-pressure, and logging (keep logs on **stderr** so they never corrupt the bitstream on stdout).

---

## 6. Takeaway

On **RDK X5**, the state of the art is a **two-lane toolbox**:

- **Wave521 (HW)**   best efficiency when the consumer can decode vendor H.264/HEVC.  
- **libx264 (SW)**   best control when you need **true Constrained Baseline**, mid-stream headers, and AI overlays on NV12.

Everything else (ffmpeg remux, SPS patches, bitstream filters, containers) is useful glue   but **only re-encoding changes what the decoder must implement**. Choose the lane from your **decoder**, not from the marketing name of the profile.
