# CoCIS Smart Control and Monitoring System

Group coursework (BSE III, Embedded Systems): a smart control and monitoring system for the College of Computing and Information Sciences, simulated in Wokwi with ESP-IDF.

**This file explains how to build, run and use the system.** How the code is organised (folders, components, tasks, synchronisation, pin plan) is in [PROJECT_STRUCTURE.md](PROJECT_STRUCTURE.md). The tests are in [docs/TEST_PLAN.md](docs/TEST_PLAN.md).

Following the lecturer's ruling (free Wokwi supports one microcontroller), the whole system runs on **one ESP32**: the building entrance and lecture rooms 1 and 2 all run in the same firmware.

## Team

| Module | Owner |
|---|---|
| Entrance and RFID access control | Dorcus Mukyala |
| Lecture rooms 1 and 2 (occupancy, automatic lighting) | Mariam (Room 1), Arnold (Room 2) |
| SD card log and settings, opening hours and clock, PIN login, admin commands | Joshua |
| Integration: start-up, hub, offline detection | Kiran |

## What the system does

- **Entrance.** A card is scanned at the entry or the exit reader and is granted or denied. It is denied if the card is unknown or blocked, if the person scans in twice or out without being inside (anti-passback), or if the building is closed.
  - **Outside opening hours both entry and exit are denied**, also for people still inside (coursework §2.6). Only emergency mode, switched on by an admin, allows exit outside hours.
  - Granted: green LED and the entry or exit door servo opens for 3 s. Denied: red LED and a buzzer beep.
  - The system keeps a list of who is inside.
- **RFID is simulated.** Wokwi has no RFID reader part, so a scan is typed in the serial monitor as `in <UID>` or `out <UID>`.
- **Lecture rooms.** A PIR sensor in each room switches the room light on when it detects motion, and off 10 s after the last motion. Each room reports its status, and a room that stops reporting for 15 s is shown as **OFFLINE**.
- **Opening hours and clock.** Default **08:00–18:00**, saved in flash. The simulated clock starts at 08:00 at every boot (Wokwi has no real-time clock).
- **Admin login.** Changing a setting needs `login <pin>`. On first boot the PIN is **1234** and the system asks you to change it. 3 wrong PINs lock login for 30 s, and a session ends after 120 s without a protected command.
- **Log.** Every event (access decisions, room changes, rooms going offline or online, logins, setting changes) is written to `ACCESS.LOG` on the SD card as `seq,HH:MM:SS,<event>`, and the last 20 lines are also kept in RAM. If the SD card cannot be mounted, the log is RAM only and the boot message says so.

## Build

Requires **ESP-IDF v5.5.1**. First open a terminal where ESP-IDF is set up, in one of these ways:

- **VS Code ESP-IDF extension:** F1 → **ESP-IDF: Open ESP-IDF Terminal**.
- **Windows installer:** open the **ESP-IDF 5.5 PowerShell** shortcut from the Start menu.
- **Manual install:** run the export script from your own ESP-IDF folder, `<your esp-idf folder>\export.ps1` on Windows or `. <your esp-idf folder>/export.sh` on Linux and macOS. If you installed the tools to a non-default folder, set `IDF_TOOLS_PATH` to it first.

Then, from the repository folder:

```powershell
cd cocis_system
idf.py set-target esp32     # first time only
idf.py build
```

`idf.py --version` should print `v5.5.1`.

## Run in Wokwi

1. Open the `cocis_system` folder in VS Code.
2. Press F1 and choose **Wokwi: Start Simulator**.
3. Click inside the Wokwi terminal and type `help`. Click a PIR sensor to simulate motion in a room.

If the VS Code terminal does not accept typing on your machine, add `rfc2217ServerPort = 4000` to your **local** `wokwi.toml` (do not commit it), start the simulator, and in a second ESP-IDF terminal (see Build) run:

```powershell
python -m serial.tools.miniterm rfc2217://localhost:4000
```

Expected output when you click a PIR sensor, and about 10 s after it stops sensing motion:

```text
[ROOM 1] OCCUPIED, light ON
[HUB] room 1 OCCUPIED, light ON
[ROOM 1] EMPTY, light OFF
[HUB] room 1 EMPTY, light OFF
```

## Console commands

`[login]` means you must run `login <pin>` first. While you type a PIN after `login ` or `setpin `, the console shows `*`.

| Command | Login | What it does |
|---|---|---|
| `in <UID>` / `out <UID>` | no | Simulate a card scan at the entry or exit reader |
| `inside` | no | Who is inside now |
| `users` | no | Registered cards, inside/outside, blocked |
| `log` | no | Recent access decisions (CSV) |
| `status` | no | Clock, opening hours, open/closed, emergency mode, people inside |
| `rooms` | no | Each room: ONLINE/OFFLINE, occupied, light, light-on seconds; people inside |
| `hours` | no | Show opening hours |
| `clock` | no | Show the simulated clock |
| `help` | no | List all commands |
| `login <pin>` | — | Start an admin session (3 wrong PINs = 30 s lockout) |
| `logout` | — | End the admin session |
| `setpin <new>` | yes | Change the admin PIN (4–8 digits) |
| `hours HH:MM HH:MM` | yes | Set opening and closing time (saved; opening must be before closing) |
| `clock HH:MM` | yes | Set the simulated clock |
| `syslog [n]` | yes | Last n system log lines from RAM and from the SD card |
| `emergency on` / `emergency off` | yes | Allow or block exit outside opening hours (logged) |
| `block <UID>` / `unblock <UID>` | yes | Block or unblock a card (saved) |
| `roomfail <1\|2> on\|off` | yes | Test: the room stops reporting, so it is shown OFFLINE |

Test cards: `A1B2C3D4` (Alice), `B2C3D4E5` (Brian), `C3D4E5F6` (Carol), `D4E5F6A7` (Dan), `DEADBEEF` (blocked). The card list is fixed in the firmware.

## Working on the repo

- Do not push directly to `main`. Work on your own branch and open a pull request.
- Do not commit `build/`, `sdkconfig`, `.vscode/` or your local `wokwi.toml` changes.
- `cocis_system/main/` is where components meet; tell Kiran before changing it.
