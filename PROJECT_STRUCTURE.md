# CoCIS Smart Control: Project Structure

**This file explains how the code is organised**: folders, what each component does and its functions, the tasks and queues, the start-up order, synchronisation and the pin plan. How to build, run and use the system (and who owns what) is in [README.md](README.md).

There is one firmware (`cocis_system`) for one ESP32. Each of the three terminals in the brief (the entrance, room 1 and room 2) is a group of FreeRTOS tasks with its own pins, data and mutex, and they report to a hub task through one FreeRTOS queue. The terminals do not need Wi-Fi to talk to each other. Wi-Fi is used only for the web Security Dashboard, and the system keeps working without it.

## Folders

```text
cocis-smart-system/
├── README.md                    What the system does; build, run, commands, web API, test cards
├── PROJECT_STRUCTURE.md         This file
├── .gitignore                   Build output, sdkconfig, editor files, docs/
│
├── components/                  Every component has CMakeLists.txt, <name>.c and include/<name>.h
│   ├── access_control/          Entrance: card check, roles/colours, doors, LEDs, buzzer, inside list, console task
│   ├── rfid/                    Two MFRC522 readers (ENTRY, EXIT): software SPI, driver (REQA/WUPA,
│   │                            anticollision, BCC check), rfid task, "rfid" command
│   ├── lcd_display/             16x2 I2C LCD at the entrance (PCF8574 at 0x27, 4-bit mode), lcd task and queue
│   ├── web_dashboard/           Wi-Fi + web Security Dashboard
│   │   ├── wifi.c               Joins "Wokwi-GUEST", retries forever
│   │   ├── web_dashboard.c      HTTP server, JSON API, PIN sign-in, token, alert ring, "wifi" command
│   │   └── dashboard.html       The page (embedded in the firmware at build time)
│   ├── room_control/            Lecture rooms 1 and 2: one task per room, PIR -> occupancy -> light
│   │   └── include/
│   │       ├── room_control.h   room_status_t, start/get functions, fault test aid
│   │       └── room_config.h    Room pins and timings
│   ├── storage/                 SD card log (RAM fallback) + flash key/value (NVS)
│   ├── schedule/                Opening hours + simulated clock + timestamped log
│   ├── auth/                    Admin PIN (salted hash, lockout shared by console and web, idle timeout)
│   └── management/              Admin console commands
│
└── cocis_system/                The firmware project: integration and synchronisation
    ├── CMakeLists.txt           Adds ../components
    ├── sdkconfig.defaults       Large app partition, 4 MB flash, event task stack, HTTP header size
    ├── wokwi.toml               Firmware path, port forward 8180 -> 80
    ├── diagram.json             One ESP32: 2 MFRC522 readers, LCD, doors, LEDs, buzzer, PIR + LED per room, SD card
    └── main/
        ├── CMakeLists.txt
        ├── main.c               app_main: starts everything in order; room adapter for the web
        ├── hub.c                Hub queue + task, offline detection, rooms/roomfail commands
        └── hub.h                hub_start(), hub_get_room(), hub_room_t
```

Generated `build/`, `sdkconfig` and `sdkconfig.old` files are not committed. The project report, test plan and other coursework documents are kept outside the repository (`docs/` is ignored).

## What each component does

