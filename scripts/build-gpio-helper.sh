#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE="${REPO_ROOT}/vigiclient/rdk-gpio-helper.c"
OUTPUT="${1:-${REPO_ROOT}/vigiclient/rdk-gpio-helper}"

command -v gcc >/dev/null || {
  echo "gcc is required (install build-essential)." >&2
  exit 1
}

if [[ ! -f /usr/local/include/wiringPi.h ]]; then
  echo "WiringPi headers not found; run sudo install/install-wiringpi.sh." >&2
  exit 1
fi

mkdir -p "$(dirname "${OUTPUT}")"

gcc \
  -std=gnu11 \
  -O2 \
  -Wall \
  -Wextra \
  -Werror \
  -Wno-error=format-truncation \
  -pthread \
  -I/usr/local/include \
  -L/usr/local/lib \
  -Wl,-rpath,/usr/local/lib \
  "${SOURCE}" \
  -o "${OUTPUT}" \
  -lwiringPi \
  -lrt

chmod 0755 "${OUTPUT}"
echo "Built ${OUTPUT}"
