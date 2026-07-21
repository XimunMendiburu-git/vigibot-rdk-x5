#!/usr/bin/env bash
# Pull modified Vigibot files from a reference RDK X5 board into this repo.
# Run from repo root (Git Bash / WSL on Windows).
set -euo pipefail

BOARD_USER="${BOARD_USER:-sunrise}"
BOARD_HOST="${BOARD_HOST:-10.59.161.115}"
REMOTE="${REMOTE:-/usr/local/vigiclient}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOCAL_VIGI="${REPO_ROOT}/vigiclient"
LOCAL_CONFIG="${REPO_ROOT}/config"
LOCAL_SYSTEMD="${REPO_ROOT}/systemd"

REMOTE_SPEC="${BOARD_USER}@${BOARD_HOST}"
SSH_OPTS=(-o ConnectTimeout=10)

VIGI_FILES=(
  clientrobotpi.js
  vigi-encode-rdk.py
  vigi-encode-rdk.sh
  vigi-encode-yolo.py
  vigi-encode-yolo.sh
  vigi-encode-pose.py
  vigi-encode-pose.sh
  vigi-pose.launch.py
  rdk-pigpio.js
  rdk-gpio-helper.c
  rdk-gpio-helper.py
  rdk-i2c-bus.js
  rdk-pca9685.js
)

echo "==> Fetch vigiclient from ${REMOTE_SPEC}:${REMOTE}"
mkdir -p "${LOCAL_VIGI}"

for f in "${VIGI_FILES[@]}"; do
  echo "    ${f}"
  scp "${SSH_OPTS[@]}" "${REMOTE_SPEC}:${REMOTE}/${f}" "${LOCAL_VIGI}/${f}" 2>/dev/null || {
    echo "    WARN: missing ${f} on board (skipped)"
  }
done

echo "==> Fetch sys.json → config/sys.json.example (sanitized copy)"
if scp "${SSH_OPTS[@]}" "${REMOTE_SPEC}:${REMOTE}/sys.json" "${LOCAL_CONFIG}/sys.json.example"; then
  echo "    OK (review for secrets before commit)"
else
  echo "    WARN: sys.json not fetched"
fi

echo "==> Fetch systemd units"
mkdir -p "${LOCAL_SYSTEMD}/vigiclient.service.d"
scp "${SSH_OPTS[@]}" "${REMOTE_SPEC}:/etc/systemd/system/vigiclient.service" \
  "${LOCAL_SYSTEMD}/vigiclient.service" 2>/dev/null || echo "    WARN: vigiclient.service not fetched"

scp "${SSH_OPTS[@]}" "${REMOTE_SPEC}:/etc/systemd/system/vigiclient.service.d/encode.conf" \
  "${LOCAL_SYSTEMD}/vigiclient.service.d/encode.conf" 2>/dev/null || echo "    WARN: encode.conf not fetched"

for dropin in baseline.conf; do
  scp "${SSH_OPTS[@]}" "${REMOTE_SPEC}:/etc/systemd/system/vigiclient.service.d/${dropin}" \
    "${LOCAL_SYSTEMD}/vigiclient.service.d/${dropin}" 2>/dev/null || true
done

echo "==> Done. Review git diff — never commit robot.json with real credentials."
echo "    robot.json is NOT fetched (use config/robot.json.example as template)."
