#!/usr/bin/env bash
# YOLO overlay — prefer full C++ binary; fall back to Python.
export LD_LIBRARY_PATH=/usr/hobot/lib:/usr/lib:${LD_LIBRARY_PATH:-}

# Give a dying previous CSI session time to release before we open.
sleep 1

BIN=/usr/local/vigiclient/vigi-encode-yolo
if [[ -x "$BIN" ]]; then
  exec "$BIN" "$@"
fi
exec /usr/bin/python3 /usr/local/vigiclient/vigi-encode-yolo.py "$@"