| Component | Purpose | Main functions |
|---|---|---|
| `access_control` | Entrance: ENTRY and EXIT scan points, grant/deny, door servos, LEDs, buzzer, who is inside (role, since-time). Owns the serial console; other components add commands to it. | `entrance_start()`, `entrance_submit_scan()`, `entrance_register_command()`, `entrance_set_event_cb()` (up to 4 listeners), `entrance_get_users()`, `entrance_inside_count()`, `entrance_set_emergency()`, `entrance_get_emergency()`, `entrance_set_blocked()`, `entrance_role_name()`, `entrance_role_color()` |
| `rfid` | Two MFRC522 readers (Wokwi `board-mfrc522`) driven by software SPI (shared SCK/MOSI, own CS and own MISO per reader). The `rfid` task polls both about every 50 ms and sends each new card to the entrance once (debounced); the `rfid` command shows reader status. A reader that is not found is skipped and typed scans still work | `rfid_register_commands()`, `rfid_start()` |
| `lcd_display` | 16x2 I2C LCD at the entrance. An entrance event listener copies each decision into a queue; the `lcd` task shows it for 3 s, otherwise the clock, people inside and OPEN/CLOSED. Runs without the LCD if none answers | `lcd_display_start()` |
| `web_dashboard` | Wi-Fi station, HTTP server on port 80, PIN sign-in with one session token (10 min idle timeout), JSON API (listed in README), ring of the last 30 alerts, `wifi` command | `web_dashboard_set_room_provider()`, `web_dashboard_register_commands()`, `web_dashboard_start()`, `wifi_net_start()` |
| `room_control` | One task per room: polls the PIR every 100 ms, light follows occupancy with a 10 s vacancy delay, counts light-on seconds, reports on change and every 5 s | `room_control_start(1/2)`, `room_control_set_publish_cb()`, `room_control_get()`, `room_control_simulate_fault()` (test aid) |
| `storage` | Log lines to `/sdcard/ACCESS.LOG` (SPI2) plus a RAM ring of 20 lines (RAM only if the card fails); small settings in NVS | `storage_init()`, `storage_log()`, `storage_log_print()`, `storage_log_dump_sd()`, `storage_sd_mounted()`, `storage_kv_set/get()` |
| `schedule` | The only clock and opening hours (saved in NVS, opening before closing), timestamped logging used by every component | `schedule_init()`, `schedule_set/get()`, `schedule_is_open()`, `clock_set()`, `clock_minutes()`, `clock_str()`, `schedule_log()` |
| `auth` | One ADMIN role. PIN stored as a random 16-byte salt + SHA-256 hash, compared in constant time; lockout, idle timeout, default-PIN flag. Console and web share one failure counter | `auth_init()`, `auth_login()` (console), `auth_check_pin()` (web), `auth_logout()`, `auth_is_logged_in()`, `auth_set_pin()`, `auth_lockout_remaining_s()`, `auth_must_change_pin()` |
| `management` | Registers the admin commands with the entrance console; every setting change checks `auth_is_logged_in()` first | `management_register_commands()` |
| `cocis_system` | Start-up order, hub task, room table (online/offline, latest report), offline detection, `rooms` and `roomfail` commands, room adapter `web_room()` for the dashboard | `app_main()`, `hub_start()`, `hub_get_room()` |

## Tasks and queues

```text
rfid task -----entrance_submit_scan()--+
console_task --entrance_submit_scan()--+--scan_queue--> access_task --actuator_queue--> actuator_task --> servos, LEDs, buzzer
web /api/scan -entrance_submit_scan()--+                     |
                                                             +-- entrance event (each listener) --+--> hub queue --> hub_task
room1 task --status (change / 5 s)----------------------------------------------------------------+
room2 task --status (change / 5 s)----------------------------------------------------------------+
                                                             +-- entrance event --> web alert ring --> browser (polls /api/state every 1.5 s)
                                                             +-- entrance event --> lcd_queue --> lcd_task --> 16x2 LCD (I2C)
```

| Task | Component | Priority | Stack (bytes) |
|---|---|---|---|
| `hub` | cocis_system | 6 | 6144 |
| `access` | access_control | 5 | 6144 |
| `room1`, `room2` | room_control | 5 | 4096 |
| `actuator` | access_control | 4 | 4096 |
| `rfid` | rfid | 4 | 4096 |
| `console` | access_control | 3 | 6144 |
| `lcd` | lcd_display | 3 | 3072 |
| `httpd` (HTTP server) | web_dashboard (ESP-IDF `httpd_start`) | ESP-IDF default (5) | 6144 |
| Wi-Fi driver and event tasks | ESP-IDF | ESP-IDF default | event task 6144 (`sdkconfig.defaults`) |

- All scans (reader, typed, web test scan) go through `entrance_submit_scan()`, which puts a `scan_event_t` on `scan_queue` without waiting. So the decision code is the same for all three.
- The rfid task reports a card **once** when it appears. It reports the same card again only after the reader saw no card for 3 polls in a row (debounce).
- The console task is the only reader of the serial port.
- The actuator task holds a door open for 3 s, so the access task can decide the next scan meanwhile.
- The hub waits on its queue with a 1 s timeout and, after each wake-up, marks a room OFFLINE if it has not reported for 15 s (3 missed heartbeats).

