#!/bin/bash
set -e
echo sunrise | sudo -S bash -c '
systemctl stop vigiclient
cp /tmp/clientrobotpi.js /usr/local/vigiclient/clientrobotpi.js
systemctl start vigiclient
'
sleep 12
echo sunrise | sudo -S journalctl -u vigiclient --since "20 sec ago" --no-pager | grep -E "VIDEO_|FLUSH|Started|Stopped" | tail -20
grep -n "hardFlush\|destroy\|VIDEO_UPLINK_FLUSH" /usr/local/vigiclient/clientrobotpi.js || echo "no destroy/flush left"
