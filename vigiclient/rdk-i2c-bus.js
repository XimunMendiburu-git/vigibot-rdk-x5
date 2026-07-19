"use strict";

// Stub I2C — no PCA9685 / INA219 on test robot. See docs/gpio-mapping.md.

function openSync(_bus, _options) {
  return {
    i2cWriteSync: function () {
      throw new Error("I2C not implemented on RDK X5 POC");
    },
    i2cReadSync: function () {
      throw new Error("I2C not implemented on RDK X5 POC");
    },
    closeSync: function () {},
  };
}

module.exports = { openSync };
