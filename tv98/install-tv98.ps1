$ErrorActionPreference = "Stop"

$Adb = "C:\Users\User\Downloads\platform-tools-latest-windows\platform-tools\adb.exe"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

& $Adb push "$Root\realtime_server.py" /sdcard/realtime_server.py
& $Adb push "$Root\start-stackchan.sh" /sdcard/start-stackchan.sh

& $Adb shell su 0 cp /sdcard/realtime_server.py /data/data/com.termux/files/home/realtime_server.py
& $Adb shell su 0 mkdir -p /data/data/com.termux/files/home/.termux/boot
& $Adb shell su 0 cp /sdcard/start-stackchan.sh /data/data/com.termux/files/home/.termux/boot/start-stackchan.sh
& $Adb shell su 0 chmod 700 /data/data/com.termux/files/home/.termux/boot/start-stackchan.sh

Write-Host "TV98 files installed successfully."
