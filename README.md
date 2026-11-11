# CoCIS Smart Control and Monitoring System

Group coursework (BSE III, Embedded Systems): a smart control and monitoring system for the College of Computing and Information Sciences, simulated in Wokwi with ESP-IDF.

**This file explains what the system does and how to build, run and use it.** How the code is organised (folders, components, tasks, synchronisation, pin plan) is in [PROJECT_STRUCTURE.md](PROJECT_STRUCTURE.md).

Following the lecturer's ruling (free Wokwi supports one microcontroller), the whole system runs on **one ESP32**: the building entrance and lecture rooms 1 and 2 all run in the same firmware, as separate groups of FreeRTOS tasks.

## Team

| Member | Main work |
|---|---|
| Mukyala Dorcus Nandy | Entrance and access control; the two MFRC522 RFID readers (driver); card roles and colours; buzzer pattern |
| Mariam Wambui | Lecture-room occupancy and lighting; rooms panel on the web dashboard |
| Kisomose Arnold Patrick | Room component, project structure and circuit: `diagram.json` wiring and layout, pin plan, `wokwi.toml`, `sdkconfig.defaults`; the entrance LCD (`lcd_display`) |
| Katumba Joshua Mugwanya | SD card log and settings, opening hours and clock, PIN login, admin commands; web sign-in security (shared lockout, tokens) |
| Nannyombi Shakiran (Kiran) | Integration (start-up, hub, offline detection); Wi-Fi and the web Security Dashboard |

## What the system does

- **Entrance.** A card is tapped on the ENTRY or the EXIT reader and is granted or denied. It is denied if the card is unknown or blocked, if the person scans in twice or out without being inside (anti-passback), or if the building is closed.
  - **Outside opening hours both entry and exit are denied**, also for people still inside (coursework §2.6). Only emergency mode, switched on by an admin, allows exit outside hours.
  - Granted: green LED, one short beep, and the entry or exit door servo opens for 3 s. Denied: red LED and two long buzzer beeps.
  - The system keeps a list of who is inside, with each person's role and the time of their last granted scan.
