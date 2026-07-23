#!/bin/bash
set -e
echo sunrise | sudo -S bash -c '
set -e
systemctl stop vigiclient 2>/dev/null || true
sleep 1
# Strip Windows CRLF — shebang 'bash\r' breaks exec via Node.
tr -d '\r' < /tmp/vigi-encode-pose.sh > /usr/local/vigiclient/vigi-encode-pose.sh
chmod +x /usr/local/vigiclient/vigi-encode-pose.sh
# Ensure LF on C++ sources copied from Windows hosts.
tr -d '\r' < /tmp/vigi-encode-pose.cpp > /usr/local/vigiclient/vigi-encode-pose.cpp
tr -d '\r' < /tmp/pose_post_process.cpp > /usr/local/vigiclient/pose_post_process.cpp
tr -d '\r' < /tmp/pose_post_process.hpp > /usr/local/vigiclient/pose_post_process.hpp

INC=/home/sunrise/x5-hobot-spdev/src/clang
g++ -O2 -std=c++17 \
  /usr/local/vigiclient/vigi-encode-pose.cpp \
  /usr/local/vigiclient/pose_post_process.cpp \
  -o /usr/local/vigiclient/vigi-encode-pose \
  -I"$INC" -I/usr/local/vigiclient \
  -lx264 -lspcdev -ldnn -lopencv_world -lpthread

chmod +x /usr/local/vigiclient/vigi-encode-pose
ls -l /usr/local/vigiclient/vigi-encode-pose
systemctl start vigiclient
'
echo "build OK"
