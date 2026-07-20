# Vigibot on RDK X5

Integration of a **D-Robotics RDK X5** robot with the **[Vigibot](https://www.vigibot.com)** stack (`clientrobotpi.js`), which was originally designed for Raspberry Pi.

This repository contains the adapted scripts, sample configuration, systemd units, and proof-of-concept (POC) documentation. The Hobot SDK (camera, BPU) remains in a separate repository: [x5-hobot-spdev](https://github.com/D-Robotics/x5-hobot-spdev).

## POC status

| Component | Status | Selected solution |
|-----------|--------|-------------------|
| Video source 0 (H.264) | OK | Software libx264 (ffmpeg) |
| Video source 1 (YOLO) | OK | Python + BPU pipeline, stream-first |
| DC motors | OK | 250 Hz C software PWM + ±15 dead zone |
| Buzzer | OK | Software PWM through the WiringPi C bridge |
| Servos | Under validation | Real-time 50 Hz C software PWM, including BCM7 |
| Hardware H.264 encoder | Abandoned | Incompatible with the Vigibot browser decoder |
| PCA9685 | Unavailable | No module installed on the test robot |

Core issue: Vigibot assumes `pigpio`, the Pi camera encoder, and universal software PWM are available. On RDK X5, each primitive was replaced or worked around—see [docs/poc-vigibot-rdk-x5.md](docs/poc-vigibot-rdk-x5.md).

## Architecture

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

The video encoder is an **external process** launched through `CMDDIFFUSION` (`sys.json`). It sends H.264 Annex-B to `tcp://127.0.0.1:8043`; Node reads the stream and forwards it to the Vigibot server.

## Prerequisites

| Component | Details |
|-----------|---------|
| Board | RDK X5, Ubuntu 22.04, kernel 6.1.x |
| Camera | IMX219 (CSI) |
| Vigibot client | `clientrobotpi.js` installed in `/usr/local/vigiclient/` |
| Runtime | Node.js, Python 3, ffmpeg, WiringPi RDK, `hobot_dnn`, `hobot_vio` |
| YOLO model | `/opt/hobot/model/x5/basic/yolov5s_v7_640x640_nv12.bin` |

## Quick installation

### 1. Clone onto the board (or development PC)

```bash
git clone <YOUR-REPOSITORY-URL>/vigibot-rdk-x5.git
cd vigibot-rdk-x5
```

### 2. (Optional) Fetch from a reference board

From a PC with SSH access to the POC board:

```bash
export BOARD_HOST=10.146.245.115   # adjust the IP address
chmod +x scripts/fetch-from-board.sh
./scripts/fetch-from-board.sh
```

This fetches the modified files from `/usr/local/vigiclient/` and the systemd units. **Never commit** `robot.json` with real credentials.

### 3. Deploy to the RDK

On the board, as root:

```bash
cd vigibot-rdk-x5
chmod +x install/install.sh scripts/health-check.sh
sudo ./install/install.sh
```

The installer:

- backs up the existing `/usr/local/vigiclient/` directory;
- copies the RDK scripts (`vigi-encode-*`, `rdk-*`);
- installs systemd and the `encode.conf` drop-in (`VIGI_USE_FFMPEG=1`);
- enables and restarts `vigiclient`.

### 4. Configure

```bash
sudo cp config/robot.json.example /usr/local/vigiclient/robot.json
sudo nano /usr/local/vigiclient/robot.json   # NAME, PASSWORD Vigibot
```

If missing, `sys.json` is created from the example file. The official Vigibot Pi hardware configuration (**BCM** numbering) is retained; `rdk-gpio-helper` (C/WiringPi) handles BCM→BOARD mapping and real-time software PWM. The Python helper remains available as a fallback.

## Operation

```bash
sudo systemctl enable vigiclient
sudo systemctl start vigiclient
sudo systemctl status vigiclient
tail -f /var/log/vigiclient.log
sudo ./scripts/health-check.sh
```

### Switching video sources (0 ↔ 1)

If the screen is black or `Mipi csi0 has been used` appears:

```bash
kill -9 $(pgrep -f 'vigi-encode-yolo|vigi-encode-rdk') 2>/dev/null
sleep 2
sudo systemctl restart vigiclient
```

Full runbook: [docs/known-issues.md](docs/known-issues.md).

## Repository structure

```
├── README.md
├── docs/                 # Detailed POC documentation
├── vigiclient/           # Scripts deployed to the board
├── config/               # sys.json and robot.json examples
├── systemd/              # vigiclient.service + drop-ins
├── install/install.sh    # Idempotent deployment
└── scripts/
    ├── fetch-from-board.sh
    └── health-check.sh
```

## Documentation

| Document | Contents |
|----------|----------|
| [docs/README.md](docs/README.md) | Index |
| [docs/poc-vigibot-rdk-x5.md](docs/poc-vigibot-rdk-x5.md) | POC report and table of 12 configurations |
| [docs/video-encoding.md](docs/video-encoding.md) | Software H.264 vs. hardware Wave521 |
| [docs/yolo-source.md](docs/yolo-source.md) | Second YOLO BPU source |
| [docs/gpio-mapping.md](docs/gpio-mapping.md) | GPIO bridge, BCM→BOARD, hardware PWM |
| [docs/known-issues.md](docs/known-issues.md) | Operations runbook |

## Roadmap

1. **Servos** — migrate to X5 hardware PWM (`srpi-config`, `GPIO.PWM(50)`)
2. **Latency failsafe** — reimplement cleanly (`boucleVideoCommande` guard)
3. **CSI switching** — guarantee encoder cleanup when changing sources
4. **PCA9685** — implement `rdk-i2c-bus.js` / `rdk-pca9685.js` if a module is added
5. **CI deployment** — use scp/rsync from GitHub Actions or a release tag

## License

MIT—see [LICENSE](LICENSE). The Vigibot client (`clientrobotpi.js`) remains subject to the Vigibot terms.
