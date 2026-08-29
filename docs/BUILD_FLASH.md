# Build, Flash and Install TARA V1

This guide describes the current TARA V1 setup for:

- Windows PC
- M5Stack CoreS3 / StackChan
- TV98 Android box with Termux
- OpenAI Realtime API

## 1. Prerequisites

On Windows, install:

- Visual Studio Code
- PlatformIO
- Python 3
- ADB platform tools
- USB drivers for the CoreS3 if required

The TV98 requires:

- Termux
- Termux:Boot
- Python inside Termux
- network access to OpenAI

## 2. Configure Wi-Fi

The current V1 firmware uses a local secrets file:

```text
src/secrets.h
```

Create it locally with the required Wi-Fi values.

Example:

```cpp
#pragma once

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

This file is ignored by Git.

Never commit real Wi-Fi credentials.

## 3. Build the CoreS3 Firmware

From the repository root in PowerShell:

```powershell
pio run -e m5stack-cores3
```

A successful build creates:

```text
.pio/build/m5stack-cores3/firmware.bin
```

## 4. Flash the CoreS3


Connect the CoreS3 to the Windows PC by USB.

Then run:

```powershell
pio run -e m5stack-cores3 -t upload
```

If multiple serial ports are present, specify the port explicitly.

Example:

```powershell
pio run -e m5stack-cores3 -t upload --upload-port COM5
```

## 5. Prepare the TV98 Environment

On the TV98, TeRmux home is:

```text
/data/data/com.termux/files/home
```

The TARA runtime uses:

```text
/data/data/com.termux/files/home/realtime_server.py
/data/data/com.termux/files/home/stackchan.env
/data/data/com.termux/files/home/server.log
/data/data/com.termux/files/home/tara_memory.json
```

## 6. Install the TV98 Server

Copy the repository file:

```text
tv98/realtime_server.py
```

to:

```text
/data/data/com.termux/files/home/realtime_server.py
```

The server uses the OpenAI Realtime API.

## 7. Configure the OpenAI API Key

On the TV98, create:

```text
/data/data/com.termux/files/home/stackchan.env
```

Start from the repository example:

```text
tv98/stackchan.env.example
```

Example:

```text
OPENAI_API_KEY=YOUR_OPENAI_API_KEY_HERE
```

Never commit a real OpenAI API key.

## 8. Configure Automatic Startup

TARA V1 uses Termux:Boot to start the server automatically after the TV98 boots.

The repository includes:

```text
tv98/start-stackchan.sh
```

It should be installed as the Termux:Boot startup script.

The script:

- loads `stackchan.env`
- waits 15 seconds for networking
- starts `realtime_server.py`
- appends output to `server.log`

## 9. Verify the TV98 Server

After startup, the server should listen on port:

```text
8002
```

The server log file is:

```text
/data/data/com.termux/files/home/server.log
```

## 10. Serial Monitor

To view CoreS3 logs from Windows:

```powershell
pio device monitor -b 115200
```

## 11. First Full Test

After both devices are ready:

1. Power on the TV98.
2. Wait for Termux:Boot to start the server.
3. Power on the CoreS3.
4. Wait for Wi-Fi connection.
5. Touch TARA to start a turn.
6. Speak normally.
7. VErify that TARA replies in Hebrew.
8. Verify that speaker playback completes.

## 12. Fresh Checkout Verification

Before a release, verify that a clean clone builds without depending on local cache.

On Windows, a fresh copy of the repository should be able to run:

```powershell
pio run -e m5stack-cores3
```

## 13. Security Checklist

Before publishing:

- `stackchan.env` is not committed
- `src/secrets.h` is not committed
- no real OpenAI API key is committed
- no real Wi-Fi password is committed
- runtime logs are not committed
