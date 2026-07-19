"use strict";

const { spawn } = require("child_process");
const path = require("path");

const HELPER = path.join(__dirname, "rdk-gpio-helper.py");
let helperProc = null;
let helperQueue = Promise.resolve();

function ensureHelper() {
  if (helperProc && !helperProc.killed) {
    return;
  }
  helperProc = spawn("python3", [HELPER], {
    stdio: ["pipe", "ignore", "pipe"],
  });
  helperProc.on("exit", () => {
    helperProc = null;
  });
  helperProc.stderr.on("data", (chunk) => {
    process.stderr.write(chunk);
  });
}

function sendLine(line) {
  ensureHelper();
  helperQueue = helperQueue.then(
    () =>
      new Promise((resolve, reject) => {
        if (!helperProc || !helperProc.stdin) {
          reject(new Error("gpio helper not running"));
          return;
        }
        helperProc.stdin.write(line + "\n", (err) => {
          if (err) reject(err);
          else resolve();
        });
      })
  );
  return helperQueue;
}

function Gpio(pin, options) {
  this.pin = pin;
  this.mode = (options && options.mode) || "output";
}

Gpio.prototype.digitalWrite = function (value) {
  const v = value ? 1 : 0;
  return sendLine(`out ${this.pin} ${v}`);
};

Gpio.prototype.pwmWrite = function (duty) {
  const d = Math.max(0, Math.min(255, Math.round(Number(duty) || 0)));
  return sendLine(`pwm ${this.pin} ${d}`);
};

Gpio.prototype.servoWrite = function (pulseUs) {
  const us = Math.max(500, Math.min(2500, Math.round(Number(pulseUs) || 1500)));
  return sendLine(`servo ${this.pin} ${us}`);
};

Gpio.prototype.digitalRead = function () {
  return 0;
};

Gpio.prototype.modeSet = function () {};
Gpio.prototype.glitchSet = function () {};

module.exports = Gpio;
