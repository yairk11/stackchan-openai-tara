# TARA Codex Working Rules

- This is a Windows + VS Code + PlatformIO project.
- Project environment: m5stack-cores3.
- Before running terminal commands, clear the integrated terminal first.
- Prefer one controlled task at a time.
- Inspect the relevant code before editing. Do not guess.
- Make the smallest safe change necessary.
- After code changes, run a PlatformIO build for m5stack-cores3.
- Do not commit or push unless explicitly requested.
- Check git diff and git status before and after significant changes.
- Never overwrite unrelated user changes.
- Do not open Serial Monitor automatically.
- CoreS3 upload port is COM5.
- Before upload, verify COM5 is available.
- Do not keep Serial Monitor open during reset or upload.
- Do not manually start tv98/realtime\_server.py.
- The TV98 server is started by Termux Boot.
- TV98 ADB executable:
  C:\Users\User\Downloads\platform-tools-latest-windows\platform-tools\adb.exe
- PlatformIO executable:
  C:\Users\User\.platformio\penv\Scripts\pio.exe
- Preserve the current stable servo behavior.
- Do not re-enable physical servo\_gesture or head\_motion handling unless explicitly requested.
- Do not call Motion.goHome() unless explicitly requested.
- Servo RX reads are unreliable on this hardware; avoid introducing blocking servo read loops.
- Do not change the working server\_vad configuration unless explicitly requested.
- Keep OpenAI Realtime output\_modalities as audio unless explicitly requested.
- Prefer safe reversible edits.
- When a command fails, inspect the actual error before trying another approach.
- Never claim success without verifying the result.
- For file edits, preserve existing line endings and avoid unnecessary formatting changes.
- Never expose or print secrets such as OPENAI\_API\_KEY.
