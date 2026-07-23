# GPIO — Vigibot / RDK X5

## 1. Context

The **official Raspberry Pi** Vigibot hardware configuration uses **BCM** numbers and relies on `pigpio` (general-purpose soft PWM, servo pulses in µs). On RDK X5:

| | Raspberry Pi | RDK X5 |
|--|--------------|--------|
| Library | `pigpio` | WiringPi RDK (C), `Hobot.GPIO` fallback |
| Numbering | BCM | **BOARD** (physical pins 1–40) |
| Soft PWM | Available everywhere through pigpio | Implemented by the C helper |
| Hardware PWM | Limited | 8 channels on dedicated pins (X5) |

**Current strategy**: keep the Vigibot BCM configuration unchanged; translate BCM→BOARD and generate PWM in a WiringPi C daemon. The original Python backend remains available as a fallback.

---

## 2. Initial state: no-op stubs

The modules supplied in `/usr/local/vigiclient/` did **nothing**:

```javascript
// rdk-pigpio.js (original)
Gpio.prototype.digitalWrite = function () {};
Gpio.prototype.pwmWrite = function () {};
Gpio.prototype.servoWrite = function () {};

// rdk-i2c-bus.js (original)
openSync: function () { return { i2cWriteSync: function () { throw ... } }; }

// rdk-pca9685.js (original)
Pca9685Driver.prototype.setPulseLength = function () {};
```

Vigibot appeared to control the hardware, but no commands reached it.

---

## 3. GPIO bridge architecture

```mermaid
flowchart LR
  Node[clientrobotpi.js]
  RdkJs[rdk-pigpio.js]
  Helper[rdk-gpio-helper C]
  WiringPi[WiringPi BOARD]
  Pins[Header 40-pin]
  Node --> RdkJs
  RdkJs -->|"spawn stdin"| Helper
  Helper -->|"BCM to BOARD + soft PWM"| WiringPi
  WiringPi --> Pins
```

### stdin protocol (text)

| Command | Usage |
|----------|-------|
| `out <bcm> <0\|1>` | Digital write |
| `pwm <bcm> <0-255>` | Motor/buzzer PWM (soft PWM) |
| `servo <bcm> <pulse_us>` | Servo 500–2500 µs (soft PWM 50 Hz) |

### Files

| File | Role |
|---------|------|
| `rdk-pigpio.js` | pigpio-like API, spawns the helper, `digitalWrite`, `pwmWrite`, `servoWrite` |
| `rdk-gpio-helper.c` | WiringPi daemon, BCM→BOARD translation, real-time soft PWM |
| `rdk-gpio-helper.py` | Legacy Hobot.GPIO backend, automatic fallback if the binary is missing |

Node calls `setServos` → `servoWrite(pwm_µs)` when `ADRESSE == -1` (no PCA). Similarly, `setPwmPwm` → `pwmWrite` for the motors.

---

## 4. BCM → BOARD table

Official Pi Vigibot configuration — translation for Hobot.GPIO:

| Configuration role | BCM | BOARD |
|-------------|-----|-------|
| Turret pan | 5 | 29 |
| Turret tilt | 6 | 31 |
| Gripper claw | 7 | 26 |
| Gripper tilt | 8 | 24 |
| Front left wheel (IN1, IN2) | 22, 23 | 15, 16 |
| Front right wheel | 24, 25 | 18, 22 |
| Rear left wheel | 16, 17 | 36, 11 |
| Rear right wheel | 26, 27 | 37, 13 |
| IR left | 13 | 33 |
| IR right | 9 | 21 |
| Buzzer | 4 | 7 |
| Brightness boost | 1 | 28 |
| Switch 4–7 | 18–21 | 12, 35, 38, 40 |

Hardware validation: blink test with `simple_out.py` on BOARD **37** → rear-right wheel (BCM 26).

---

## 5. Results by output type

### DC motors (`PwmPwm`) — OK

| Item | Details |
|---------|--------|
| Mechanism | C soft PWM at 250 Hz, one thread per pin |
| Dead zone | Vigibot `INS`: `[-100, -15, 15, 100]` (instead of ±1) |
| CPU | Native helper measured at ~0.7% while idle (active load remains to be measured) |
| Speed control | Progressive (duty 0–255) |

**Initial issue**: wheels turned while controls were neutral (joystick offset + overly narrow ±1 dead zone).

### Buzzer (`Pwms`, BCM 4 → BOARD 7) — OK

Driven through `pwmWrite` → soft PWM. Validated.

### Servos (`Servos`, pulses 500–2500 µs) — UNDER VALIDATION

| Attempt | Result |
|-----------|----------|
| 50 Hz soft PWM + `time.sleep` | Significant jitter |
| Busy-wait with `perf_counter` during HIGH pulse | Reduced |
| 15–40 µs hysteresis + 20 µs quantization | Reduced |
| One `servo-engine` thread for all servos | Reduced |
| `renice -10` on helper | Marginal |
| WiringPi C helper + `SCHED_FIFO` | Deployed; clear improvement observed |
| Single servo thread + phase-shifted pulses | Deployed to reduce jitter and simultaneous current draw |

