# CoCIS Smart Control and Monitoring System

Group coursework (BSE III, Embedded Systems): a smart control and monitoring system for the College of Computing and Information Sciences, simulated in Wokwi with ESP-IDF.

Following the lecturer's ruling, the whole system runs on **one ESP32**. Every module is a FreeRTOS component in the same firmware and shares the board's pins.

## Team and modules

| Module | Folder | Owner |
|---|---|---|
| Entrance and RFID access control | `components/access_control` | Dorcus Mukyala |
| Lecture Room 1 and Room 2 (occupancy, automatic lighting) | `components/room_control` | Mariam (Room 1), Arnold (Room 2) |
| SD card storage, opening hours and clock, PIN login | `components/storage`, `components/schedule`, `components/auth` | Joshua |
| System integration and synchronisation | `cocis_system` | Kiran |

The full folder layout and every component's functions are in [PROJECT_STRUCTURE.md](PROJECT_STRUCTURE.md).

## How it works

- `cocis_system/main/main.c` starts the components and the hub.
- **Entrance node** (`access_control`): three FreeRTOS tasks connected by queues, with a mutex protecting the shared state.
  `console_task -> scan_queue -> access_task -> actuator_queue -> actuator_task`
  - RFID scans are simulated by typing `in <UID>` or `out <UID>`.
  - A scan is granted or denied: unknown tag, blocked tag, anti-passback, access hours.
  - Granted: green LED and the entry or exit door servo opens for 3 s. Denied: red LED and buzzer.
  - Keeps the count of people inside and an event log in CSV form (`time,uid,name,point,result,reason`).
  - Emergency mode lets people leave outside access hours.
- **Room nodes** (`room_control`): read a PIR sensor, switch the room light, count light-on time and report to the hub through a queue.

## Pin plan

| Use | GPIO |
|---|---|
| Room 1 PIR / LED | 32 / 21 |
| Room 2 PIR / LED | 33 / 22 |
| SD card SCK / MOSI / MISO / CS | 14 / 13 / 34 / 4 |
| Entrance servos (entry / exit) | 18 / 19 |
| Entrance LEDs (grant / deny) and buzzer | 25 / 26 / 27 |

## Build

Requires ESP-IDF v5.3.x. In PowerShell (adjust the paths to your own installation):

```powershell
$env:IDF_TOOLS_PATH = "D:\Espressif"
D:\esp\v5.3.5\esp-idf\export.ps1
cd cocis_system
idf.py set-target esp32     # first time only
idf.py build
```

## Run in Wokwi

1. Open the `cocis_system` folder in VS Code.
2. Press F1 and choose **Wokwi: Start Simulator**.
3. Type commands in the serial monitor. If the VS Code terminal does not accept typing on your machine, add `rfc2217ServerPort = 4000` to your **local** `wokwi.toml` (do not commit it), start the simulator, and in a second terminal (after running the ESP-IDF setup above) run:
   ```powershell
   python -m serial.tools.miniterm rfc2217://localhost:4000
   ```

## Entrance console commands

| Command | What it does |
|---|---|
| `in <UID>` / `out <UID>` | Simulate a card scan at the entry or exit reader |
| `inside` | Who is inside now |
| `users` | Registered users |
| `log` | Recent access events (CSV) |
| `status` | Time, opening hours, emergency mode, people inside |
| `time HH:MM` | Set the simulated clock |
| `sched HH:MM HH:MM` | Set opening and closing time |
| `emergency on` / `emergency off` | Allow or block exit outside hours |
| `help` | List the commands |

Test cards: `A1B2C3D4` (Alice), `B2C3D4E5` (Brian), `C3D4E5F6` (Carol), `D4E5F6A7` (Dan, admin), `DEADBEEF` (blocked).

## Entrance tests

| Command | Expected result |
|---|---|
| `in A1B2C3D4` | GRANTED, green LED, entry servo opens |
| `in A1B2C3D4` (again) | DENIED, already inside (anti-passback), red LED and buzzer |
| `in 12345678` | DENIED, unknown tag |
| `in DEADBEEF` | DENIED, tag blocked |
| `inside` | Lists Alice |
| `out A1B2C3D4` | GRANTED, exit servo opens, inside = 0 |
| `time 21:00`, `in B2C3D4E5` | DENIED, outside access hours |
| `time 19:30`, `in D4E5F6A7`, `time 21:00`, `out D4E5F6A7` | Exit DENIED, outside access hours |
| `emergency on`, `out D4E5F6A7` | GRANTED, emergency exit |
| `log` | CSV lines of all events |

## Working on the repo

- Do not push directly to `main`. Work on your own branch (for example `access-control-component`) and open a pull request.
- Do not commit `build/`, `sdkconfig`, `.vscode/` or your local `wokwi.toml` changes.
- Before changing a file that belongs to someone else (for example `cocis_system/main/main.c` or `diagram.json`), tell its owner.
