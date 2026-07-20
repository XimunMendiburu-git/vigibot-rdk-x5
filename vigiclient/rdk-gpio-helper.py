#!/usr/bin/env python3
import sys
import threading
import time
import Hobot.GPIO as GPIO

BCM_TO_BOARD = {
    1: 28, 4: 7, 5: 29, 6: 31, 7: 26, 8: 24, 9: 21,
    13: 33, 16: 36, 17: 11, 18: 12, 19: 35, 20: 38, 21: 40,
    22: 15, 23: 16, 24: 18, 25: 22, 26: 37, 27: 13,
}

PWM_FREQ = 250
SERVO_FREQ = 50
SERVO_HYST_US = 40
SERVO_QUANTUM_US = 20  # arrondi → moins de micro-variations

GPIO.setwarnings(False)
GPIO.setmode(GPIO.BOARD)
setup = set()
pwm_channels = {}
servo_pulses = {}
lock = threading.Lock()


class SoftPwm:
    def __init__(self, pin, freq):
        self.pin = pin
        self.freq = freq
        self.duty = 0
        self._stop = False
        self._th = threading.Thread(target=self._run, name="spwm-%d" % pin, daemon=True)
        self._th.start()

    def set_duty(self, duty):
        self.duty = max(0, min(255, int(duty)))

    def stop(self):
        self._stop = True
        self.duty = 0
        try:
            GPIO.output(self.pin, GPIO.LOW)
        except Exception:
            pass

    def _run(self):
        period = 1.0 / float(self.freq)
        while not self._stop:
            d = self.duty
            if d <= 0:
                GPIO.output(self.pin, GPIO.LOW)
                time.sleep(period)
            elif d >= 255:
                GPIO.output(self.pin, GPIO.HIGH)
                time.sleep(period)
            else:
                on = period * (d / 255.0)
                off = period - on
                GPIO.output(self.pin, GPIO.HIGH)
                if on > 0:
                    time.sleep(on)
                GPIO.output(self.pin, GPIO.LOW)
                if off > 0:
                    time.sleep(off)


def quantize_us(us):
    if us <= 0:
        return 0
    q = SERVO_QUANTUM_US
    return int(round(us / float(q)) * q)


def servo_engine():
    period = 1.0 / float(SERVO_FREQ)
    while True:
        t0 = time.perf_counter()
        with lock:
            items = [(p, u) for p, u in servo_pulses.items() if u > 0]
            off_pins = [p for p, u in servo_pulses.items() if u <= 0]
        for pin in off_pins:
            try:
                GPIO.output(pin, GPIO.LOW)
            except Exception:
                pass
        for pin, us in items:
            try:
                GPIO.output(pin, GPIO.HIGH)
                end_h = time.perf_counter() + us / 1000000.0
                while time.perf_counter() < end_h:
                    pass
                GPIO.output(self.pin if False else pin, GPIO.LOW)
            except Exception:
                pass
        remain = (t0 + period) - time.perf_counter()
        if remain > 0.001:
            time.sleep(remain)


threading.Thread(target=servo_engine, name="servo-engine", daemon=True).start()


def board_pin(n):
    n = int(n)
    return BCM_TO_BOARD.get(n, n)


def ensure_out(pin):
    pin = board_pin(pin)
    with lock:
        if pin not in setup:
            GPIO.setup(pin, GPIO.OUT, initial=GPIO.LOW)
            setup.add(pin)
    return pin


def set_pwm(bcm, duty):
    pin = ensure_out(bcm)
    with lock:
        servo_pulses.pop(pin, None)
        ch = pwm_channels.get(pin)
        if ch is None:
            ch = SoftPwm(pin, PWM_FREQ)
            pwm_channels[pin] = ch
        ch.set_duty(duty)


def set_servo(bcm, pulse_us):
    pin = ensure_out(bcm)
    us = quantize_us(max(0, min(2500, int(pulse_us))))
    with lock:
        ch = pwm_channels.pop(pin, None)
        if ch is not None:
            ch.stop()
        cur = servo_pulses.get(pin, 0)
        if us != 0 and abs(us - cur) < SERVO_HYST_US:
            return
        servo_pulses[pin] = us


def set_out(bcm, value):
    pin = ensure_out(bcm)
    with lock:
        if pin in pwm_channels:
            pwm_channels[pin].set_duty(255 if value else 0)
            return
        if pin in servo_pulses:
            if not value:
                servo_pulses[pin] = 0
            return
    GPIO.output(pin, GPIO.HIGH if value else GPIO.LOW)


print("rdk-gpio-helper ready pwm=%d servo=engine hyst=%d q=%d" % (
    PWM_FREQ, SERVO_HYST_US, SERVO_QUANTUM_US), flush=True)

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    parts = line.split()
    cmd = parts[0]
    try:
        if cmd == "out":
            set_out(parts[1], int(parts[2]))
            print("ok", flush=True)
        elif cmd == "pwm":
            set_pwm(parts[1], int(parts[2]))
            print("ok", flush=True)
        elif cmd == "servo":
            set_servo(parts[1], int(parts[2]))
            print("ok", flush=True)
        elif cmd == "ping":
            print("ok", flush=True)
        else:
            print("err unknown", flush=True)
    except Exception as e:
        print("err", e, flush=True)
