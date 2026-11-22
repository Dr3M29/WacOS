# WacOS

**macOS Tahoe's look and feel, on Windows 11.** A liquid glass menu bar, Dock, Control Center,
Dynamic Island, Spotlight, Notification Center, System Settings and a lock screen with Face ID
through your webcam, drawn on top of the Windows you already have. Your apps, files and drivers
don't change; quit WacOS and the Windows taskbar is back.

WacOS = Windows + macOS. The shell itself is called **Liquid** (`Liquid.exe`), after the glass.

> Not affiliated with Apple. macOS, Mac and Tahoe are Apple's trademarks. WacOS ships no Apple
> artwork, fonts or wallpapers: the glass is rendered from scratch, icons come from your apps, and
> the wallpaper is whatever yours is.

## What's in it

**Liquid glass.** Every panel is real refracting glass: a D3D11 shader reads the live desktop
behind it (DXGI Desktop Duplication), bends it through a curved bezel, frosts it and lights the
rim. It isn't a blur filter; text and windows behind the Dock visibly bend at the edges.

**Menu bar** (on every monitor)
- The ✦ menu (macOS's Apple menu): About This PC, System Settings, App Store (Microsoft Store),
  Force Quit (Task Manager), Sleep, Restart, Shut Down, Lock Screen, Log Out
- The active app's name and its real menus: classic apps' own menus redrawn in glass, and
  macOS-style File / Edit / View / Window / Help for everything else
- Spotlight, Wi-Fi, volume, Control Center and the clock (Notification Center)

**Dynamic Island** in the middle of the bar
- Now playing: album art and a live equalizer; hover for controls
- New notifications drop into it for a few seconds; click to open the app
- Face ID: on the lock screen it grows into the Face ID square and checks when it's you
- Claude Code: shows when Claude is working or waiting for you (optional, see below)

**Dock**: magnification, bouncing launches, running-app dots, minimise-into-the-Dock animations,
right-click menus, auto-hide, four icon styles (Tahoe silver, dark, clear, original). Pins come
from your Windows taskbar unless you set your own.

**Control Center**: Wi-Fi (scan and connect), Bluetooth, Ethernet, volume and output device,
Now Playing.

**Notification Center**: your Windows notifications as glass cards, with the date, a clock and a
calendar. Banners replace Windows' toasts.

**Spotlight** on Alt+Space (or a tap of the Windows key): apps, files, actions, clipboard
history, a calculator and web search, with Tahoe's buttons to narrow it to one of them.

**System Settings**, laid out like macOS: Wi-Fi, Bluetooth, Network, Energy, General (About,
Login Items...), Accessibility, Appearance, Control Center, Desktop & Dock, Displays, Menu Bar,
Spotlight, Wallpaper, Notifications, Sound, Focus, Lock Screen, Privacy & Security, Face ID &
Password, Users & Groups, Air Gestures, Gaming, Apps, Keyboard, Mouse, Printers & Scanners.
WacOS's own settings are switches right there; Windows' settings are rows that open the matching
Windows Settings page, so everything is in one place.

**About This PC**: what the machine is and what's in it (processor, graphics, memory, storage,
Windows version), read from the hardware, with your wallpaper on the little screen.

**Lock screen**: wallpaper, big clock, your account picture and a PIN field. Locks at start, on
wake, after idle time, and when you walk away.

**Face ID and walk-away lock** (Air Ring, below): the webcam unlocks the lock screen when it
sees your face, locks the PC when you leave the desk, and pauses your music while you're gone.

**Air gestures** (Air Ring): pinch and move your hand up or down for volume; swipe an open hand
for the next or previous song.

**Windows**: Mac traffic-light buttons, Windows' caption buttons hidden on classic apps,
frosted title bars, rounded corners, a Cmd+Tab-style app switcher on Alt+Tab, and Spaces: a
maximised window gets its own space; right-drag left or right to move between spaces.

**Desktop**: liquid glass widgets (clock, calendar, music, system, and more; right-click the
desktop > Edit Widgets), macOS-style desktop icons, and a glass right-click menu with the real
Windows commands.

**Volume HUD**: the volume keys show a small glass bubble instead of Windows' flyout.

**Liquid Keys** (optional): passkeys kept on this PC and unlocked with Face ID, for Chromium
browsers. See [keys/README.md](keys/README.md).

**Remote viewing**: when you stream this PC with Moonlight (Sunshine), RustDesk, Axiom or Remote
Desktop, WacOS shows up in the stream and keeps its glass, rebuilt from window captures.

## Requirements

- Windows 11 (developed on build 26300, multi-monitor, mixed sizes and a vertical screen)
- A DirectX 11 GPU
- To build: Visual Studio 2022 or later (the free Community edition is fine) with the
  **Desktop development with C++** workload
- For Face ID, walk-away lock and air gestures: a webcam and **Python 3.12**
  (`winget install Python.Python.3.12`)

## Install

```powershell
git clone https://github.com/Dr3M29/WacOS.git
cd WacOS
powershell -ExecutionPolicy Bypass -File tools\setup.ps1
```

`setup.ps1` builds `Liquid.exe`, makes Air Ring's Python environment, downloads the five models
Air Ring runs (about 53 MB, from Google's MediaPipe and OpenCV's model zoo), sets WacOS to start
when you sign in, and starts it. Run it again after pulling an update; it only redoes what's
missing. `-NoAirRing` skips the webcam part; `-NoStart` doesn't start it.

