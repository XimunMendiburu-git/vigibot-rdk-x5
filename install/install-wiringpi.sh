#!/usr/bin/env bash
set -euo pipefail

WIRINGPI_REPO="${WIRINGPI_REPO:-https://gitee.com/xin03liu/WiringPi.git}"
WIRINGPI_COMMIT="${WIRINGPI_COMMIT:-59cc2e82c7a92d2e4ce5bb8e732ebbfe5512ec3f}"
SOURCE_DIR="${SOURCE_DIR:-/opt/wiringpi-rdk}"

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run as root (sudo)." >&2
  exit 1
fi

if [[ -f /usr/local/include/wiringPi.h ]] &&
   ldconfig -p | grep -q 'libwiringPi\.so'; then
  echo "WiringPi is already installed."
  exit 0
fi

command -v git >/dev/null || {
  echo "git is required to install WiringPi." >&2
  exit 1
}
command -v gcc >/dev/null || {
  echo "gcc/build-essential is required to install WiringPi." >&2
  exit 1
}

if [[ -d "${SOURCE_DIR}/.git" ]]; then
  git -C "${SOURCE_DIR}" fetch --all --tags
else
  rm -rf "${SOURCE_DIR}"
  git clone "${WIRINGPI_REPO}" "${SOURCE_DIR}"
fi

git -C "${SOURCE_DIR}" checkout --detach "${WIRINGPI_COMMIT}"

echo "==> Building RDK WiringPi at ${WIRINGPI_COMMIT}"
(
  cd "${SOURCE_DIR}"
  ./build
)
ldconfig

test -f /usr/local/include/wiringPi.h
ldconfig -p | grep 'libwiringPi\.so'
echo "WiringPi installation complete."
