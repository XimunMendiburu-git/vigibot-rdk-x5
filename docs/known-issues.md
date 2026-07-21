# Known issues and runbook

Operational guide for the RDK X5 robot running Vigibot. Supplements the [POC report](./poc-vigibot-rdk-x5.md).

---

## 1. Black video after switching sources (0 ↔ 1)

### Symptoms

- Black screen in Vigibot
- Logs : `Mipi csi0 has been used`, `camera.open_cam failed`
- Previous encoder sometimes still active (`sent … nv12 frames` in the log)

### Cause

CSI is not released before the new encoder starts.

### Procedure

```bash
kill -9 $(pgrep -f vigi-encode) 2>/dev/null
sleep 2
systemctl restart vigiclient
sleep 5
grep -E 'open_cam|connected|sent |camera ready' /var/log/vigiclient.log | tail -20
```

**Warning**: avoid using `pkill -f vigi-encode` without a more specific pattern—it may match the shell command line and produce unexpected `Killed` messages. Prefer `pgrep -f 'vigi-encode-yolo|vigi-encode-rdk|vigi-encode-pose'`.

### Prevention

- Wait 2–3 seconds between source switches in Vigibot
- Do not save the hardware configuration while video is running without allowing time for the restart

---

## 2. False extreme-latency alarm

### Symptoms

```
1784300838532 ms latency, stopping of motors and streams
```

It then returns to ~300 ms. The motor failsafe is triggered.

### Cause

In `clientrobotpi.js` (around line 668):

```javascript
lastTimestamp = data.boucleVideoCommande;
```

When the server sends `boucleVideoCommande = 0`, the calculation becomes `Date.now() - 0 ≈ 1.7e12 ms`.

### Applied workarounds

1. Guard: assign only if `boucleVideoCommande > 1e12`
2. Disable the `LATENCYALARMBEGIN` condition (`false &&`)
3. Increase thresholds: END 60000 / BEGIN 120000 ms
4. Video alarm clear already commented out

### Implication

Actual latency protection is **weakened or disabled**. It should be reimplemented correctly (initialized timestamp, range validation, gradual failsafe).

---

## 3. Robot missing from Vigibot after reboot

### Symptoms

SSH works, but the robot is not visible on the website.

### Common causes

- `vigiclient` service not enabled at boot
- Wi-Fi network change (temporary disconnection)
- Client did not reconnect to the server

### Procedure

```bash
systemctl is-enabled vigiclient    # should display: enabled
systemctl status vigiclient
grep 'Connected to https://www.vigibot.com' /var/log/vigiclient.log | tail -5
ping -c 2 www.vigibot.com
```

Enable at boot if necessary:

```bash
systemctl enable vigiclient
systemctl start vigiclient
```

---

## 4. Motors turn while controls are neutral (stick centered)

### Symptoms

The wheels move slightly forward at rest and stop when a small reverse command is applied.

### Causes

- Vigibot dead zone too narrow (`INS: [-100, -1, 1, 100]`)
- Remote-control stick offset / trim
- `pwmWrite` stub (before bridge implementation)

### Solution

In the Vigibot hardware configuration, for each `PwmPwm` wheel:

```json
"INS": [-100, -15, 15, 100],
"OUTS": [-100, 0, 0, 100]
```

Check that stick trim is set to 0 on the Vigibot remote control.

---

## 5. Servos jitter at rest

### Symptoms

Unstable position holding, with audible/visible vibration at rest.

### Cause

Legacy Python backend: non-real-time userspace soft PWM.

### Options

| Option | Action |
|--------|--------|
| Accept | Tolerate it for basic teleoperation |
| Disable at rest | Pulse 0 (floating)—no jitter, but no holding torque |
| **Current** | WiringPi C helper with `SCHED_FIFO` threads, validation in progress |
| Long term | Add a PCA9685 module (I2C) |

Do not enable PWM0/PWM1 with a custom overlay: this test disabled Wi-Fi on the reference robot. See [gpio-mapping.md](./gpio-mapping.md).

---

## 6. Deploying scripts over SSH

### Issue: broken paste

Symptom: `[200~`, `^[[201~`, truncated heredoc, incomplete Python file.

### Solutions

```bash
# Disable bracketed paste before pasting a large block
bind 'set enable-bracketed-paste off'
```

Prefer transferring from the PC:

```powershell
scp vigi-encode-yolo.py sunrise@10.146.245.115:/tmp/
```

Then, on the board:

```bash
cp /tmp/vigi-encode-yolo.py /usr/local/vigiclient/
python3 -m py_compile /usr/local/vigiclient/vigi-encode-yolo.py && echo OK
wc -l /usr/local/vigiclient/vigi-encode-yolo.py
```

---

## 7. Quick diagnostic commands

### Service and processes

```bash
systemctl status vigiclient --no-pager
pgrep -af 'clientrobotpi|vigi-encode|rdk-gpio'
```

### Logs

```bash
tail -n 50 /var/log/vigiclient.log
sudo journalctl -u vigiclient -n 80 --no-pager
sudo journalctl -u vigiclient -f | grep --line-buffered 'VIDEO NAL'
```

Note: Python encoder logs often go to stderr → visible in journald, but not always in `vigiclient.log`.

### TCP video

```bash
sudo ss -tpn | grep 8043
```

Expected: Node in LISTEN state + connection from the encoder process.

### GPIO helper

```bash
pgrep -af rdk-gpio-helper
grep -n 'servoWrite\|pwmWrite' /usr/local/vigiclient/rdk-pigpio.js
```

### I2C (if PCA is added later)

```bash
ls -l /dev/i2c-*
for b in 0 2 3 4 5 6 7 8; do echo "=== bus $b ==="; i2cdetect -y -r $b 2>/dev/null | grep -E '40|70|UU'; done
```

---

## 8. Open issues (post-POC)

| Issue | Priority | Approach |
|-------|----------|-------|
| Hardware encoder incompatible with browser | Medium | Analyze NAL slices, WebCodecs |
| Servos jitter at rest | High | X5 hardware PWM or PCA9685 |
| PWM0/PWM1 overlay disables Wi-Fi | Critical | Do not deploy; analyze pinmux outside production |
| Fragile CSI source switching | Medium | Guaranteed cleanup, encoder watchdog |
| Latency failsafe disabled | High | Reimplement timestamp guard |
| I2C/PCA stubs | Low | If hardware is added |
| Unversioned scripts | Medium | Dedicated repository + deployment CI |
| IR / switches not validated | Low | Vigibot button tests |
| HBRT vs. YOLO model warning | Low | Align OpenExplorer |

---

## 9. Useful contacts and paths

| Resource | Path / URL |
|-----------|--------------|
| Client Vigibot | `/usr/local/vigiclient/` |
| Main log | `/var/log/vigiclient.log` |
| Service systemd | `/etc/systemd/system/vigiclient.service` |
| Encoder drop-in | `/etc/systemd/system/vigiclient.service.d/encode.conf` |
| Samples 40-pin | `/app/40pin_samples/` |
| BPU models | `/opt/hobot/model/x5/basic/` |
| POC documentation | `docs/` (vigibot-rdk-x5 repository) |
