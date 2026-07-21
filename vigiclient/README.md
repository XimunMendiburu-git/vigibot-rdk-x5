# Vigibot RDK X5 files

Scripts deployed to `/usr/local/vigiclient/` by `install/install.sh`.

| File | Purpose |
|------|---------|
| `vigi-encode-rdk.py` / `.sh` | Video source 0 (raw camera feed, libx264) |
| `vigi-encode-yolo.py` / `.sh` | Video source 1 (YOLO BPU overlay) |
| `vigi-encode-pose.py` / `.sh` | Video source 2 (body keypoint overlay via TROS) |
| `vigi-pose.launch.py` | Minimal TROS launch for mono2d body detection |
| `rdk-pigpio.js` | pigpio-like API → native helper, with Python fallback |
| `rdk-gpio-helper.c` | WiringPi C daemon source (BCM→BOARD, real-time software PWM) |
| `rdk-gpio-helper` | Binary compiled on the RDK by the installer |
| `rdk-gpio-helper.py` | Legacy Hobot.GPIO backend, retained as a fallback |
| `rdk-i2c-bus.js` / `rdk-pca9685.js` | Stubs (no I2C module installed on the test robot) |

## `clientrobotpi.js`

The main Vigibot Node client is **not redistributed** in this repository (it is a proprietary Vigibot binary). It must already have been installed by Vigibot in `/usr/local/vigiclient/`.

To fetch a patched version from your reference board:

```bash
./scripts/fetch-from-board.sh
```

This also copies `clientrobotpi.js` (latency patches, etc.) if it is present on the board.

**Never commit** `robot.json` with real credentials.
