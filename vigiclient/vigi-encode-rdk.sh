#!/bin/bash
# Video source 0 — native C++ libx264 encoder.
export LD_LIBRARY_PATH=/usr/hobot/lib:/usr/lib:${LD_LIBRARY_PATH:-}
export VIGI_HW_ENCODE=0

# Give a dying previous CSI session time to release before we open.
sleep 1

BIN=/usr/local/vigiclient/vigi-encode-x264
if [[ ! -x "$BIN" ]]; then
  echo "missing $BIN — build with rebuild-x264-on-board.sh" >&2
  exit 1
fi
exec "$BIN" "$@"
