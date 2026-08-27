# TARA Project

## Goal
TARA is a self-contained embodied desktop robot based on CoreS3 + TV98 + OpenAI Realtime.

Main goals:
- Natural Hebrew voice conversation.
- Fast response latency.
- Natural head gestures and persistent look anchor.
- Emotion / LED / face states.
- Persistent memory across sessions and reboots.
- Automatic startup after power loss.
- Automatic recovery from OpenAI session expiry, Wi-Fi issues and WebSocket reconnects.
- Normal use should require no manual commands after power-on.

## Hardware and Network
- CoreS3 IP: 10.0.0.59
- TV98 IP: 10.0.0.210
- Realtime proxy port: 8002
- ADB: 10.0.0.210:5555

## Windows Project
Path:
C:\Users\User\Documents\PlatformIO\Projects\stackchan-openai-tara

Main firmware:
src\main.cpp

## TV98 / Termux
Home:
/data/data/com.termux/files/home

Main server:
/data/data/com.termux/files/home/realtime_server.py

Environment:
/data/data/com.termux/files/home/stackchan.env

Log:
/data/data/com.termux/files/home/server.log

Persistent memory:
/data/data/com.termux/files/home/tara_memory.json

Python:
/data/data/com.termux/files/usr/bin/python

## OpenAI Realtime
- Model: gpt-realtime-2.1
- CoreS3 input: PCM16 / 16000 Hz
- OpenAI input: PCM16 / 24000 Hz
- OpenAI output: PCM16 / 24000 Hz
- Audio pacing: 48000 bytes/sec
- Audio packet: 4096 bytes
- Input queue: 256

## What Already Works
- CoreS3 audio streaming.
- OpenAI Realtime conversation.
- Hebrew replies.
- Touch: one touch = one turn.
- Turn sequencing.
- Head gestures.
- Emotions.
- Persistent memory via tara_memory.json.
- Termux:Boot automatic server startup.
- Full power-cycle recovery was tested successfully.
- OpenAI session_expired handling added.
- CoreS3 reconnect interval is 2000 ms.
- Dead OpenAI task guard added.
- Latency prompt improved so simple factual replies do not require a gesture first.

## Persistent Memory
Tool:
remember_fact

Memory is loaded into new OpenAI sessions from tara_memory.json.

Verified:
A remembered fact survived server/session restart.

Permissions:
tara_memory.json should be:
u0_a65:u0_a65
mode 600

## Language Behavior
Current instruction:
Always reply in the same language as the latest spoken user message.
If the user speaks Hebrew, reply in Hebrew.
Switch languages only when explicitly asked.

## Emotion Goal
Desired flow:
- listening while user speaks
- thinking = yellow after speech_stopped
- speaking only when actual audio begins
- neutral after turn completes

Thinking patch already applied:
speakerStartRequested = true was removed from response_started.
It remains triggered when the first binary audio packet arrives.

Build and upload succeeded after this patch.

## Current Main Bug
Duplicate/stale WebSocket connections.

Observed:
Multiple ESTAB connections from the same CoreS3 to TV98:8002.

Examples:
- 2 simultaneous ESTAB connections
- later 3 simultaneous ESTAB connections

When this happens CoreS3 audio send can stall badly:
send around 10000 ms
result=FAIL

Temporary cleanup:
pkill -f realtime_server.py

This is only a workaround.

## Next Technical Priority
Fix duplicate/stale WebSocket connections properly.

Investigate in realtime_server.py:
handle_stackchan(stackchan_ws)

Questions:
- Why does an old StackChan connection stay ESTAB after reconnect/reset?
- Does the old handler remain alive?
- Are sender/receiver tasks cancelled correctly?
- Is stackchan_ws explicitly closed in finally?
- Should there be a single-active-client policy?
- Should the previous Core connection be closed when a new one arrives?
- Should server-side ping/timeout settings be added or tightened?

Desired solution:
At most one active CoreS3 WebSocket connection at any time.
When a new CoreS3 connection arrives:
- close the old one
- cancel its tasks
- clear its queues
- keep only the new connection active

Do not break:
- audio pacing
- turn sequencing
- reconnect logic
- memory
- tools
- emotions

After this bug is fixed:
re-test thinking yellow behavior.

## Important Existing Patches
- touch dedup patch
- turn finishing patch
- OpenAI dead-task guard
- session_expired reconnect patch
- Hebrew language prompt
- remember_fact tool
- persistent memory injection
- thinking-until-first-audio patch

Do not refactor working areas unnecessarily.

## Git
Current working baseline commit:
d91fbda

Message:
TARA working baseline: OpenAI Realtime, audio, emotions and reconnect

Do not push to origin yet.
Current origin points to:
https://github.com/taranton/stackchan-gemini-firmware.git

## Checkpoints
Known checkpoints:
C:\Users\User\Desktop\TARA_CHECKPOINT_20260826-083425
C:\Users\User\Desktop\TARA_CHECKPOINT_20260826-084709
C:\Users\User\Desktop\TARA_CHECKPOINT_20260826-220030
C:\Users\User\Desktop\TARA_CHECKPOINT_20260826-220438

Latest known manual checkpoint:
C:\Users\User\Desktop\TARA_CHECKPOINT_20260826-220438

Note:
The thinking-until-first-audio patch was made after that checkpoint.

## Terminal Names
Use these names:
- CoreS3 - Serial Monitor
- PC - PowerShell
- TV98

Always verify environment by prompt:
- PS C:\... = Windows PowerShell
- android_tv_box:/ $ = Android shell
- android_tv_box:/ # = Android root shell
- android_tv_box:/data/data/com.termux/files/home # = Termux home as root

Never give PowerShell commands inside Android shell.
Never give Android shell commands inside PowerShell.

## Working Style
- One command/action at a time.
- Wait for output before continuing.
- Start PowerShell command blocks with Clear-Host.
- Prefer short targeted grep/sed/Select-String output.
- Do not request huge logs unless necessary.
- Inspect code before patching.
- After patch: syntax/build check.
- After build: upload.
- After stable milestone: Git commit / checkpoint.
- Avoid large refactors unless necessary.
