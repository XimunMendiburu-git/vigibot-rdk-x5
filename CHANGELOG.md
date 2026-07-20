# Changelog

All notable changes to this project are documented here.

## [Unreleased]

### Changed

- Replaced the Python GPIO backend with a native WiringPi C helper
- Moved motor, buzzer and servo generation to real-time soft-PWM threads
- Kept the Python GPIO helper as an automatic fallback
- Disabled custom PWM pinmux overlays after an overlay caused loss of Wi-Fi
- Added kernel GPIO control for the left IR illuminator on GPIO357/BOARD33

## [v0.1.0-poc] — 2026-07-19

Initial proof-of-concept release for Vigibot on RDK X5.

### Added

- POC documentation (`docs/`): software H.264 video, YOLO source, GPIO, and runbook
- Vigibot scripts adapted for RDK X5 (`vigiclient/`)
- Configuration examples (`config/*.example`)
- systemd units (`systemd/`)
- `install/install.sh` — idempotent deployment to `/usr/local/vigiclient/`
- `scripts/fetch-from-board.sh` — fetching from a reference board
- `scripts/health-check.sh` — post-installation checks

### Working (POC)

- Video source 0: software libx264 through ffmpeg
- Video source 1: YOLOv5 BPU overlay
- DC motors and buzzer through the Python GPIO bridge
- Vigibot cloud connection

### Known limitations

- Wave521 hardware H.264 encoder is incompatible with the Vigibot browser
- Servos: 50 Hz software PWM—jitter while idle
- Latency failsafe partially disabled (false positives from `boucleVideoCommande=0`)
- PCA9685 / I2C not implemented (no module installed on the test robot)
