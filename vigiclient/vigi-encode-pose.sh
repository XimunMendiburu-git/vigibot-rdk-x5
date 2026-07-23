#!/usr/bin/env bash
# Body-pose overlay - prefer full C++ binary; fall back to TROS Python.
export LD_LIBRARY_PATH=/usr/hobot/lib:/usr/lib:${LD_LIBRARY_PATH:-}

# Give a dying previous CSI session time to release before we open.
sleep 1

BIN=/usr/local/vigiclient/vigi-encode-pose
if [[ -x "$BIN" ]]; then
  exec "$BIN" "$@"
fi

# Legacy fallback (ROS/TROS).
set +u
# shellcheck disable=SC1091
source /opt/tros/humble/setup.bash
exec /usr/bin/python3 /usr/local/vigiclient/vigi-encode-pose.py "$@"
