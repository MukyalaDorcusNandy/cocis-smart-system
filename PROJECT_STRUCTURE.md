# CoCIS Smart Control: Project Structure

**This file explains how the code is organised**: folders, what each component does and its functions, the tasks and queues, the start-up order, synchronisation and the pin plan. How to build, run and use the system (and who owns what) is in [README.md](README.md).

There is one firmware (`cocis_system`) for one ESP32. Each of the three terminals in the brief (the entrance, room 1 and room 2) is a group of FreeRTOS tasks with its own pins, data and mutex, and they report to a hub task through one FreeRTOS queue. No Wi-Fi, MQTT or separate per-room projects are used.

## Folders

```text
cocis-smart-system/
├── components/
│   ├── access_control/          Entrance and (simulated) RFID access control
│   │   ├── CMakeLists.txt
│   │   ├── access_control.c     Card check, doors, LEDs, buzzer, inside list, console task
│   │   └── include/
│   │       └── access_control.h
│   │
│   ├── room_control/            Lecture rooms 1 and 2
│   │   ├── CMakeLists.txt
│   │   ├── room_control.c       One task per room: PIR -> occupancy -> light
│   │   └── include/
│   │       ├── room_control.h   room_status_t, start/get functions
│   │       └── room_config.h    Room pins and timings
│   │
│   ├── storage/                 SD card log (RAM fallback) + flash key/value
│   ├── schedule/                Opening hours + simulated clock + timestamped log
│   ├── auth/                    Admin PIN login (salted hash, lockout, idle timeout)
│   └── management/              Admin console commands
│       (each: CMakeLists.txt, <name>.c, include/<name>.h)
│
├── cocis_system/                The firmware project: integration and synchronisation
│   ├── CMakeLists.txt           Adds ../components
│   ├── wokwi.toml               Points Wokwi to build/cocis_system.elf
│   ├── diagram.json             One ESP32: entrance parts, PIR + LED per room, SD card
│   └── main/
│       ├── CMakeLists.txt
│       ├── main.c               app_main: starts everything in order
│       ├── hub.c                Hub queue + task, offline detection, rooms/roomfail commands
│       └── hub.h                hub_start(), hub_get_room(), hub_room_t
│
└── docs/
    └── TEST_PLAN.md             Tests with steps and expected serial output
```

Generated `build/`, `sdkconfig` and `sdkconfig.old` files are not committed.

## What each component does

| Component | Purpose | Main functions |
|---|---|---|
| `access_control` | Entrance: ENTRY and EXIT scan points, grant/deny, door servos, LEDs, buzzer, who is inside. Owns the serial console; other components add commands to it. | `entrance_start()`, `entrance_register_command()`, `entrance_set_event_cb()`, `entrance_inside_count()`, `entrance_set_emergency()`, `entrance_set_blocked()` |
| `room_control` | One task per room: polls the PIR every 100 ms, light follows occupancy with a 10 s vacancy delay, counts light-on seconds, reports on change and every 5 s | `room_control_start(1/2)`, `room_control_set_publish_cb()`, `room_control_get()`, `room_control_simulate_fault()` (test aid) |
| `storage` | Log lines to `/sdcard/ACCESS.LOG` (SPI2) plus a RAM ring of 20 lines (RAM only if the card fails); small settings in NVS | `storage_init()`, `storage_log()`, `storage_log_print()`, `storage_log_dump_sd()`, `storage_sd_mounted()`, `storage_kv_set/get()` |
| `schedule` | The only clock and opening hours (saved in NVS, opening before closing), timestamped logging used by every component | `schedule_init()`, `schedule_set/get()`, `schedule_is_open()`, `clock_set()`, `clock_minutes()`, `clock_str()`, `schedule_log()` |
| `auth` | One ADMIN role. PIN stored as a random 16-byte salt + SHA-256 hash, compared in constant time; lockout, idle timeout, default-PIN flag | `auth_init()`, `auth_login()`, `auth_logout()`, `auth_is_logged_in()`, `auth_set_pin()`, `auth_lockout_remaining_s()`, `auth_must_change_pin()` |
| `management` | Registers the admin commands with the entrance console; every setting change checks `auth_is_logged_in()` first | `management_register_commands()` |
| `cocis_system` | Start-up order, hub task, room table, building occupancy, offline detection, `rooms` and `roomfail` commands | `app_main()`, `hub_start()`, `hub_get_room()` |

## Tasks and queues

```text
console_task --scan_queue--> access_task --actuator_queue--> actuator_task --> servos, LEDs, buzzer
                                  |
                                  +--entrance event--+
room1 task --status (change / 5 s)-------------------+--> hub queue --> hub_task
room2 task --status (change / 5 s)-------------------+
```

| Task | Component | Priority | Stack (bytes) |
|---|---|---|---|
| `hub` | cocis_system | 6 | 6144 |
| `access` | access_control | 5 | 6144 |
| `room1`, `room2` | room_control | 5 | 4096 |
| `actuator` | access_control | 4 | 4096 |
| `console` | access_control | 3 | 6144 |

- The console task is the only reader of the serial port. A real MFRC522 reader task would put the same `scan_event_t` on `scan_queue`.
- The actuator task holds a door open for 3 s, so the access task can decide the next scan meanwhile.
- The hub waits on its queue with a 1 s timeout and, after each wake-up, marks a room OFFLINE if it has not reported for 15 s (3 missed heartbeats).

## Start-up order (main.c)

1. `storage_init()` → `schedule_init()` → `auth_init()` (each later one uses the earlier ones)
2. `hub_start()`: queue, hub task, room and entrance callbacks, `rooms`/`roomfail` commands
3. `management_register_commands()`
4. `room_control_start(1)`, `room_control_start(2)`
5. `entrance_start()` last: it starts the console, and all commands must be registered before it

## Synchronisation (coursework §2.7)

| Shared resource | Protection |
|---|---|
| Entrance state (users, inside count, emergency, access log) | `state_mutex` in access_control |
| Each room's status | one mutex per room |
| Hub's room table and building occupancy | hub mutex |
| Opening hours and clock | schedule mutex |
| Log ring and SD log file | storage mutex (lines from different tasks are never mixed) |
| Login session, lockout, PIN record | auth mutex |
| Scans, door commands, reports to the hub | FreeRTOS queues (`scan_queue`, `actuator_queue`, hub queue) |
| Serial port input | read only by `console_task` |

Room and entrance callbacks only call `xQueueSend(..., 0)`, so a busy hub never blocks a room or the entrance.

## Pin plan (must not clash)

| Use | GPIO | Defined in |
|---|---|---|
| Room 1 PIR / LED | 32 / 21 | `room_config.h` |
| Room 2 PIR / LED | 33 / 22 | `room_config.h` |
| SD card SCK / MOSI / MISO / CS (SPI2) | 14 / 13 / 34 / 4 | `storage.c` |
| Entrance servos (entry / exit), LEDC 50 Hz | 18 / 19 | `access_control.c` |
| Entrance LEDs (grant / deny) and buzzer (LEDC 2 kHz) | 25 / 26 / 27 | `access_control.c` |

Change a pin only after telling the group, and update `diagram.json` to match.
