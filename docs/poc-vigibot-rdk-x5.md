# POC Report — Vigibot Integration on RDK X5

## 1. Executive Summary

The POC aimed to run an **RDK X5** robot with the **Vigibot** stack (`clientrobotpi.js`), which was originally designed for Raspberry Pi. Four areas were addressed:

| Area | Final Status | Solution Type |
|-------|-----------|------------------------|
| H.264 video encoding | Working | Software workaround (libx264) |
| Second video source + YOLO overlay | Working | Python + BPU pipeline |
| Latency alarm (false positives) | Worked around | Patch / disabling |
| GPIO (motors, buzzer, servos, IR) | Working with limitations | Native WiringPi C bridge + real-time software PWM |

**Common theme**: Vigibot assumes Raspberry Pi primitives (`pigpio`, V4L2/HW camera encoder, universal software PWM) that either do not exist or differ on the RDK X5. Each area required a low-level adaptation, with trade-offs documented in the detailed guides.

---

## 2. Hardware and Software Context

### 2.1 Platform

- **Board**: RDK X5, **IMX219** CSI camera (mipi rx csi0)
- **OS**: Ubuntu 22.04.5 LTS, kernel 6.1.83
- **AI runtime**: BPU (Bayes-e), `hobot_dnn.pyeasy_dnn` working, `libpostprocess.so` available
- **Precompiled models**: `/opt/hobot/model/x5/basic/*.bin` (yolov5s_v7, yolov8, yolov10, etc.)

### 2.2 Vigibot Stack

The `rdk-*.js` modules supplied with the Vigibot RDK client were **empty stubs** (no-ops): no actual hardware operations were initially connected. The POC replaced or completed them (GPIO bridge) while retaining the official Vigibot hardware configuration (Raspberry Pi BCM numbering).

---

## 3. Overall Software Architecture

```mermaid
flowchart LR
  subgraph vigibot [Vigibot cloud]
    Server["www.vigibot.com:8042"]
  end
  subgraph rdk [RDK X5]
    Node[clientrobotpi.js]
    Enc0[vigi-encode-rdk.py]
    Enc1[vigi-encode-yolo.py]
    GpioBridge[rdk-gpio-helper C]
    Node -->|"TCP 8043"| Enc0
    Node -->|"TCP 8043"| Enc1
    Node --> GpioBridge
  end
  Server <-->|"H264 + telemetry"| Node
  Enc0 -->|libx264| Node
  Enc1 -->|"BPU YOLO + libx264"| Node
```

**Key point**: the video encoder is **not** part of Node. It is an external process launched through `CMDDIFFUSION` (defined in `sys.json`) that pushes an H.264 Annex-B stream to `tcp://127.0.0.1:8043`; Node reads the stream and forwards it to the Vigibot server.

### Key Components

| File | Role |
|---------|------|
| `clientrobotpi.js` | Main Vigibot client (remote control, GPIO, video) |
| `sys.json` | Ports, `CMDDIFFUSION`, I2C addresses, PCA frequency |
| `vigi-encode-rdk.py` | Source 0 encoder (raw camera) |
| `vigi-encode-yolo.py` | Source 1 encoder (YOLO overlay) |
| `rdk-pigpio.js` | pigpio-like API → native helper, with Python fallback |
| `rdk-gpio-helper.c` | WiringPi daemon (BCM→BOARD, real-time software PWM, GPIO357 workaround) |
| `rdk-gpio-helper.py` | Legacy Hobot.GPIO fallback |

---

## 4. Configuration Summary

