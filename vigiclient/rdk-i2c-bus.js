"use strict";
function bus() {
  return {
    i2cWriteSync: function () { throw new Error("no i2c"); },
    readWordSync: function () { throw new Error("no i2c"); },
    readWord: function (a, b, cb) { cb(new Error("no i2c")); },
    readByte: function (a, b, cb) { cb(new Error("no i2c")); },
    closeSync: function () {},
  };
}
module.exports = { openSync: function () { return bus(); } };
