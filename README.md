# CoCIS Smart Control and Monitoring System

Group coursework (BSE III, Embedded Systems): a smart control and monitoring system for the College of Computing and Information Sciences, simulated in Wokwi with ESP-IDF.

**This file explains how to build, run and use the system.** How the code is organised (folders, components, tasks, synchronisation, pin plan) is in [PROJECT_STRUCTURE.md](PROJECT_STRUCTURE.md). The tests are in [docs/TEST_PLAN.md](docs/TEST_PLAN.md). The group report is in [docs/REPORT.md](docs/REPORT.md). Help for the video recordings is in [docs/VIDEO_GUIDE.md](docs/VIDEO_GUIDE.md).

Following the lecturer's ruling (free Wokwi supports one microcontroller), the whole system runs on **one ESP32**: the building entrance and lecture rooms 1 and 2 all run in the same firmware.

## Team

| Member | Main work |
|---|---|
| Mukyala Dorcus Nandy | Entrance and access control; the two MFRC522 RFID readers (driver + Wokwi custom chip); card roles, colours and the RGB LED |
| Mariam Wambui | Lecture-room occupancy and lighting; rooms panel on the web dashboard |
| Kisomose Arnold Patrick | Room component, project structure and circuit: `diagram.json` wiring, pin plan, `wokwi.toml`, `sdkconfig.defaults` |
| Katumba Joshua Mugwanya | SD card log and settings, opening hours and clock, PIN login, admin commands; web sign-in security (shared lockout, tokens) |
| Nannyombi Shakiran (Kiran) | Integration (start-up, hub, offline detection); Wi-Fi and the web Security Dashboard |

## What the system does

- **Entrance.** A card is tapped on the ENTRY or the EXIT reader and is granted or denied. It is denied if the card is unknown or blocked, if the person scans in twice or out without being inside (anti-passback), or if the building is closed.
  - **Outside opening hours both entry and exit are denied**, also for people still inside (coursework §2.6). Only emergency mode, switched on by an admin, allows exit outside hours.
  - Granted: green LED, the **RGB LED shows the card colour**, and the entry or exit door servo opens for 3 s. Denied: red LED and a buzzer beep.
  - The system keeps a list of who is inside, with each person's role and the time they came in.
- **Two RFID readers (MFRC522).** Wokwi has no official MFRC522 part, so the group built one as a **Wokwi custom chip** (`cocis_system/chips/`). It speaks the real MFRC522 SPI register protocol, and the firmware talks to it with a real MFRC522 driver. Typing `in <UID>` / `out <UID>` still works as a backup.
- **Card roles and colours.** Student = blue, Staff = green, Guest = yellow, Admin = purple.
- **Lecture rooms.** A PIR sensor in each room switches the room light on when it detects motion, and off 10 s after the last motion. Each room reports its status, and a room that stops reporting for 15 s is shown as **OFFLINE**.
- **Opening hours and clock.** Default **08:00–18:00**, saved in flash. The simulated clock starts at 08:00 at every boot (Wokwi has no real-time clock).
- **Admin login.** Changing a setting needs `login <pin>`. On first boot the PIN is **1234** and the system asks you to change it. 3 wrong PINs lock login for 30 s, and a session ends after 120 s without a protected command.
- **Web Security Dashboard.** The ESP32 joins the Wokwi Wi-Fi (`Wokwi-GUEST`) and serves a page at **http://localhost:8180**. Sign in with the admin PIN (same lockout as the console). It shows who is inside (colour chips), who is out, the rooms, and a live alert feed with pop-ups and beeps. Denied, unknown and blocked cards are red **SECURITY ALERT**s. Admin actions: opening hours, emergency mode, block/unblock, test scan.
- **Log.** Every event (access decisions, room changes, rooms going offline or online, logins, web actions, setting changes) is written to `ACCESS.LOG` on the SD card as `seq,HH:MM:SS,<event>`, and the last 20 lines are also kept in RAM. If the SD card cannot be mounted, the log is RAM only and the boot message says so.

## Build

Requires **ESP-IDF v6.0.1** (tested). First open a terminal where ESP-IDF is set up:

- **VS Code ESP-IDF extension:** F1 → **ESP-IDF: Open ESP-IDF Terminal**.
- **Manual install:** run the export script from your ESP-IDF folder, `<esp-idf>\export.ps1` on Windows or `. <esp-idf>/export.sh` on Linux and macOS.

Then, from the repository folder:

