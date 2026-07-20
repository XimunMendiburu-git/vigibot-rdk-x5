#!/bin/bash
export LD_LIBRARY_PATH=/usr/hobot/lib:/usr/lib
export VIGI_USE_FFMPEG=1
exec /usr/bin/python3 /usr/local/vigiclient/vigi-encode-rdk.py "$@"
