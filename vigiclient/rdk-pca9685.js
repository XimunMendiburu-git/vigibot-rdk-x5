"use strict";
function Pca9685Driver(_options, callback) {
  if (callback) callback(null);
}
Pca9685Driver.prototype.channelOn = function channelOn() {};
Pca9685Driver.prototype.channelOff = function channelOff() {};
Pca9685Driver.prototype.setPulseLength = function setPulseLength() {};
Pca9685Driver.prototype.setDutyCycle = function setDutyCycle() {};
module.exports = { Pca9685Driver: Pca9685Driver };
