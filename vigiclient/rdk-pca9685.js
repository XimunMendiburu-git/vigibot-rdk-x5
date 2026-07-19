"use strict";

// Stub PCA9685 driver — no physical module on test robot.

function Pca9685Driver() {}

Pca9685Driver.prototype.setPulseLength = function () {};
Pca9685Driver.prototype.setPWMFreq = function () {};
Pca9685Driver.prototype.setPWM = function () {};
Pca9685Driver.prototype.setAllPWM = function () {};

module.exports = Pca9685Driver;