- **Two RFID readers (MFRC522).** Two Wokwi `board-mfrc522` parts ([Wokwi docs](https://docs.wokwi.com/parts/board-mfrc522)). The firmware drives them over software SPI (shared clock and data-out lines, a separate chip-select and data-in line per reader) with its own register-level MFRC522 driver, no library. A card is reported once when it appears; holding it on the reader does not repeat the scan. Typing `in <UID>` / `out <UID>` on the console still works as a backup.
- **Card roles and colours.** Student = blue, Staff = green, Guest = yellow, Admin = red. The colour is the colour of the Wokwi card itself, and it is shown on the LCD, the console and the dashboard.
- **Entrance LCD.** A 16x2 I2C LCD at the door. When idle it shows the clock, how many people are inside and OPEN/CLOSED. After a scan it shows the result for 3 s, for example `WELCOME Brian` / `Student BLUE card` or `DENIED` / `Building closed`. The system runs without it if no LCD answers.
- **Lecture rooms.** A PIR sensor in each room switches the room light on when it detects motion, and off 10 s after the last motion. The room counts how long its light has been on. Each room reports its status on every change and every 5 s; a room that stops reporting for 15 s is shown as **OFFLINE**.
- **Opening hours and clock.** Default **08:00–18:00**, saved in flash. The simulated clock starts at 08:00 at every boot (Wokwi has no real-time clock) and can be set by an admin.
- **Admin login.** Changing a setting needs `login <pin>`. On first boot the PIN is **1234** and the system asks you to change it. The PIN is stored only as a salted SHA-256 hash. 3 wrong PINs lock login for 30 s, and a session ends after 120 s without a protected command.
- **Web Security Dashboard.** The ESP32 joins the Wokwi Wi-Fi (`Wokwi-GUEST`) and serves a page at **http://localhost:8180**. Sign in with the admin PIN (the console and the web share one lockout counter). It shows who is inside (colour chips), who is out, the rooms, and a live alert feed with pop-ups and beeps; every denied scan pops up as a red **SECURITY ALERT**. Admin actions: opening hours, emergency mode, block/unblock, test scan. One web session at a time; it ends after 10 minutes without a request. The rest of the system works without Wi-Fi.
- **Log.** Every event (access decisions, room changes, rooms going offline or online, logins, web actions, setting changes) is written to `ACCESS.LOG` on the SD card as `seq,HH:MM:SS,<event>`, and the last 20 lines are also kept in RAM. If the SD card cannot be mounted, the log is RAM only and the boot message says so.

## Build

Requires **ESP-IDF v6.0.1** (tested). `idf.py` only works in a terminal where ESP-IDF is loaded:

- **VS Code ESP-IDF extension:** F1 → **ESP-IDF: Open ESP-IDF Terminal**.
- **Linux / macOS:** `. <esp-idf folder>/export.sh` (for example `. ~/.espressif/tools/activate_idf_v6.0.1.sh`).
- **Windows:** open the **ESP-IDF PowerShell** shortcut, or run `<esp-idf folder>\export.ps1`.

Then, from the repository folder:

```sh
cd cocis_system
rm -f sdkconfig             # first time only, so sdkconfig.defaults is used (Windows: del sdkconfig)
idf.py set-target esp32     # first time only
idf.py build
```

`sdkconfig.defaults` sets a larger app partition (Wi-Fi + web server do not fit in 1 MB), 4 MB flash, a bigger system event task stack and a larger HTTP header limit.

## Run in Wokwi

1. Open the `cocis_system` folder in VS Code.
2. Press F1 and choose **Wokwi: Start Simulator**. Turn the computer sound on (buzzer and dashboard beeps).
3. Click inside the Wokwi terminal and type `help`.
4. **Tap a card:** click the ENTRY (or EXIT) reader, pick a card and press **Tap**. With the keyboard: click the reader's card panel, press `b` / `g` / `y` / `r` / `n` / `k` to pick a card, and `t` to tap. **Hold** keeps the card on the reader.
5. **Rooms:** click a PIR sensor to simulate motion in a room.
6. **Dashboard:** wait for `[WIFI] connected, IP ...`, then open **http://localhost:8180** in your browser and sign in with the PIN (default `1234`).

Expected result of the first tap on the ENTRY reader:

| Card in Wokwi | Person | Role / colour | Result |
|---|---|---|---|
| Blue Card | Brian | Student / blue | GRANTED, LCD `WELCOME Brian` / `Student BLUE card` |
| Green Card | Alice | Staff / green | GRANTED, LCD `WELCOME Alice` / `Staff GREEN card` |
| Yellow Card | Grace | Guest / yellow | GRANTED, LCD `WELCOME Grace` / `Guest YELLOW card` |
| Red Card | Dan | Admin / red | GRANTED, LCD `WELCOME Dan` / `Admin RED card` |
| Key Fob (orange) | Lost Card | blocked | DENIED, LCD `Card blocked` |
| NFC Tag (grey) | not registered | — | DENIED, LCD `Unknown card` |

Tapping the same card on ENTRY again gives DENIED `Already inside`. Tap it on EXIT to leave (`BYE ...`).

Serial output when the Blue Card is tapped on the ENTRY reader:

```text
[RFID ENTRY] card 01020304
>> ENTRY 01020304 (Brian (Student), blue card) -> GRANTED [ok] | inside now: 1
[HUB] building occupancy: 1
```

If the VS Code terminal does not accept typing on your machine, add `rfc2217ServerPort = 4000` to your **local** `wokwi.toml` (do not commit it), start the simulator, and in a second ESP-IDF terminal run:

```sh
python -m serial.tools.miniterm rfc2217://localhost:4000
```

### Troubleshooting

- A tap does nothing: pick the card first, then press **Tap**. Type `rfid` to see each reader's status. If a reader is `NOT FOUND`, use the typed `in <UID>` / `out <UID>` commands instead.
- No text on the LCD, or `[LCD] no LCD at 0x27, running without it`: check that the LCD is in I2C mode (`"pins": "i2c"` in `diagram.json`).
- Dashboard not loading: wait for the `[WIFI] connected, IP ...` line, then reload the page.
- Build uses old settings: delete `sdkconfig` and build again.

## Console commands

`[login]` means you must run `login <pin>` first. While you type a PIN after `login ` or `setpin `, the console shows `*`.

| Command | Login | What it does |
|---|---|---|
| `in <UID>` / `out <UID>` | no | Backup: a typed card scan at the entry or exit reader |
| `inside` | no | Who is inside now: name, role, card colour, since when |
| `users` | no | Registered cards: role, colour, inside/outside, blocked |
| `log` | no | Last 20 access decisions (CSV) |
| `status` | no | Clock, opening hours, open/closed, emergency mode, people inside |
| `rooms` | no | Each room: ONLINE/OFFLINE, occupied, light, light-on seconds; people inside |
| `rfid` | no | Status of the two RFID readers (found, cards read, card on reader) |
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
| `roomfail <1\|2> on\|off` | yes | Test: the room stops reporting, so it is shown OFFLINE after 15 s |

## Web API

The dashboard page talks to the ESP32 through a small JSON API. Every call except `/` and `/api/login` needs the header `X-Token: <token from login>`, otherwise the answer is `401`.

| Method and path | Body | What it does |
|---|---|---|
| `GET /` | — | The dashboard page (embedded in the firmware) |
| `POST /api/login` | `pin=1234` | Sign in, returns `{"ok":true,"token":"..."}` |
| `POST /api/logout` | — | End the web session |
| `GET /api/state` | — | Clock, hours, emergency, people inside, users, rooms, alerts |
| `POST /api/hours` | `open=HH:MM&close=HH:MM` | Set opening hours |
| `POST /api/emergency` | `on=1` or `on=0` | Emergency mode on/off |
| `POST /api/block` | `uid=...&blocked=1` or `0` | Block or unblock a card |
| `POST /api/scan` | `uid=...&entry=1` or `0` | Test scan, same as `in` / `out` on the console |

## Test cards

The card list is fixed in the firmware (`users[]` in `access_control.c`). The UIDs match the Wokwi MFRC522 card presets, and the card colour is the role colour. Only the blocked flags can change at run time; they are saved in flash.

| Wokwi card | UID | Name | Role | Colour |
|---|---|---|---|---|
| Green Card | `11223344` | Alice | Staff | green |
| Blue Card | `01020304` | Brian | Student | blue |
| Yellow Card | `55667788` | Grace | Guest | yellow |
| Red Card | `AABBCCDD` | Dan | Admin | red |
| Key Fob (orange) | `C0FFEE99` | Lost Card | (blocked) | — |
| NFC Tag (grey) | `04112233` | not registered | unknown | — |
| (none, typed only) | `C3D4E5F6` | Carol | Student | blue |

The NFC Tag has a 7-byte UID. The driver reads only its first 4 bytes (`04112233`), and that ID is not registered, so it is denied as an unknown tag.

## Working on the repo

- Do not push directly to `main`. Work on your own branch and open a pull request.
- Do not commit `build/`, `sdkconfig`, `.vscode/` or your local `wokwi.toml` changes.
- `cocis_system/main/` is where the components meet; agree changes there with the integrator.
