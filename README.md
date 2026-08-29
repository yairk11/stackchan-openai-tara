# TARA V1

TARA is an embodied desktop robot built around:

- M5Stack CoreS3 / StackChan
- TV98 Android box running Termux
- OpenAI Realtime API
- PlatformIO on Windows

The current V1 architecture uses the CoreS3 for the robot hardware and audio interface, while the TV98 acts as the local realtime server between the robot and OpenAI.

## Current Status

TARA V1 is working and has been tested with repeated realtime conversations.

Verified features include:

- Hebrew voice conversation by default
- OpenAI Realtime responses
- CoreS3 microphone streaming
- Speaker playback
- Touch-to-talk interaction
- Head gestures
- Emotion / face states
- Persistent memory
- Automatic TV98 startup through Termux:Boot
- Recovery after power cycle
- OpenAI session recovery
- WebSocket reconnect handling

## Architecture

```text
CoreS3 / StackChan
        |
        | WebSocket / PCM16 audio
        v
TV98 / Termux
realtime_server.py
        |
        | OpenAI Realtime API
        v
OpenAI
```

CoreS3 microphone audio is recorded at 16 kHz PCM16.

The TV98 server converts the input stream for OpenAI Realtime and sends OpenAI audio responses back to the CoreS3 at 24 kHz PCM16.

## Hardware

Current V1 hardware:

- M5Stack CoreS3
- StackChan-compatible servo hardware
- TV98 Android box
- Wi-Fi network
- Windows PC for development and flashing

## Repository Layout

```text
src/
    main.cpp

tv98/
    realtime_server.py
    start-stackchan.sh
    stackchan.env.example

lib/
    StackChan-BSP/
    WebSockets/

docs/
examples/
TARA_PROJECT.md
platformio.ini
```

## CoreS3 Firmware

The firmware is built with PlatformIO.

PlatformIO environment: `m5stack-cores3`

Build: `pio run -e m5stack-cores3`

Flash: `pio run -e m5stack-cores3 -t upload`

The Wi-Fi credentials used by the current development firmware are stored locally in `src/secrets.h`.

This file is excluded from Git and must not be committed.

## TV98 Server

The main TV98 server is `tv98/realtime_server.py`.

On the TV98 it runs from `/data/data/com.termux/files/home/realtime_server.py`.

The server listens for the CoreS3 WebSocket connection on port `8002`.

Current OpenAI Realtime model: `gpt-realtime-2.1`.

## OpenAI API Key

The OpenAI API key is not stored in the repository.

On the TV98, create `/data/data/com.termux/files/home/stackchan.env` from `tv98/stackchan.env.example`.

Example:

```text
OPENAI_API_KEY=YOUR_OPENAI_API_KEY_HERE
```

Never commit a real API key.

## Automatic TV98 Startup

The repository includes `tv98/start-stackchan.sh`.

The production startup flow uses Termux:Boot. The script loads `stackchan.env`, waits for Android networking to initialize, starts `realtime_server.py`, and writes server output to `server.log`.

TV98 runtime files:

```text
/data/data/com.termux/files/home/realtime_server.py
/data/data/com.termux/files/home/stackchan.env
/data/data/com.termux/files/home/server.log
/data/data/com.termux/files/home/tara_memory.json
```

## Language Behavior

TARA replies in Hebrew by default. It switches to another language only when the user explicitly asks it to speak that language.

## Persistent Memory

TARA supports persistent memory through `tara_memory.json`.

The OpenAI tool used to save remembered information is `remember_fact`.

Stored memory is reloaded into new OpenAI sessions. Sensitive information such as passwords, API keys, financial information, and similar secrets should not be stored in memory.

## Audio

```text
CoreS3 microphone: PCM16 / 16000 Hz
OpenAI input:      PCM16 / 24000 Hz
OpenAI output:     PCM16 / 24000 Hz
```

The TV98 server performs the required input conversion.

## WebSocket Stability

TARA uses a vendored copy of the WebSockets library under `lib/WebSockets/`.

The V1 build contains a stability patch that increases the ESP32 WebSocket write timeout from 500 ms to 2000 ms. The previous 500 ms timeout caused valid audio transmissions to be aborted during short network stalls.

## Development Environment

Main development environment:

- Windows
- VS Code
- PlatformIO
- PowerShell
- ADB for TV98 management

Main firmware: `src/main.cpp`

Main TV98 server: `tv98/realtime_server.py`

## Project State

Detailed engineering notes and current project state are kept in `TARA_PROJECT.md`.

## V1 Goal

TARA V1 is intended to operate without manual intervention after power-on:

1. TV98 boots
2. Termux:Boot starts the realtime server
3. CoreS3 connects to Wi-Fi
4. TARA is ready for conversation
5. Reconnect and OpenAI session recovery happen automatically

## Security

Do not commit:

- `src/secrets.h`
- `stackchan.env`
- OpenAI API keys
- Wi-Fi passwords
- other runtime secrets

Use example/template files for public configuration.

## License

See `LICENSE`.