The C helper uses an absolute monotonic clock and a single real-time thread.
Servo pulses are emitted sequentially within each 20 ms frame (spread current
draw). Each high phase is busy-waited for accurate width under video/BPU load;
consigne updates are quantized (40 µs) with 80 µs hysteresis. Node also skips
identical consecutive `servoWrite` values. BCM7/BOARD26 uses the same soft PWM
engine.

### IR (`Gpios`) — bridge validated

BOARD21/BCM9 is driven directly by WiringPi. BOARD33/BCM13 belongs
to the second LSIO bank and remains reserved by PWM3 at startup; the
WiringPi X5 fork cannot switch it correctly. The native helper therefore
detaches PWM3 and then drives GPIO357 through the kernel GPIO interface.
This workaround does not affect any motor or servo GPIO.

Vigibot `COMMANDS1` buttons 0 and 1 control the left and right
illuminators, respectively.

### Switches (`Gpios`) — Not validated

Switch outputs 4–7 still need explicit testing.

---

## 6. PCA9685 — unavailable

| Finding | Details |
|---------|--------|
| Physical module | **Not present** on the robot |
| I2C scan | No `0x40` or `0x70` on buses 0, 2, 3, 4, 5, 6, 7, 8 |
| `/dev/i2c-1` | **Does not exist** (I2C1 multiplexed with PWM3) |
| `rdk-i2c-bus.js` | Stub no-op |
| `clientrobotpi.js` | `I2C.openSync(1)` — invalid bus on this image |

Pi-style header wiring: SDA pin **3**, SCL pin **5** (RDK X5 documentation).

---

## 7. X5 hardware PWM (experimental, disabled)

Correction of an incorrect assumption (X3 sample): on **X5**, the hardware PWM frequency is configurable over a wide range.

| | X3 (sample comment) | **X5 (actual)** |
|--|------------------------|---------------|
| Frequency | 48 kHz – 192 MHz | **0.05 Hz – 1 MHz** |
| Channels | 2 (pins 32, 33) | **8** (4 groups × 2) |

### Hardware PWM mapping (D-Robotics documentation)

| Group | BOARD pins | Pi configuration match (approx.) |
|--------|------------|-----------------------------------|
| PWM0 | 29, 31 | Turret pan/tilt (BCM 5, 6) |
| PWM1 | 37, 24 | Rear R + gripper tilt (BCM 26, 8) |
| PWM2 | 28, 27 | Boost + rear R IN2 (BCM 1, 27) |
| PWM3 | 32, 33 | (often enabled by default) |

### Activation warning

Do not enable PWM0/PWM1 with a custom overlay on the reference robot. The July 20, 2026 test made the Wi-Fi interface unavailable until the overlay was removed from `/boot/config.txt` and the system was rebooted.

The exact pinmux cause has not yet been isolated. The native helper therefore leaves hardware PWM disabled by default and requires no overlay.

### Servo usage (principle)

```python
import Hobot.GPIO as GPIO
GPIO.setmode(GPIO.BOARD)
p = GPIO.PWM(29, 50)  # 50 Hz
p.start(7.5)          # 1500 µs → duty = 1500/20000 * 100 = 7.5 %
```

### Multiplexing (conflicts)

| Function 1 | Function 2 |
|------------|------------|
| uart3 | i2c5 |
| i2c0 | pwm2 |
| spi2 | pwm0 |
| spi2 | pwm1 |
| i2c1 | pwm3 |

Enabling PWM **disables** the multiplexed function on those pins.

**Note**: BCM 7 (gripper claw) → BOARD **26** is **not** a hardware PWM pin. Wiring or Vigibot configuration reassignment is required.

---

## 8. Workaround implications

| Workaround | Implication |
|---------------|-------------|
| Persistent C bridge | External process to monitor; Python fallback retained |
| Motor soft PWM | Best-effort real time; active load remains to be measured |
| Servo soft PWM | Quantize + hysteresis + busy-wait falling edge; residual tremor possible under heavy load |
| BCM configuration + translation | Mapping must be maintained; risk of error if wiring differs from Pi |
| I2C/PCA stubs retained | INA219 / PCA not functional on the Vigibot side |
| PWM0/PWM1 overlay | Observed Wi-Fi conflict; prohibited in the installation |

---

## 9. Potential improvements

1. Measure WiringPi C helper jitter with all four servos and video active
2. Precisely identify the pinmux/Wi-Fi conflict before any further hardware PWM tests
3. Evaluate PWM3 on BOARD 32/33 with rewiring, without a PWM0/PWM1 overlay
4. Implement functional `rdk-i2c-bus.js` + `rdk-pca9685.js` modules if a PCA module is added
5. Validate IR illuminators and switches
