#!/bin/bash
set -e
echo sunrise | sudo -S bash -c '
set -e
systemctl stop vigiclient 2>/dev/null || true
sleep 1
cp /tmp/vigi-encode-yolo.cpp /usr/local/vigiclient/vigi-encode-yolo.cpp
cp /tmp/yolov5_post_process.cpp /usr/local/vigiclient/yolov5_post_process.cpp
cp /tmp/yolov5_post_process.hpp /usr/local/vigiclient/yolov5_post_process.hpp
cp /tmp/vigi-encode-yolo.sh /usr/local/vigiclient/vigi-encode-yolo.sh
chmod +x /usr/local/vigiclient/vigi-encode-yolo.sh

INC=/home/sunrise/x5-hobot-spdev/src/clang
g++ -O2 -std=c++17 \
  /usr/local/vigiclient/vigi-encode-yolo.cpp \
  /usr/local/vigiclient/yolov5_post_process.cpp \
  -o /usr/local/vigiclient/vigi-encode-yolo \
  -I"$INC" -I/usr/local/vigiclient \
  -lx264 -lspcdev -ldnn -lopencv_world -lpthread

chmod +x /usr/local/vigiclient/vigi-encode-yolo
ls -l /usr/local/vigiclient/vigi-encode-yolo
systemctl start vigiclient
'
sleep 8
echo sunrise | sudo -S journalctl -u vigiclient --since "20 sec ago" --no-pager | grep -iE "yolo|encode|Started|failed|error" | tail -25
pgrep -af "vigi-encode-yolo|vigi-encode-x264" || true
