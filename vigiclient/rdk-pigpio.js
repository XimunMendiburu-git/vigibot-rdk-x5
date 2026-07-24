"use strict";
const { spawn } = require("child_process");
const fs = require("fs");

const nativeHelper = "/usr/local/vigiclient/rdk-gpio-helper";
const helperCommand = process.env.VIGI_GPIO_HELPER ||
  (fs.existsSync(nativeHelper) ? nativeHelper : "python3");
const helperArgs = helperCommand === "python3"
  ? ["/usr/local/vigiclient/rdk-gpio-helper.py"]
  : [];

const helper = spawn(helperCommand, helperArgs, {
  stdio: ["pipe", "pipe", "inherit"],
  env: process.env,
});
helper.stdin.setDefaultEncoding("utf8");

const queue = [];
helper.stdout.setEncoding("utf8");
helper.stdout.on("data", (chunk) => {
  for (const line of chunk.split("\n")) {
    if (!line || line.indexOf("ready") >= 0) continue;
    const cb = queue.shift();
    if (cb) cb(null, line);
  }
});
helper.on("exit", (code) =>
  console.error("rdk-gpio-helper exited", helperCommand, code));

function send(cmd, cb) {
  queue.push(cb || (() => {}));
  try {
    helper.stdin.write(cmd + "\n");
  } catch (e) {
    console.error("gpio send failed", e);
  }
}

const SERVO_SKIP_US = parseInt(process.env.VIGI_SERVO_NODE_SKIP_US || "100", 10);

function Gpio(pin, options) {
  this.pin = pin;
  this._mode = options && options.mode;
  this._lastRead = 0;
  this._lastServoUs = -1;
  if (this._mode === module.exports.OUTPUT) {
    send("mode " + this.pin + " out");
    send("out " + this.pin + " 0");
  } else if (this._mode === module.exports.INPUT) {
    send("mode " + this.pin + " in");
  }
}
Gpio.prototype.mode = function (value) {
  this._mode = value;
  send("mode " + this.pin + " " +
    (value === module.exports.INPUT ? "in" : "out"));
};
Gpio.prototype.digitalWrite = function (value) {
  if (this._mode === module.exports.INPUT) {
    this.mode(module.exports.OUTPUT);
  }
  send("out " + this.pin + " " + (value ? 1 : 0));
};
Gpio.prototype.digitalRead = function () {
  const self = this;
  send("read " + this.pin, function (err, line) {
    if (err || !line) return;
    const m = /val\s+(\d+)/.exec(line);
    if (m) {
      self._lastRead = parseInt(m[1], 10) | 0;
    }
  });
  return this._lastRead;
};
Gpio.prototype.pwmWrite = function (value) {
  send("pwm " + this.pin + " " + (value | 0));
};
Gpio.prototype.pwmFrequency = function (hz) {
  if (hz == null || hz === "") return;
  send("freq " + this.pin + " " + (hz | 0));
};
Gpio.prototype.servoWrite = function (pulseWidth) {
  const us = pulseWidth | 0;
  /* Drop identical and near-identical refreshes (heat / hunting). */
  if (this._lastServoUs >= 0 && us !== 0 &&
      Math.abs(us - this._lastServoUs) < SERVO_SKIP_US) {
    return;
  }
  if (us === this._lastServoUs) {
    return;
  }
  this._lastServoUs = us;
  send("servo " + this.pin + " " + us);
};

module.exports = { Gpio, OUTPUT: 1, INPUT: 0 };
