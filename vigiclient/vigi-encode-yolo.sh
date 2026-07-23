#!/usr/bin/env bash
# Video source 1 — YOLO overlay (full C++).
export LD_LIBRARY_PATH=/usr/hobot/lib:/usr/lib:${LD_LIBRARY_PATH:-}

# Give a dying previous CSI session time to release before we open.
sleep 1

BIN=/usr/local/vigiclient/vigi-encode-yolo
if [[ ! -x "$BIN" ]]; then
  echo "missing $BIN — build with rebuild-yolo-cpp-on-board.sh" >&2
  exit 1
fi
exec "$BIN" "$@"
