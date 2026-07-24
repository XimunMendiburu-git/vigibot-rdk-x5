# POC Documentation — Vigibot on RDK X5

Documentation for the proof of concept integrating an **RDK X5** robot (D-Robotics) with the **Vigibot** stack, which was originally designed for Raspberry Pi.

## Index

| Document | Content |
|----------|---------|
| [poc-vigibot-rdk-x5.md](./poc-vigibot-rdk-x5.md) | Full POC report (summary, architecture, overview table) |
| [video-encoding.md](./video-encoding.md) | H.264 encoding: hardware attempts, libx264 software solution |
| [video-codecs-and-encoders-guide.md](./video-codecs-and-encoders-guide.md) | RDK X5 guide: codecs, Wave521 vs libx264, tools, pros/cons (English) |
| [yolo-source.md](./yolo-source.md) | Second video source with YOLO overlay (BPU) |
| [agricultural-detection-rdk-x5.md](./agricultural-detection-rdk-x5.md) | Agri pests/diseases: YOLOv8n/v11n, 2-stage pipeline, datasets, BPU deploy |
| [pose-source.md](./pose-source.md) | Third video source with body keypoint overlay (C++ BPU) |
| [gpio-mapping.md](./gpio-mapping.md) | GPIO, PWM, servos, mapping BCM→BOARD |
| [known-issues.md](./known-issues.md) | Known issues, workarounds, and runbook |

## Target platform

| Component | Details |
|---------|--------|
| Board | RDK X5 (aarch64, Ubuntu 22.04.5 LTS, kernel 6.1.83) |
| Camera | IMX219 (CSI, mipi rx csi0) |
| Vigibot client | `/usr/local/vigiclient/` (`clientrobotpi.js`, Node.js) |
| Hardware configuration | Identical to the official Raspberry Pi configuration (BCM numbers) |
| AI runtime | BPU, `hobot_dnn.pyeasy_dnn`, `libpostprocess.so` |

## Key paths on the board

```
/usr/local/vigiclient/
├── clientrobotpi.js          # Main Vigibot client
├── sys.json                  # Ports, CMDDIFFUSION, I2C addresses
├── robot.json                # Hardware configuration (often pushed by the server)
├── vigi-encode-x264          # Video source 0 C++ binary (libx264)
├── vigi-encode-rdk.sh        # Wrapper source 0
├── vigi-encode-yolo          # Video source 1 C++ binary (YOLO)
├── vigi-encode-yolo.sh       # Wrapper source 1
├── vigi-encode-pose          # Video source 2 C++ binary (body keypoints)
├── vigi-encode-pose.sh       # Wrapper source 2
├── rdk-pigpio.js             # GPIO wrapper (native helper, Python fallback)
├── rdk-gpio-helper.c         # WiringPi C daemon (BCM→BOARD, PWM, servo)
├── rdk-gpio-helper.py        # Legacy fallback Hobot.GPIO daemon
├── rdk-i2c-bus.js            # I2C stub (originally a no-op)
└── rdk-pca9685.js            # PCA stub (originally a no-op)
```

## POC status (summary)

| Area | Status | Selected solution |
|-------|------|------------------|
| Video source 0 (H.264) | OK | Full C++ libx264 |
| Video source 1 (YOLO) | OK | Full C++ BPU + libx264 |
| Video source 2 (pose) | OK | Full C++ BPU + libx264 |
| DC motors | OK | WiringPi C soft PWM at 250 Hz + ±15 dead zone |
| Buzzer | OK | Soft PWM via bridge WiringPi C |
| Servos | OK (soft PWM) | Accel + hold-lock; residual twitch is servo-model dependent |
| IR illuminators | Under validation | WiringPi on BOARD21; kernel GPIO357 on BOARD33 |
| Hardware H.264 encoder | Abandoned | Incompatible with browser decoder |
| PCA9685 | Unavailable | No physical module on the robot |
| X5 hardware PWM (servos) | Experimental | Custom PWM0/PWM1 overlay disabled Wi-Fi |

## Source repository

This directory is part of the **vigibot-rdk-x5** repository (the GitHub repository dedicated to integrating Vigibot on RDK X5).

- **Main README**: [../README.md](../README.md) (installation, architecture, and operation)
- **Hobot SDK** (camera, BPU): separate [x5-hobot-spdev](https://github.com/D-Robotics/x5-hobot-spdev) repository
