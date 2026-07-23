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

function Gpio(pin, options) {
  this.pin = pin;
  this._mode = options && options.mode;
  if (this._mode === module.exports.OUTPUT) {
    send("out " + this.pin + " 0");
  }
}
Gpio.prototype.mode = function (value) { this._mode = value; };
Gpio.prototype.digitalWrite = function (value) {
  send("out " + this.pin + " " + (value ? 1 : 0));
};
Gpio.prototype.digitalRead = function () { return 0; };
Gpio.prototype.pwmWrite = function (value) {
  send("pwm " + this.pin + " " + (value | 0));
};
Gpio.prototype.pwmFrequency = function () {};
Gpio.prototype._lastServoUs = -1;
Gpio.prototype.servoWrite = function (pulseWidth) {
  const us = pulseWidth | 0;
  /* Drop identical refreshes; helper also applies hysteresis for near values. */
  if (us === this._lastServoUs) {
    return;
  }
  this._lastServoUs = us;
  send("servo " + this.pin + " " + us);
};

module.exports = { Gpio, OUTPUT: 1, INPUT: 0 };