## Start-up order (main.c)

1. `storage_init()` → `schedule_init()` → `auth_init()` (each later one uses the earlier ones)
2. `hub_start()`: queue, hub task, room and entrance callbacks, `rooms`/`roomfail` commands
3. `management_register_commands()`
4. `web_dashboard_set_room_provider(web_room)` and `web_dashboard_register_commands()`: alert listener and `wifi` command
5. `lcd_display_start()`: I2C bus, LCD check and the entrance listener for the LCD
6. `room_control_start(1)`, `room_control_start(2)`
7. `entrance_start()`: it starts the console, so all commands and listeners must be registered before it
8. `rfid_start()`: the readers send scans to the entrance, so they start after it
9. `web_dashboard_start()`: Wi-Fi and the web server. It does not wait for Wi-Fi.

## Synchronisation (coursework §2.7)

| Shared resource | Protection |
|---|---|
| Entrance state (users, roles, since-times, inside count, emergency, access log) | `state_mutex` in access_control |
| Each room's status | one mutex per room |
| Hub's room table (online/offline, last room report) | hub mutex |
| Building occupancy and who-is-inside list | `state_mutex` in access_control (single owner; hub reads via `entrance_inside_count()`, web via `entrance_get_users()`) |
| Opening hours and clock | schedule mutex |
| Log ring and SD log file | storage mutex (lines from different tasks are never mixed) |
| Login session, lockout counter, PIN record | auth mutex; **one lockout counter shared by console (`auth_login`) and web (`auth_check_pin`)** |
| The two readers (shared SCK/MOSI) | Only the `rfid` task talks to them, one reader at a time; each has its own CS and its own MISO line, so their answers can never mix. Reader status for the `rfid` command is guarded by `state_lock` |
| LCD screen | written only by the `lcd` task; decisions reach it through `lcd_queue` |
| Web alert ring and web session token | `s_lock` mutex in web_dashboard (entrance task writes alerts, HTTP task reads them) |
| Scans, door commands, reports to the hub, LCD messages | FreeRTOS queues (`scan_queue`, `actuator_queue`, hub queue, `lcd_queue`) |
| Serial port input | read only by `console_task` |

Room and entrance callbacks only call `xQueueSend(..., 0)` or copy into the alert ring, so a busy hub, web server or LCD never blocks a room or the entrance. A full queue prints a drop message; the room heartbeat retries within 5 s. Room reports include a per-room sequence number so the hub can ignore a duplicate.

## Pin plan (must not clash)

| Use | GPIO | Defined in |
|---|---|---|
| Room 1 PIR / LED | 32 / 21 | `room_config.h` |
| Room 2 PIR / LED | 33 / 22 | `room_config.h` |
| SD card SCK / MOSI / MISO / CS (SPI2) | 14 / 13 / 34 / 4 | `storage.c` |
| RFID readers SCK / MOSI (software SPI, shared) | 16 / 17 | `rfid.c` |
| RFID ENTRY reader CS (SDA pin) / MISO | 5 / 35 | `rfid.c` |
| RFID EXIT reader CS (SDA pin) / MISO | 23 / 36 (VP) | `rfid.c` |
| RFID RST (both readers) | wired to 3.3 V (always running) | `diagram.json` |
| Entrance servos (entry / exit), LEDC 50 Hz | 18 / 19 | `access_control.c` |
| Entrance LEDs (grant / deny) and buzzer (LEDC 2 kHz) | 25 / 26 / 27 | `access_control.c` |
| Entrance LCD SDA / SCL (I2C, address 0x27) | 15 / 2 | `lcd_display.c` |

**Circuit layout (`diagram.json`).** The parts are placed in zones around the ESP32 so nothing overlaps: the rooms (PIRs + room LEDs) at the top left, above the ESP32; the ENTRY reader and its door servo at the top right; the LCD on the right between the two readers; the EXIT reader and its door servo at the bottom right; the SD card, the green/red LEDs and the buzzer at the bottom left. Every wire is listed part-first in `diagram.json`, because Wokwi draws the first leg of a wire sideways from its first end: the wire runs through empty space and then straight along the ESP32 pin column, instead of across the board.

Change a pin only after telling the group, and update `diagram.json` to match.