| # | Area | Attempted Configuration | Result | Root Cause of Failure | Workaround | Consequence |
|---|-------|----------------------|----------|-------------------------|---------------|-------------|
| 1 | Video | Native `hb_mm_mc` Baseline HW encoder | Failed (gray/black) | Wave521 slices incompatible with Broadway | — | — |
| 2 | Video | Constrained Baseline SPS patch | Failed | Problem is in the slices, not the SPS | — | — |
| 3 | Video | ffmpeg `dump_extra` rewrap | Failed | Rewrapping does not re-encode | — | — |
| 4 | Video | **SW libx264** | **Working** | — | CPU-based ffmpeg encoding | 15 fps, 200–600 ms latency |
| 5 | YOLO | Load model before stream | Failed (black) | First inference blocks | Stream-first + infer 1/N | Delayed boxes |
| 6 | YOLO | Switch source 0↔1 | Intermittent failure | CSI not released | Kill PID + delay | Fragile switching |
| 7 | Latency | Original logic | False positive | `Date.now()-0` | Guard + disabling | Weakened safety |
| 8 | GPIO | No-op stubs | No movement | Not implemented | Initial Python bridge | 1 dedicated process |
| 9 | Motors | Python 250 Hz software PWM | Working | — | ±15 dead zone | High CPU usage |
| 10 | Servos | Python 50 Hz software PWM | Degraded | Non-real-time userspace | Busy-wait, 1 thread, hysteresis | Shaking at rest |
| 11 | Servos | PCA9685 | Not possible | No physical module | — | — |
| 12 | Servos | Custom X5 HW PWM overlay | Rejected | Overlay disabled Wi-Fi | Roll back overlay | Hardware PWM remains experimental |
| 13 | GPIO | **Native WiringPi C helper** | **Working** | — | `SCHED_FIFO`, absolute timing | Much lower CPU usage; slight servo tremor remains |
| 14 | IR left | WiringPi on BCM13/BOARD33 | Failed | PWM3 ownership + second LSIO bank | Kernel GPIO357 backend | PWM3 detached while the helper runs |
| 15 | IR right | WiringPi on BCM9/BOARD21 | **Working** | — | Direct GPIO | — |

---

## 5. Summary by Area

### Video

The Wave521 hardware encoder produces a Baseline stream whose **slice content** cannot be decoded by the Vigibot player (browser). The selected solution is an **NV12 → libx264 → TCP 8043** pipeline at 15 fps and ~700 kbps. See [video-encoding.md](./video-encoding.md).

### YOLO (source 1)

Second `CMDDIFFUSION` entry + `"SOURCE": 1` camera. BPU pipeline with an OpenCV overlay. A **stream-first** strategy is required to prevent a black screen at startup. See [yolo-source.md](./yolo-source.md).

### GPIO

Node → native WiringPi C helper with BCM→BOARD translation. Motors and the buzzer use 250 Hz software PWM; servos share a real-time 50 Hz engine with staggered pulses. A slight residual servo tremor remains. The left IR output uses the kernel GPIO357 interface because the WiringPi X5 fork cannot drive that LSIO bank correctly. See [gpio-mapping.md](./gpio-mapping.md).

### Operations

Diagnostic runbook, false latency alarms, and SSH deployment. See [known-issues.md](./known-issues.md).

---

## 6. Improvement Recommendations

### Video

- Investigate Wave521 slice compatibility with browser decoders (WebCodecs)
- Document the H.264 profile / Broadway compatibility matrix
- Test increasing FPS (20–25) if CPU capacity allows

### YOLO

- Multithreaded pipeline (capture / inference / encoding)
- Align OpenExplorer versions (model vs. HBRT)
- Version scripts and deploy through git/scp (no more SSH heredocs)

### GPIO

- Measure residual servo jitter and validate the power supply under load
- Do not retry PWM0/PWM1 overlays until the Wi-Fi pinmux conflict is understood
- Evaluate PWM3 on BOARD32/33 only with deliberate rewiring and isolated testing
- Reimplement latency safety correctly (instead of disabling it)
- Validate both IR illuminators from Vigibot and test switches 4–7

### Productionization

- Keep all modified `/usr/local/vigiclient/` files versioned in this repository
- Maintain the idempotent installation script and native helper build
- Keep the operational runbook up to date

---

## 7. References

- [video-encoding.md](./video-encoding.md)
- [yolo-source.md](./yolo-source.md)
- [gpio-mapping.md](./gpio-mapping.md)
- [known-issues.md](./known-issues.md)
- D-Robotics RDK X5 documentation — [40-pin PWM](https://developer.d-robotics.cc/rdk_x_doc/en/Basic_Application/01_40pin_user_sample/pwm)
- D-Robotics RDK X5 documentation — [Pin definition](https://developer.d-robotics.cc/rdk_x_doc/en/Basic_Application/01_40pin_user_sample/40pin_define)
