#!/usr/bin/env python3
"""
GPIO bridge daemon for Vigibot on RDK X5.

Reads line commands on stdin:
  out <bcm> <0|1>
  pwm <bcm> <0-255>
  servo <bcm> <pulse_us>

BCM pin numbers match official Vigibot Raspberry Pi hardware config;
translated to BOARD numbering for Hobot.GPIO.
"""

from __future__ import annotations

import os
import sys
import threading
import time

import Hobot.GPIO as GPIO

# Official Vigibot Pi config (BCM) → RDK X5 BOARD header pins
BCM_TO_BOARD = {
    1: 28,
    4: 7,
    5: 29,
    6: 31,
    7: 26,
    8: 24,
    9: 21,
    13: 33,
    16: 36,
    17: 11,
    18: 12,
    19: 35,
    20: 38,
    21: 40,
    22: 15,
    23: 16,
    24: 18,
    25: 22,
    26: 37,
    27: 13,
}

MOTOR_PWM_HZ = 250
SERVO_PWM_HZ = 50
SERVO_HYSTERESIS_US = 25
SERVO_QUANTUM_US = 20

GPIO.setwarnings(False)
GPIO.setmode(GPIO.BOARD)

configured: set[int] = set()
pwm_threads: dict[int, threading.Thread] = {}
pwm_state: dict[int, dict] = {}
servo_lock = threading.Lock()
servo_targets: dict[int, int] = {}
servo_thread: threading.Thread | None = None
servo_stop = False


def board_pin(bcm: int) -> int:
    if bcm not in BCM_TO_BOARD:
        raise ValueError(f"unknown BCM pin {bcm}")
    return BCM_TO_BOARD[bcm]


def ensure_output(pin: int) -> None:
    if pin not in configured:
        GPIO.setup(pin, GPIO.OUT, initial=GPIO.LOW)
        configured.add(pin)


def soft_pwm_loop(pin: int, hz: int) -> None:
    period = 1.0 / hz
    while True:
        st = pwm_state.get(pin)
        if not st or st.get("stop"):
            break
        duty = max(0, min(255, int(st.get("duty", 0))))
        if duty <= 0:
            GPIO.output(pin, GPIO.LOW)
            time.sleep(period)
            continue
        if duty >= 255:
            GPIO.output(pin, GPIO.HIGH)
            time.sleep(period)
            continue
        high = period * (duty / 255.0)
        low = period - high
        GPIO.output(pin, GPIO.HIGH)
        time.sleep(high)
        GPIO.output(pin, GPIO.LOW)
        time.sleep(low)


def set_pwm(bcm: int, duty: int) -> None:
    pin = board_pin(bcm)
    ensure_output(pin)
    st = pwm_state.setdefault(pin, {"duty": 0, "stop": False, "hz": MOTOR_PWM_HZ})
    st["duty"] = max(0, min(255, duty))
    st["hz"] = MOTOR_PWM_HZ
    if pin not in pwm_threads or not pwm_threads[pin].is_alive():
        t = threading.Thread(target=soft_pwm_loop, args=(pin, MOTOR_PWM_HZ), daemon=True)
        pwm_threads[pin] = t
        t.start()


def servo_engine() -> None:
    period = 1.0 / SERVO_PWM_HZ
    while not servo_stop:
        with servo_lock:
            items = list(servo_targets.items())
        if not items:
            time.sleep(0.01)
            continue
        cycle_start = time.perf_counter()
        for pin, pulse_us in items:
            pulse_us = max(500, min(2500, pulse_us))
            high = pulse_us / 1_000_000.0
            GPIO.output(pin, GPIO.HIGH)
            time.sleep(high)
            GPIO.output(pin, GPIO.LOW)
        elapsed = time.perf_counter() - cycle_start
        rest = period - elapsed
        if rest > 0:
            time.sleep(rest)


def start_servo_engine() -> None:
    global servo_thread
    if servo_thread and servo_thread.is_alive():
        return
    t = threading.Thread(target=servo_engine, daemon=True)
    servo_thread = t
    t.start()


def set_servo(bcm: int, pulse_us: int) -> None:
    pin = board_pin(bcm)
    ensure_output(pin)
    pulse_us = max(500, min(2500, int(pulse_us)))
    prev = servo_targets.get(pin)
    if prev is not None:
        if abs(prev - pulse_us) < SERVO_HYSTERESIS_US:
            return
        pulse_us = ((pulse_us + SERVO_QUANTUM_US // 2) // SERVO_QUANTUM_US) * SERVO_QUANTUM_US
    with servo_lock:
        servo_targets[pin] = pulse_us
    start_servo_engine()


def handle_line(line: str) -> None:
    parts = line.strip().split()
    if len(parts) < 2:
        return
    cmd = parts[0].lower()
    bcm = int(parts[1])
    if cmd == "out":
        val = int(parts[2])
        pin = board_pin(bcm)
        ensure_output(pin)
        GPIO.output(pin, GPIO.HIGH if val else GPIO.LOW)
    elif cmd == "pwm":
        set_pwm(bcm, int(parts[2]))
    elif cmd == "servo":
        set_servo(bcm, int(parts[2]))


def main() -> None:
    try:
        os.nice(-5)
    except OSError:
        pass
    for line in sys.stdin:
        if not line:
            break
        try:
            handle_line(line)
        except Exception as exc:
            print(f"err: {exc}", file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
