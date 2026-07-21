#!/usr/bin/env bash
# Deploy vigibot-rdk-x5 files to /usr/local/vigiclient on RDK X5.
# Run as root on the board from a cloned repo, or via: ssh board 'sudo bash -s' < install/install.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="/usr/local/vigiclient"
BACKUP=""
FORCE_CONFIG=0

usage() {
  echo "Usage: sudo $0 [--force-config]"
  echo "  --force-config  overwrite ${TARGET}/robot.json (default: preserve existing)"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --force-config) FORCE_CONFIG=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1"; usage; exit 2 ;;
  esac
done

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run as root (sudo)."
  exit 1
fi

if [[ "$(uname -m)" != "aarch64" ]]; then
  echo "WARN: expected aarch64 RDK board, got $(uname -m)"
fi

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1"
    exit 1
  fi
}

require_cmd node
require_cmd python3
require_cmd ffmpeg
require_cmd gcc
require_cmd git

if [[ ! -f /opt/hobot/model/x5/basic/yolov5s_v7_640x640_nv12.bin ]]; then
  echo "WARN: YOLO model not found at /opt/hobot/model/x5/basic/yolov5s_v7_640x640_nv12.bin"
fi

python3 -c "import Hobot.GPIO" 2>/dev/null || {
  echo "Missing Hobot.GPIO Python module"
  exit 1
}

if [[ ! -f "${TARGET}/clientrobotpi.js" ]]; then
  echo "ERROR: ${TARGET}/clientrobotpi.js not found."
  echo "Install the Vigibot client first, or run scripts/fetch-from-board.sh on your PC"
  echo "and redeploy including clientrobotpi.js."
  exit 1
fi

if [[ -d "${TARGET}" ]]; then
  BACKUP="${TARGET}.bak.$(date +%s)"
  echo "==> Backup ${TARGET} → ${BACKUP}"
  cp -a "${TARGET}" "${BACKUP}"
fi

mkdir -p "${TARGET}"

echo "==> Copy vigiclient/* → ${TARGET}/"
shopt -s nullglob
for f in "${REPO_ROOT}/vigiclient/"*; do
  base="$(basename "$f")"
  [[ "$base" == "README.md" ]] && continue
  if [[ -f "$f" ]]; then
    cp -a "$f" "${TARGET}/${base}"
  fi
done

chmod +x "${TARGET}/"*.sh 2>/dev/null || true
chmod +x "${TARGET}/"*.py 2>/dev/null || true
chmod +x "${REPO_ROOT}/install/"*.sh "${REPO_ROOT}/scripts/"*.sh 2>/dev/null || true

echo "==> Install WiringPi and build native GPIO helper"
bash "${REPO_ROOT}/install/install-wiringpi.sh"
bash "${REPO_ROOT}/scripts/build-gpio-helper.sh" "${TARGET}/rdk-gpio-helper"

echo "==> Install config examples (if missing)"
if [[ ! -f "${TARGET}/sys.json" ]]; then
  cp "${REPO_ROOT}/config/sys.json.example" "${TARGET}/sys.json"
  echo "    created ${TARGET}/sys.json from example"
else
  echo "    kept existing ${TARGET}/sys.json"
  python3 - <<'PY'
import json
from pathlib import Path
path = Path("/usr/local/vigiclient/sys.json")
data = json.loads(path.read_text())
wanted = [
    "/usr/local/vigiclient/vigi-encode-pose.sh ",
    "WIDTH ",
    "HEIGHT ",
    "FPS ",
    "BITRATE",
]
cmds = data.setdefault("CMDDIFFUSION", [])
if len(cmds) < 3:
    cmds.append(wanted)
    path.write_text(json.dumps(data, indent=1) + "\n")
    print("    appended pose CMDDIFFUSION entry")
elif cmds[2] != wanted:
    cmds[2] = wanted
    path.write_text(json.dumps(data, indent=1) + "\n")
    print("    updated pose CMDDIFFUSION entry")
else:
    print("    pose CMDDIFFUSION entry already present")
PY
fi

if [[ ! -f "${TARGET}/robot.json" ]] || [[ "${FORCE_CONFIG}" -eq 1 ]]; then
  if [[ "${FORCE_CONFIG}" -eq 1 && -f "${TARGET}/robot.json" ]]; then
    cp "${TARGET}/robot.json" "${TARGET}/robot.json.bak.$(date +%s)"
  fi
  cp "${REPO_ROOT}/config/robot.json.example" "${TARGET}/robot.json"
  echo "    installed robot.json from example — EDIT NAME/PASSWORD before production"
else
  echo "    kept existing ${TARGET}/robot.json"
fi

echo "==> Python syntax check"
python3 -m py_compile "${TARGET}/vigi-encode-rdk.py"
python3 -m py_compile "${TARGET}/vigi-encode-yolo.py"
python3 -m py_compile "${TARGET}/vigi-encode-pose.py"
python3 -m py_compile "${TARGET}/vigi-pose.launch.py"
python3 -m py_compile "${TARGET}/rdk-gpio-helper.py"

echo "==> Install systemd units"
cp "${REPO_ROOT}/systemd/vigiclient.service" /etc/systemd/system/vigiclient.service
mkdir -p /etc/systemd/system/vigiclient.service.d
if [[ -f "${REPO_ROOT}/systemd/vigiclient.service.d/encode.conf" ]]; then
  cp "${REPO_ROOT}/systemd/vigiclient.service.d/encode.conf" \
    /etc/systemd/system/vigiclient.service.d/encode.conf
fi
systemctl daemon-reload

echo "==> Enable and restart vigiclient"
systemctl enable vigiclient
systemctl restart vigiclient
sleep 3

if [[ -x "${REPO_ROOT}/scripts/health-check.sh" ]]; then
  bash "${REPO_ROOT}/scripts/health-check.sh" || true
fi

echo ""
echo "Install complete."
echo "  Edit ${TARGET}/robot.json if using example credentials."
echo "  Logs: tail -f /var/log/vigiclient.log"
echo "  Runbook: ${REPO_ROOT}/docs/known-issues.md"