```sh
cd cocis_system
rm sdkconfig                # once, so the new sdkconfig.defaults are used (Windows: del sdkconfig)
idf.py set-target esp32     # first time only
idf.py build
```

`sdkconfig.defaults` sets a larger app partition (Wi-Fi + web server do not fit in 1 MB) and 4 MB flash. The RFID custom chip (`chips/mfrc522.chip.wasm`) is already built and committed. Only after changing `mfrc522.chip.c` run `chips/build.sh` (needs wasi-sdk).

## Run in Wokwi

1. Open the `cocis_system` folder in VS Code.
2. Press F1 and choose **Wokwi: Start Simulator**.
3. Click inside the Wokwi terminal and type `help`.
4. **Tap a card:** click the ENTRY (or EXIT) reader chip in the diagram and move the **Card** slider: `0` none, `1` Alice (staff), `2` Brian (student), `3` Carol (student), `4` Dan (admin), `5` Grace (guest), `6` lost card (blocked), `7` unknown. Set it back to `0` to take the card away (the same card is reported again only after it was removed).
5. **Rooms:** click a PIR sensor to simulate motion in a room.
6. **Dashboard:** wait for `[WIFI] connected, IP ...`, then open **http://localhost:8180** in your browser and sign in with the PIN (default `1234`).

If the VS Code terminal does not accept typing on your machine, add `rfc2217ServerPort = 4000` to your **local** `wokwi.toml` (do not commit it), start the simulator, and in a second ESP-IDF terminal run:

```sh
python -m serial.tools.miniterm rfc2217://localhost:4000
```

Expected output when the ENTRY reader slider is set to 1:

```text
[RFID ENTRY] card A1B2C3D4
>> ENTRY A1B2C3D4 (Alice (Staff), green card) -> GRANTED [ok] | inside now: 1
[HUB] building occupancy: 1
```

## Console commands

`[login]` means you must run `login <pin>` first. While you type a PIN after `login ` or `setpin `, the console shows `*`.

| Command | Login | What it does |
|---|---|---|
| `in <UID>` / `out <UID>` | no | Backup: a typed card scan at the entry or exit reader |
| `inside` | no | Who is inside now: name, role, card colour, since when |
| `users` | no | Registered cards: role, colour, inside/outside, blocked |
| `log` | no | Recent access decisions (CSV) |
| `status` | no | Clock, opening hours, open/closed, emergency mode, people inside |
| `rooms` | no | Each room: ONLINE/OFFLINE, occupied, light, light-on seconds; people inside |
| `hours` | no | Show opening hours |
| `clock` | no | Show the simulated clock |
| `wifi` | no | Wi-Fi status, IP address and the dashboard address |
| `help` | no | List all commands |
| `login <pin>` | — | Start an admin session (3 wrong PINs = 30 s lockout, shared with the web) |
| `logout` | — | End the admin session |
| `setpin <new>` | yes | Change the admin PIN (4–8 digits) |
| `hours HH:MM HH:MM` | yes | Set opening and closing time (saved; opening must be before closing) |
| `clock HH:MM` | yes | Set the simulated clock |
| `syslog [n]` | yes | Last n system log lines from RAM and from the SD card |
| `emergency on` / `emergency off` | yes | Allow or block exit outside opening hours (logged) |
| `block <UID>` / `unblock <UID>` | yes | Block or unblock a card (saved) |
| `roomfail <1\|2> on\|off` | yes | Test: the room stops reporting, so it is shown OFFLINE |

## Test cards

The card list is fixed in the firmware (`access_control.c`) and in the custom chip (`mfrc522.chip.c`).

| Slider | UID | Name | Role | Colour |
|---|---|---|---|---|
| 1 | `A1B2C3D4` | Alice | Staff | green |
| 2 | `B2C3D4E5` | Brian | Student | blue |
| 3 | `C3D4E5F6` | Carol | Student | blue |
| 4 | `D4E5F6A7` | Dan | Admin | purple |
| 5 | `E5F6A7B8` | Grace | Guest | yellow |
| 6 | `DEADBEEF` | Lost card | (blocked) | — |
| 7 | `12345678` | not registered | unknown | — |

## Working on the repo

- Do not push directly to `main`. Work on your own branch and open a pull request.
- Do not commit `build/`, `sdkconfig`, `.vscode/` or your local `wokwi.toml` changes.
- `cocis_system/main/` is where components meet; tell Kiran before changing it.
