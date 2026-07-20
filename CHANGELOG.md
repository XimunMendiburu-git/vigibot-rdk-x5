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

- Documentation POC (`docs/`) : vidéo H.264 SW, source YOLO, GPIO, runbook
- Scripts Vigibot adaptés RDK X5 (`vigiclient/`)
- Exemples de configuration (`config/*.example`)
- Unités systemd (`systemd/`)
- `install/install.sh` — déploiement idempotent vers `/usr/local/vigiclient/`
- `scripts/fetch-from-board.sh` — rapatriement depuis une carte de référence
- `scripts/health-check.sh` — vérifications post-install

### Working (POC)

- Vidéo source 0 : libx264 software via ffmpeg
- Vidéo source 1 : overlay YOLOv5 BPU
- Moteurs DC et buzzer via bridge GPIO Python
- Connexion Vigibot cloud

### Known limitations

- Encodeur H.264 matériel Wave521 incompatible navigateur Vigibot
- Servos : soft PWM 50 Hz — tremblement au repos
- Failsafe latence partiellement neutralisé (faux positifs `boucleVideoCommande=0`)
- PCA9685 / I2C non implémentés (pas de module sur le robot de test)
