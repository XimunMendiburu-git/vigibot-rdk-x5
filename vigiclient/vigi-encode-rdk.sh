#!/bin/bash
# Prefer native C++ libx264 encoder; fall back to Python/ffmpeg.
export LD_LIBRARY_PATH=/usr/hobot/lib:/usr/lib:${LD_LIBRARY_PATH:-}
export VIGI_HW_ENCODE=0

# Give a dying previous CSI session time to release before we open.
sleep 1

BIN=/usr/local/vigiclient/vigi-encode-x264
if [[ -x "$BIN" ]]; then
  exec "$BIN" "$@"
fi
exec /usr/bin/python3 /usr/local/vigiclient/vigi-encode-rdk.py "$@"