**First run**: the lock screen asks you to choose a PIN. Then, for Face ID: System Settings >
Face ID & Password > Set Up Face ID, and look at the camera for about 20 seconds.

**Notifications** need Windows' permission once: System Settings > Notifications shows whether
WacOS can read them, with a link to the Windows switch.

## Using it

| Keys | What |
|---|---|
| Alt+Space, or tap the Windows key | Spotlight |
| Alt+Tab | App switcher (Q quits the selected app, Esc cancels) |
| Win+L | Lock |
| Ctrl+Win+F | Make the active window borderless full screen, or back |
| Right-drag left / right | Previous / next space |
| Right-drag up / down | Task View / show the desktop |
| Ctrl+Alt+Shift+S | Snapshot of the screen with WacOS on it (saved in `snapshots\`) |
| Ctrl+Alt+Shift+R | Reload Liquid |
| Ctrl+Alt+Shift+Q | Quit Liquid (the Windows taskbar comes back) |

WacOS keeps itself out of normal screenshots and screen recordings (that's how the glass can see
the windows behind it without seeing itself), so use Ctrl+Alt+Shift+S to capture it.

## Settings

Most things are in System Settings. Everything is also in `config.ini` next to `Liquid.exe`,
which System Settings writes as you click; [config.example.ini](config.example.ini) lists every
setting with its default. Dock icons can be replaced with your own PNGs: see
[icons/README.txt](icons/README.txt).

## Air Ring

Air Ring (`air-ring\`) is the part of WacOS that owns the webcam. It's Python because it runs
Google's MediaPipe (hands, body) and OpenCV's YuNet and SFace (face detection and recognition).
Liquid starts it, restarts it if it stops, and it quits by itself after Liquid does; there's no
tray icon, its controls are in System Settings > Air Gestures.

- **Face ID**: SFace turns your face into a 128-number embedding. `air-ring\data\face.npz` keeps
  those numbers, not pictures. The lock screen opens when two frames in a row match.
- **Walk-away lock**: Air Ring tells Liquid about once a second whether someone is at the desk
  (a face, a body, hands, or movement), and Liquid locks after the time you set.
- **Gestures**: pinch for volume and open-hand swipes for songs work out of the box. Tune them in
  `air-ring\config.json` (System Settings > Air Gestures > Open Settings File).
- **Privacy**: camera frames never leave your PC and aren't saved. Turn the camera off from
  System Settings > Air Gestures, or set `air_ring=off` in `config.ini` to never start it.

## Claude Code in the Dynamic Island (optional)

If you use [Claude Code](https://claude.com/claude-code), the island can show when Claude is
working or waiting for you, and which project. Add `tools\claude-hook.js` (needs Node.js) as a
hook for `UserPromptSubmit`, `Stop`, `Notification` and `SessionEnd` in `~/.claude/settings.json`:

```json
{
  "hooks": {
    "UserPromptSubmit": [{ "hooks": [{ "type": "command", "command": "node C:/path/to/WacOS/tools/claude-hook.js" }] }],
    "Stop":             [{ "hooks": [{ "type": "command", "command": "node C:/path/to/WacOS/tools/claude-hook.js" }] }],
    "Notification":     [{ "hooks": [{ "type": "command", "command": "node C:/path/to/WacOS/tools/claude-hook.js" }] }],
    "SessionEnd":       [{ "hooks": [{ "type": "command", "command": "node C:/path/to/WacOS/tools/claude-hook.js" }] }]
  }
}
```

## Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1
```

It quits WacOS, stops it starting at sign-in, brings the taskbar back and unregisters Liquid Keys.
Then delete the folder. If the taskbar ever stays hidden after a crash, run `restore-taskbar.ps1`.

## How it's built

Plain C++20 and Win32, no frameworks. Each panel is a layered DirectComposition window drawn with
Direct3D 11 (the glass) and Direct2D / DirectWrite (everything on it).

| | |
|---|---|
| `src\gfx.cpp` | Screen capture and the liquid glass shader |
| `src\menubar.cpp`, `appmenu.cpp`, `glassmenu.cpp` | Menu bar and its menus |
| `src\dock.cpp`, `anim.cpp` | Dock and window animations |
| `src\island.cpp` | Dynamic Island |
| `src\controlcenter.cpp`, `notifcenter.cpp`, `osd.cpp` | Control Center, Notification Center, volume HUD |
| `src\spotlight.cpp`, `switcher.cpp` | Spotlight and the app switcher |
| `src\prefs.cpp`, `about.cpp` | System Settings and About This PC |
| `src\lockscreen.cpp`, `airring.cpp` | Lock screen, Face ID, walk-away; starting Air Ring |
| `src\chrome.cpp`, `spaces.cpp`, `gestures.cpp` | Window buttons and frost, Spaces, right-drag gestures |
| `src\widgets.cpp`, `deskicons.cpp`, `desktopmenu.cpp` | Desktop widgets, icons and menu |
| `src\remote.cpp`, `streamglass.cpp` | Remote-stream detection and the glass used during a stream |
| `src\system.cpp` | Audio, Wi-Fi, radios, media and notifications (Core Audio, WLAN, WinRT) |
| `air-ring\` | Webcam: Face ID, presence, gestures (Python) |
| `keys\` | Liquid Keys: passkey helper and browser extension |

Build by hand with `build.bat`; while WacOS runs, `tools\reload.cmd` rebuilds and swaps the
running copy.

## License

[MIT](LICENSE). The models Air Ring downloads keep their own licenses (MediaPipe: Apache 2.0;
YuNet: MIT; SFace: Apache 2.0).
