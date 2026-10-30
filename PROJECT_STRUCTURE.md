# CoCIS Smart Control: Project Structure (Single ESP32)

The lecturer's ruling: free Wokwi = ONE microcontroller. So there is one firmware (`cocis_system`) and everything runs as FreeRTOS tasks on one ESP32. Rooms report to the hub through a FreeRTOS queue. No Wi-Fi, MQTT or separate room projects are needed.

## Folders

```text
cocis-smart-system/
├── components/
│   ├── access_control/          [Dorcus] Entrance and RFID access control
│   │   ├── CMakeLists.txt
│   │   ├── access_control.c     Card validation, door state, entry/exit events, console
│   │   └── include/
│   │       └── access_control.h
│   │
│   ├── room_control/            [Mariam + Arnold] Lecture Rooms 1 and 2
│   │   ├── CMakeLists.txt
│   │   ├── room_control.c       Mariam: Room 1 behaviour and tests
│   │   │                        Arnold: Room 2 behaviour and tests
│   │   └── include/
│   │       ├── room_control.h   room_status_t, start/get functions (agree changes together)
│   │       └── room_config.h    Room 1 pins [Mariam], Room 2 pins [Arnold], shared timings
│   │
│   ├── storage/                 [Joshua] SD card log + flash key/value
│   │   ├── CMakeLists.txt
│   │   ├── storage.c
│   │   └── include/
│   │       └── storage.h
│   │
│   ├── schedule/                [Joshua] Opening hours + simulated clock
│   │   ├── CMakeLists.txt
│   │   ├── schedule.c
│   │   └── include/
│   │       └── schedule.h
│   │
│   └── auth/                    [Joshua] Manager/admin PIN login
│       ├── CMakeLists.txt
│       ├── auth.c
│       └── include/
│           └── auth.h
│
└── cocis_system/                [Kiran] System integration and synchronisation
    ├── CMakeLists.txt           Adds ../components
    ├── wokwi.toml               Points Wokwi to the firmware
    ├── diagram.json             One ESP32: entrance parts [Dorcus],
    │                            PIR + LED Room 1 [Mariam],
    │                            PIR + LED Room 2 [Arnold], SD card [Joshua]
    └── main/
        ├── CMakeLists.txt
        └── main.c               Startup, hub task, queue, offline detection, hub_get_room()
```

Every component has a `CMakeLists.txt`. Generated `build/`, `sdkconfig`, and `sdkconfig.old` files are not committed.

## What Each Component Does

| Component | Purpose | Main functions |
|---|---|---|
| `access_control` | Entrance and RFID access control, card validation, door state, entry/exit events and console | `entrance_start()` |
| `room_control` | One task per room: reads PIR, light follows occupancy, vacancy delay 10 s, counts light-on seconds, reports on change and every 5 s | `room_control_start(1/2)`, `room_control_set_publish_cb()`, `room_control_get()` |
| `storage` | Logs lines to `/sdcard/access.log` (RAM fallback), saves settings in flash | `storage_init()`, `storage_log()`, `storage_log_print()`, `storage_kv_set/get()` |
| `schedule` | Opening hours (saved), simulated clock and timestamped logging | `schedule_set/get()`, `schedule_is_open()`, `clock_set()`, `clock_set_scale()`, `schedule_log()` |
| `auth` | Salted SHA-256 PINs, 3 failed attempts = 30 s lockout, 120 s idle logout | `auth_login()`, `auth_require(ROLE_ADMIN)`, `auth_set_pin()` |
| `cocis_system` | System integration and synchronisation: startup, hub task, queue, room status and offline detection | `hub_get_room()` |

`cocis_system/main/main.c` starts storage, schedule and authentication, starts Rooms 1 and 2, and runs `hub_task`. The hub receives room reports through a queue, prints and logs changes, and marks a room OFFLINE after 15 seconds without a report. `hub_get_room()` is for the `ROOMS` command.

## Pin Plan (Must Not Clash)

| Use | GPIO |
|---|---|
| Room 1 PIR / LED | 32 / 21 |
| Room 2 PIR / LED | 33 / 22 |
| SD card SCK / MOSI / MISO / CS | 14 / 13 / 34 / 4 |
| Entrance servos (Dorcus) | 18, 19 |
| Entrance LEDs / buzzer (Dorcus) | 25, 26, 27 |


## Build and Run

```powershell
cd cocis_system
idf.py set-target esp32
idf.py build
```

Run `idf.py set-target esp32` only the first time.

Then in VS Code, press F1 and select **Wokwi: Start Simulator**. Click a PIR sensor to simulate motion.

Expected output:

```text
[ROOM n] OCCUPIED, light ON
[HUB] room n is now OCCUPIED
```

About 10 seconds after motion stops:

```text
[ROOM n] EMPTY, light OFF
```
