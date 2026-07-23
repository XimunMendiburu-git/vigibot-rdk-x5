#!/usr/bin/env bash
# Build and install rdk-gpio-helper on the RDK (run ON the board as sunrise, or via ssh).
set -euo pipefail

SRC="${1:-/tmp/rdk-gpio-helper.c}"
OUT="/usr/local/vigiclient/rdk-gpio-helper"

if [[ ! -f "$SRC" ]]; then
  echo "missing source: $SRC" >&2
  exit 1
fi

tr -d '\r' < "$SRC" > /tmp/rdk-gpio-helper.lf.c
gcc -std=gnu11 -O2 -Wall -Wextra -Wno-error=format-truncation -pthread \
  -I/usr/local/include -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
  /tmp/rdk-gpio-helper.lf.c -o /tmp/rdk-gpio-helper.new -lwiringPi -lrt

sudo cp /tmp/rdk-gpio-helper.lf.c /usr/local/vigiclient/rdk-gpio-helper.c
sudo cp /tmp/rdk-gpio-helper.new "$OUT"
sudo chmod 755 "$OUT"
sudo systemctl restart vigiclient
sleep 2
systemctl is-active vigiclient
pgrep -af rdk-gpio-helper || true
strings "$OUT" | grep -E 'hyst=|ready backend' || true
echo "deploy OK: $OUT"
