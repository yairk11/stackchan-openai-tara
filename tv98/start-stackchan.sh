#!/data/data/com.termux/files/usr/bin/bash

export PATH=/data/data/com.termux/files/usr/bin:/system/bin:/system/xbin
export HOME=/data/data/com.termux/files/home
export TMPDIR=/data/data/com.termux/files/usr/tmp

cd /data/data/com.termux/files/home

set -a
. ./stackchan.env
set +a

sleep 15

exec /data/data/com.termux/files/usr/bin/python -u /data/data/com.termux/files/home/realtime_server.py >> /data/data/com.termux/files/home/server.log 2>&1
