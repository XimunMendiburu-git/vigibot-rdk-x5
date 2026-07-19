#!/usr/bin/env bash
# Post-install health checks — run on the RDK X5 board.
set -euo pipefail

FAIL=0

check() {
  if "$@"; then
    echo "OK  $*"
  else
    echo "FAIL $*"
    FAIL=1
  fi
}

echo "=== vigibot-rdk-x5 health-check ==="

if systemctl is-active --quiet vigiclient; then
  echo "OK  vigiclient service active"
else
  echo "FAIL vigiclient service not active"
  FAIL=1
fi

if pgrep -f clientrobotpi.js >/dev/null; then
  echo "OK  clientrobotpi.js running"
else
  echo "FAIL clientrobotpi.js not running"
  FAIL=1
fi

if pgrep -f 'vigi-encode-rdk|vigi-encode-yolo' >/dev/null; then
  echo "OK  video encoder process running"
else
  echo "WARN no vigi-encode process (may start on video connect)"
fi

if pgrep -f rdk-gpio-helper.py >/dev/null; then
  echo "OK  rdk-gpio-helper.py running"
else
  echo "WARN rdk-gpio-helper.py not running (starts on first GPIO command)"
fi

if ss -ltn 2>/dev/null | grep -q ':8043'; then
  echo "OK  TCP 8043 listening"
else
  echo "FAIL TCP 8043 not listening"
  FAIL=1
fi

if [[ -f /var/log/vigiclient.log ]]; then
  if tail -n 30 /var/log/vigiclient.log | grep -q 'open_cam failed'; then
    echo "FAIL recent open_cam failed in vigiclient.log"
    FAIL=1
  else
    echo "OK  no recent open_cam failed in log tail"
  fi
  echo "--- last 5 log lines ---"
  tail -n 5 /var/log/vigiclient.log
else
  echo "WARN /var/log/vigiclient.log not found"
fi

echo "=== end (exit ${FAIL}) ==="
exit "${FAIL}"
