# M5ClaudeWatch

**English** | [日本語](README.ja.md)

Turns an M5Stack StopWatch into a desk companion for Claude Code.
Inspired by [GOROman/vibewatch](https://github.com/GOROman/vibewatch), and wired directly into Claude Code hooks.

> This is an unofficial project and is not affiliated with Anthropic.

- **Working**: a spinning blue ring and a stopwatch showing elapsed time
- **Done**: vibrates, then shows how long the turn took and Claude's last message
- **Waiting for your reply**: when Claude's last message ends with a question or a request, the screen turns yellow, vibrates three short times, and shows the question
- **Waiting for input**: notifies you when Claude is waiting for input
- **Permission request**: shows the tool name and what it wants to do (command or file name); answer **OK / NG** with the buttons or the touch screen
  - Left button (BtnA) or NG on screen → deny (Claude stops and waits for instructions)
  - Right button (BtnB) or OK on screen → allow
  - Long-press either button → don't answer on the watch; use the usual dialog on your computer
  - Re-alerts every 30 seconds until answered

| Idle | Done | Waiting for reply | Permission request |
|:---:|:---:|:---:|:---:|
| <img src="docs/images/idle.jpg" width="200" alt="Idle screen"> | <img src="docs/images/done.jpg" width="200" alt="Done screen with elapsed time and Claude's last message"> | <img src="docs/images/reply.jpg" width="200" alt="Waiting-for-reply screen showing Claude's question"> | <img src="docs/images/permission.jpg" width="200" alt="Permission request screen with OK / NG buttons"> |

> **Note:** The watch UI and messages are currently Japanese only. Reply detection works for English questions ending with "?", but not for requests like "Please …". Contributions are welcome.

```
Claude Code ──hooks──▶ m5claudewatch_hook.py ──Unix socket──▶ m5claudewatch_bridge.py ──BLE / USB──▶ StopWatch
                       (stdlib only)                          (resident, bleak)                       (firmware/)
```

## Requirements

- [M5Stack StopWatch](https://docs.m5stack.com/) (ESP32-S3, 466×466 round display)
- macOS (launchd starts the bridge at login; hooks run with `/usr/bin/python3`)
- [PlatformIO](https://platformio.org/) (to flash the firmware)
- [uv](https://docs.astral.sh/uv/) (to run the bridge; it installs bleak / pyserial automatically)
- Claude Code

## Layout

| Path | Contents |
|---|---|
| `firmware/` | StopWatch firmware (PlatformIO / M5Unified / NimBLE) |
| `host/m5claudewatch_bridge.py` | Bridge that runs on your Mac and talks to the watch over BLE (or serial) |
| `host/m5claudewatch_hook.py` | Script that Claude Code hooks call |
| `hooks/settings.example.json` | Hook settings template |
| `install-hooks.py` | Merges the hooks into `~/.claude/settings.json` |
| `install-launchd.sh` | Starts the bridge automatically at login |

## Setup

### 1. Flash the firmware

Connect the StopWatch over USB-C.

```sh
cd firmware
pio run -e m5stack-stopwatch --target upload
```

### 2. Start the bridge

```sh
uv run host/m5claudewatch_bridge.py          # BLE: finds "M5ClaudeWatch" and connects automatically
# or, to stay connected over USB
uv run host/m5claudewatch_bridge.py --serial /dev/cu.usbmodemXXXX
```

The first time, macOS asks to allow Bluetooth access for your terminal app.
The address of the watch you connect to is saved in `~/.config/m5claudewatch/config.json`; from then on the bridge connects only to that watch.

### 3. Register the hooks

```sh
python3 install-hooks.py               # adds to ~/.claude/settings.json (backup: settings.json.bak)
python3 install-hooks.py --uninstall   # removes them
```

The hooks take effect in Claude Code sessions started after you register them.

### 4. Start the bridge at login (optional)

```sh
./install-launchd.sh                   # BLE (you can also pass --serial PORT)
./install-launchd.sh --uninstall       # remove
tail -f ~/Library/Logs/m5claudewatch-bridge.log
```

When the bridge runs from launchd, it may need its own Bluetooth permission, separate from your terminal's. If `watch connected` never appears in the log, check System Settings > Privacy & Security > Bluetooth.
**If the bridge is not running or the watch is not connected, the hooks do nothing**, so Claude Code works as usual.

## Configuration

| Environment variable | Default | Meaning |
|---|---|---|
| `M5CLAUDEWATCH_PERM_TIMEOUT` | `110` | Seconds to wait for an answer on the watch before falling back to the dialog on your computer (keep it below the hook timeout of 120 s) |
| `M5CLAUDEWATCH_SOCKET` | `~/.claude/m5claudewatch.sock` | Path of the bridge's socket |

Use the same values for the hooks (the shell that launches Claude Code) and for the bridge. A bridge started by launchd does not see your shell's environment variables, so to change the wait time pass it as an argument, e.g. `./install-launchd.sh --perm-timeout 60`.

To enable sound effects, set `kSoundEnabled` to `true` in [firmware/src/main.cpp](firmware/src/main.cpp) and flash again (the default is vibration only).

## Protocol

Newline-delimited JSON over a Nordic UART–compatible GATT service (`6E400001-…`) or USB serial (115200 bps).
Details are at the top of [firmware/src/main.cpp](firmware/src/main.cpp).

## Notes

- Whether the dialog on your computer also appears while a permission request is pending may depend on your Claude Code version. If you can't answer on the watch, long-press to hand it to your computer.
- BLE currently has no pairing (no encryption). The bridge connects only to the watch address it saved, but if you want stricter security, use USB serial.

## License

[MIT](LICENSE)

The board settings in [firmware/platformio.ini](firmware/platformio.ini) are based on [GOROman/vibewatch](https://github.com/GOROman/vibewatch) (MIT License, Copyright (c) 2026 GOROman).
