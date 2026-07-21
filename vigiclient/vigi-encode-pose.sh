#!/usr/bin/env bash
set -e
# ROS setup.bash references optional unset variables.
set +u
source /opt/tros/humble/setup.bash
exec -a "$0" /usr/bin/python3 /usr/local/vigiclient/vigi-encode-pose.py "$@"
