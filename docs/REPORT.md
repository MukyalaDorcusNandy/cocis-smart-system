# CoCIS Smart Control and Monitoring System

**Course:** BSE III, Embedded Systems  
**Repository:** https://github.com/MukyalaDorcusNandy/cocis-smart-system  
**Firmware:** ESP-IDF v6.0.1 on one ESP32 DevKit C v4, simulated in Wokwi  
**Wokwi link:** [paste link]  
**Branch with the new features:** `feature/rfid-dashboard` (pull request into `main`)

## Group member details

| Name | Registration no. | Component owned |
|---|---|---|
| Mukyala Dorcus Nandy | 23/U/11911/EVE | Entrance: access control, doors (`access_control`); MFRC522 readers and driver (`rfid`); card roles, colours, buzzer pattern |
| Mariam Wambui | 23/U/18494/PS | Lecture rooms: PIR occupancy and lighting (`room_control`); rooms on the web dashboard |
| Kisomose Arnold Patrick | 23/U/10079/EVE | Lecture rooms (`room_control`), project structure and circuit (`diagram.json` layout, pin plan, `wokwi.toml`, `sdkconfig.defaults`); entrance LCD (`lcd_display`) |
| Katumba Joshua Mugwanya | 23/U/09330/PS | Storage and SD log, schedule and clock, authentication, management commands; web sign-in security |
| Nannyombi Shakiran | 23/U/11963/EVE | System integration: `main.c` and the hub task; Wi-Fi and web Security Dashboard (`web_dashboard`) |

---

## 1. System operation

One ESP32 runs the whole system as FreeRTOS tasks: the building entrance, lecture room 1 and lecture room 2. The three “terminals” of the brief are separate tasks that talk through queues instead of separate boards. The firmware is written in C on ESP-IDF v6.0.1 and runs in the Wokwi simulator. Two MFRC522 RFID readers (ENTRY and EXIT) read the cards, a 16x2 LCD at the door shows the result, and a web Security Dashboard over Wi-Fi lets security staff watch the building live.

This matches the lecturer’s ruling that the free Wokwi setup supports one microcontroller. Inter-board communication is therefore not required. Shared data structures and one owner per record are used so the three terminals cannot update the same record at the same time.

### 1.1 Architecture

The firmware is split into ten ESP-IDF components, each owned by one or two members. `main.c` starts them in a fixed order: storage, schedule, auth, hub, management commands, web dashboard commands, entrance LCD, room 1, room 2, entrance, RFID readers, Wi-Fi + web server.

| Component | Role | Tasks it runs |
|---|---|---|
| `access_control` | Entrance: card decisions, roles and colours, doors, LEDs, buzzer, serial console | `access` (priority 5), `actuator` (4), `console` (3) |
| `rfid` | Two MFRC522 readers on one SPI bus; sends each new card to the entrance | `rfid` (4) |
| `lcd_display` | 16x2 I2C LCD at the entrance: welcome / denied message, idle clock | `lcd` (3) |
| `web_dashboard` | Wi-Fi, HTTP server, PIN sign-in, live alerts, admin actions | ESP-IDF `httpd` (5) and Wi-Fi tasks |
| `room_control` | One instance per room: PIR occupancy and light control | `room1`, `room2` (5) |
| hub (in `cocis_system/main`) | Collects room reports and entrance events, detects offline rooms | `hub` (6) |
| `storage` | Settings in NVS flash; event log on the SD card plus a RAM copy | none |
| `schedule` | The single clock and the opening hours | none |
| `auth` | Admin PIN, login sessions, lockout | none |
| `management` | Admin commands added to the entrance console | none |

The brief describes three terminals: the entrance, room 1 and room 2. Here each terminal is a group of FreeRTOS tasks on the same ESP32, with its own pins, its own data and its own mutex, so it behaves like a separate unit.

### 1.2 Communication between the three terminals

How these three terminals communicate inside the one chip:

- **Entrance.** The `rfid` task reads the two readers. Each new card goes through `entrance_submit_scan()` as a message on `scan_queue`. A typed `in`/`out` and the web test scan use the same function. The access task decides and sends a door command on `actuator_queue`. The actuator task moves the servo and drives the LEDs and buzzer, so a 3-second door opening never blocks the next decision.
- **Rooms to hub.** Each room task sends its status (occupied, light on, light-on time, sequence number) on the hub queue whenever it changes, and as a heartbeat every 5 s.
- **Entrance to hub, web and LCD.** Every entry or exit decision, granted or denied, is sent to every listener (up to 4): the hub, the web dashboard's alert ring and the LCD queue. The hub announces building occupancy; the entrance remains the single owner of the who-is-inside list.
- **Offline detection.** If a room sends nothing for 15 s (three missed heartbeats), the hub marks it OFFLINE and logs it. It returns to ONLINE on the next report.
- **Lost messages.** Callbacks never wait on the hub queue. If the queue is full, the drop is printed. A room retries on the next heartbeat. Occupancy is not copied into the hub, so a dropped entrance event cannot make the `rooms` command show the wrong count.
- **Duplicates.** Each room report carries a sequence number. The hub ignores a report whose sequence matches the last one it stored.

The entrance runs as a chain of four tasks (rfid, console, access, actuator). Both rooms and the entrance feed the hub through one queue, and every task logs through the shared services (`schedule_log` → SD card / RAM).

### 1.3 Hardware components and pin plan

All parts are wired to the one ESP32 DevKit C v4 in `cocis_system/diagram.json`. The parts are placed in zones around the ESP32: the rooms (PIRs + room LEDs) at the top left, above the ESP32; the ENTRY reader and its door servo at the top right; the LCD on the right between the two readers; the EXIT reader and its door servo at the bottom right; the SD card, the green/red LEDs and the buzzer at the bottom left. The pin plan below has no clashes. It is also recorded in `room_config.h`, `access_control.c`, `rfid.c`, `lcd_display.c`, `storage.c` and `PROJECT_STRUCTURE.md`.

| Part | Purpose | ESP32 pins |
|---|---|---|
| PIR motion sensor (room 1) | Detects motion in room 1 | GPIO 32 |
| Yellow LED + 330 Ω (room 1) | Room 1 lights | GPIO 21 |
| PIR motion sensor (room 2) | Detects motion in room 2 | GPIO 33 |
| Yellow LED + 330 Ω (room 2) | Room 2 lights | GPIO 22 |
| Servo (entry door) | Opens the entry door for 3 s | GPIO 18 (PWM, 50 Hz) |
| Servo (exit door) | Opens the exit door for 3 s | GPIO 19 (PWM, 50 Hz) |
| Green LED + 220 Ω | Access granted | GPIO 25 |
| Red LED + 220 Ω | Access denied | GPIO 26 |
| Buzzer (volume 1) | One short beep when granted, two long beeps when denied (2 kHz) | GPIO 27 |
| 16x2 LCD (`wokwi-lcd1602`, I2C, address 0x27) | Welcome / denied message and the idle clock at the entrance | SDA 15, SCL 2 |
| MFRC522 reader, ENTRY (`board-mfrc522`) | Reads cards at the entry door | SPI3: SCK 16, MOSI 17, MISO 35, SDA (CS) 5; RST to 3.3 V |
| MFRC522 reader, EXIT (`board-mfrc522`) | Reads cards at the exit door | same SPI3 bus, SDA (CS) 23; RST to 3.3 V |
| microSD card (SPI2) | Log file `ACCESS.LOG` | SCK 14, MOSI 13, MISO 34, CS 4 |
| Serial monitor (UART0, 115200 baud) | Admin menu, backup typed scans | TX / RX |
| Wi-Fi (built in) | Joins `Wokwi-GUEST`; web dashboard on port 80 → http://localhost:8180 | — |

**The RFID readers.** The two readers are Wokwi's official `board-mfrc522` part (https://docs.wokwi.com/parts/board-mfrc522). To tap a card, click a reader in the simulator, pick a card (Blue, Green, Yellow, Red, NFC Tag or Key Fob) and press **Tap**; the card stays on the reader for about 0.5 s. With the keyboard, after clicking the card panel: `b` / `g` / `y` / `r` / `n` / `k` picks a card and `t` taps. **Hold** keeps the card on the reader. Both readers share one SPI bus; each has its own chip-select (SDA) pin, and RST is wired to 3.3 V so the readers always run. Typing `in <UID>` / `out <UID>` still works as a backup.

**The entrance LCD.** A 16x2 LCD (`wokwi-lcd1602` in I2C mode, address 0x27) on SDA GPIO 15 and SCL GPIO 2. When idle it shows `CoCIS  HH:MM:SS` and `In:N   OPEN` (or `CLOSED`). After every scan it shows the result for 3 s: `WELCOME Brian` / `Student BLUE card`, `BYE ...` on exit, or `DENIED ...` with a short reason (`Building closed`, `Already inside`, `Unknown card`, `Card blocked`, `Not inside`). It replaces the RGB LED of the earlier version: the card colour is now shown by the Wokwi card itself, the LCD text, the console and the dashboard.

### 1.4 Software design

**RFID driver (`rfid.c`).** It is a hand-written, register-level driver (no library). One register access is two bytes over SPI: the address byte `(reg << 1) & 0x7E` (+ `0x80` for a read), then the data. At start-up the driver does a soft reset, sets the timer (about 25 ms timeout), turns the antenna on and reads `VersionReg`. Any value except `0x00` or `0xFF` means a reader answered (`[RFID] ENTRY reader v0x%02X OK`); `0x00` or `0xFF` means nothing is on the bus (`NOT FOUND ..., use typed in/out`). Every 150 ms the `rfid` task asks each reader:

1. **WUPA** (wake up, 7 bits): a card answers with 2 bytes (ATQA). If not, there is no card (tried twice).
2. **Anticollision** (`0x93 0x20`): the card sends 4 UID bytes + BCC. The driver checks BCC = UID0 ^ UID1 ^ UID2 ^ UID3, so a corrupted read is ignored. The grey NFC Tag has a 7-byte UID; here it answers `0x88` (cascade tag) + its first 3 bytes, so it reads as `88041122`. That ID is not registered, so it is denied as an unknown tag.
3. **Debounce:** a card is reported once (`[RFID ENTRY] card 01020304`). The same card is reported again only after 2 empty polls (card removed).

**Card roles and colours.** Every card has a role: Student (blue), Staff (green), Guest (yellow), Admin (red). The test cards match the Wokwi card presets, so the colour of the card you tap is the role colour. The LCD shows it (`Student BLUE card`), `inside` and `users` print the role and colour, `inside` also shows the time the person came in, and the dashboard shows a colour chip.

**Entrance decision.** For each scan the access task checks, in this order:

1. Is the card registered? If not: unknown tag.
2. Is it blocked (lost or stolen)? If so: tag blocked.
3. Entry only: is the person already inside? If so: already inside (anti-passback).
4. Exit only: is the person inside? If not: not inside.
5. Is the building open now? If not, entry is denied, and exit is denied unless emergency mode is on (outside access hours or emergency exit).

A granted decision updates the “who is inside” record, opens the matching door for 3 s, lights the green LED and gives one short beep. A denied one lights the red LED and gives two long beeps. Every decision is printed, shown on the LCD and written to the system log.

**Safety note.** The assignment restricts both entry and exit outside configured hours. A real building must always preserve emergency egress. In this simulation, exit is denied outside hours unless an authenticated administrator turns on emergency mode. That models the coursework rule and still provides a safe override.

**Room control.** Each room task polls its PIR every 100 ms. Motion switches the light on at once. The light goes off 10 s after the last motion (a demo value; about 5 minutes in real use), because a PIR sees movement, not presence. Each room also adds up how long its light has been on.

**Schedule and clock.** One simulated clock starts at 08:00 at boot. The default opening hours are 08:00–18:00. Opening time must be before closing time. The hours are saved in NVS, so they survive a reset.

**Storage and logging.** At boot the SD card is mounted over SPI and every event is appended to `/sdcard/ACCESS.LOG` as `seq,HH:MM:SS,EVENT,details`. The last 20 lines are also kept in RAM. If the card cannot be mounted, or a write fails, the system keeps running in RAM-only mode and says so at boot. Logged events include card decisions, room changes, rooms going offline, logins, schedule and clock changes, emergency mode and card blocking.

**Web Security Dashboard (`web_dashboard`).** `wifi.c` joins the open Wokwi network `Wokwi-GUEST` and retries forever; the system does not wait for it. `web_dashboard.c` runs an ESP-IDF HTTP server on port 80; Wokwi forwards it to http://localhost:8180 (`wokwi.toml`). The page (`dashboard.html`) is built into the firmware. It polls `GET /api/state` every 1.5 s and shows: people inside per role, who is inside (colour chips and since-time), who is out, the rooms (online, occupied, light, light-on time), and a live alert feed. Every new alert pops up as a toast with a beep. A denied, unknown or blocked card is a red **SECURITY ALERT** with an alarm tone. Admin actions: opening hours, emergency on/off, block/unblock a card, and a test scan (`POST /api/hours`, `/api/emergency`, `/api/block`, `/api/scan`).

**Web sign-in.** `POST /api/login` checks the PIN with `auth_check_pin()`. This uses the **same** 3-tries / 30 s lockout counter as the console `login`, so switching between console and web gives no extra guesses. A correct PIN gives a random 16-hex-character token. Every API call must send it in the `X-Token` header; without it, or after 10 minutes idle, the answer is `401` and the page goes back to sign-in. Web actions are logged as `WEB,...` lines (for example `WEB,emergency ON`, `WEB,block C0FFEE99`), and sign-ins as `AUTH,web login OK/failed`.

**Authentication.** Changing any setting needs an admin login. The PIN is never stored in plain text: NVS holds a random 16-byte salt and the SHA-256 hash of salt + PIN, compared in constant time. Three wrong PINs lock login for 30 s. A session ends after 120 s without a protected command. The default PIN `1234` is created on first boot and the system warns until it is changed. The PIN is never printed or logged, and the console shows `****` while it is typed.

### 1.5 Synchronisation

Every shared record has one owner and one lock, so two tasks never update it at the same time.

| Shared data | Protected by | Used by |
|---|---|---|
| Card table, inside flags, inside count, entrance log | `state_mutex` | access task, console commands |
| Each room’s status | the room’s own mutex | room task, hub |
| Room table in the hub (online / last report) | hub mutex | hub task, `rooms` command |
| Clock and opening hours | schedule mutex | entrance, management commands |
| Log ring and SD file | storage mutex | every component that logs |
| Login session and lockout (one counter for console and web) | auth mutex | management commands, web sign-in |
| SPI3 bus shared by the two readers | ESP-IDF SPI master driver (one device handle and CS pin per reader, one transaction at a time) | rfid task |
| LCD screen | only the `lcd` task writes it; decisions reach it through `lcd_queue` | lcd task |
| Web alert ring (30 alerts) and session token | `s_lock` mutex in web_dashboard | access task (writes alerts), HTTP task (reads) |

Tasks pass work through queues instead of calling each other: `scan_queue` (fed by the rfid task, the console and the web through `entrance_submit_scan()`) and `actuator_queue` at the entrance, the hub queue for room and entrance events, and `lcd_queue` for the LCD. The callbacks that feed the hub only enqueue a message with no wait, so a slow hub can never stall a room or the entrance.

Building occupancy has one owner: `access_control`. The hub does not keep a second copy. The `rooms` command asks the entrance (`entrance_inside_count()`) and the web page asks for a copy of the card table (`entrance_get_users()`), so a dropped message cannot make occupancy inconsistent.

### 1.6 Implemented features and serial menu

All commands are typed in the Wokwi serial monitor. Viewing needs no login; changing a setting does. Command names follow the coursework suggestions (`HELP`, `STATUS`, `WHO_INSIDE`, `ROOMS`, `LOGS`, `SET_SCHEDULE`) with shorter words that fit a serial console.

| Command | What it does | Login |
|---|---|---|
| `in <UID>` / `out <UID>` | Backup: typed card scan at the entry or exit reader | no |
| `inside` | Who is inside and how many, with role, colour and since-time | no |
| `log` | Recent entrance decisions | no |
| `status` | Clock, hours, open or closed, emergency mode, people inside | no |
| `rooms` | Each room: online or offline, occupied or empty, light on or off, light-on time; building occupancy | no |
| `users` | Registered cards: role, colour, inside/outside, blocked | no |
| `wifi` | Wi-Fi status, IP and dashboard address | no |
| `hours` / `clock` | Show opening hours / show the clock | no |
| `login <pin>` / `logout` | Start or end an admin session | no |
| `setpin <new>` | Change the admin PIN (4–8 digits) | yes |
| `hours HH:MM HH:MM` | Set opening hours | yes |
| `clock HH:MM` | Set the simulated clock | yes |
| `syslog [n]` | Last n system log lines from RAM and the SD card | yes |
| `emergency on\|off` | Allow exit outside opening hours | yes |
| `block <UID>` / `unblock <UID>` | Block or unblock a lost or stolen card (saved in NVS) | yes |
| `roomfail <n> on\|off` | Test aid: make a room stop reporting, to show offline detection | yes |
| `help` | List all commands | no |

Registered cards are fixed in the firmware and match the Wokwi card presets: Alice (Staff) Green Card `11223344`, Brian (Student) Blue Card `01020304`, Grace (Guest) Yellow Card `55667788`, Dan (Admin) Red Card `AABBCCDD`, and the lost card, the orange Key Fob `C0FFEE99`, which starts blocked. Carol (Student, blue) `C3D4E5F6` has no Wokwi card and is used with typed scans. The grey NFC Tag is not registered: it reads as `88041122` and is denied as an unknown tag.

The web dashboard gives the same information in a browser, plus live alerts (section 1.4).

### 1.7 Requirements coverage

| Brief | How the system meets it |
|---|---|
| RFID access | Two MFRC522 readers (Wokwi `board-mfrc522`, own register-level driver); registered vs unknown vs blocked cards; grant/deny drives doors, LEDs, buzzer and the LCD |
| Entry and exit tracking | Two scan points, anti-passback, “who is inside” record and count (`inside`) |
| Two smart rooms | PIR per room; light on with motion, off 10 s after the last motion |
| Single ESP32 | One DevKit in `diagram.json`; entrance and both rooms share this controller |
| Data storage | Access decisions, occupancy changes and system activity appended to `ACCESS.LOG` (NVS for settings; RAM fallback if SD fails) |
| Serial management interface | `help`, `status`, `inside`, `rooms`, `log`, `syslog`, `wifi`, plus protected setting commands. Extra interface: the web Security Dashboard |
| Configurable schedule | Default 08:00–18:00; only a logged-in admin (console or web) can change hours or the clock; entrance enforces the hours |
| Authentication | Salted SHA-256 PIN, lockout, idle timeout; schedule and other privileged commands require login |
| Synchronisation | One mutex per shared record, one owner for occupancy, queues between tasks, SPI driver for the shared reader bus (section 1.5) |

### 1.8 Innovation / additional features

The brief asks for innovation. These features go beyond the basic requirements:

| Feature | Why it helps |
|---|---|
| **Two MFRC522 readers with a hand-written register-level driver (no library)** | The driver does register access over SPI, WUPA, anticollision and the BCC check itself, and two readers share one SPI bus with separate chip selects. The same code would work with a real reader. |
| **Entrance LCD** | A 16x2 I2C LCD greets the person by name with role and card colour, gives a short reason when denied, and shows the clock, people inside and OPEN/CLOSED when idle. |
| **Colour-coded roles** | Student blue, Staff green, Guest yellow, Admin red, matching the Wokwi card colours. A guard sees at a glance who came in; the colour is also on the LCD, the console and the dashboard. |
| **Wi-Fi Security Dashboard with live alerts** | Security staff watch who is inside, who is out and the rooms from a browser. Denied, unknown and blocked cards pop up as red alerts with an alarm tone. |
| **Shared lockout across console and web** | One failure counter for both, so an attacker cannot get extra PIN guesses by switching interface. |
| **Offline detection** | A room that stops reporting for 15 s is shown OFFLINE and logged. |
| **Emergency mode** | An admin can allow exit outside opening hours (safe egress), and it is logged. |
| **Salted PIN hash** | The PIN is stored as salt + SHA-256 hash, compared in constant time, never printed or logged. |
| **SD logging with RAM fallback** | Every event goes to `ACCESS.LOG`; if the card fails, the system keeps a RAM log and says so. |

---

## 2. Wokwi simulation link

**Wokwi link:** [paste link]  
**Source code:** https://github.com/MukyalaDorcusNandy/cocis-smart-system

The Wokwi circuit lives in the repository as `cocis_system/diagram.json` and `cocis_system/wokwi.toml`. Open `cocis_system` in VS Code, build the firmware, then start the simulator.

To run it locally in VS Code: build the `cocis_system` project with ESP-IDF v6.0.1 (`rm -f sdkconfig` once, then `idf.py build`), then press F1 and choose **Wokwi: Start Simulator**. Click inside the Wokwi terminal to type commands; `help` lists them. A quick demonstration:

1. Click the ENTRY reader, pick the **Green Card** and press **Tap**: Alice enters, the green LED lights, the LCD shows `WELCOME Alice` / `Staff GREEN card` and the entry servo opens.
2. Click PIR 1: room 1 lights up and the hub prints `[HUB] room 1 OCCUPIED, light ON`.
3. `login 1234`, then `clock 19:00`: the building is now closed.
4. Tap the Green Card on the EXIT reader (or `out 11223344`): exit is denied outside hours (LCD `Building closed`). `emergency on`, then tap again: exit is granted.
5. `syslog 10`: the same events, read back from the SD card.
6. Open http://localhost:8180, sign in with the PIN, and tap the NFC Tag (unknown card): a red SECURITY ALERT pops up.

---

## 3. Individual video demonstrations

Each member recorded their screen while explaining the code they wrote and showing it running in Wokwi.

| Member | Video link | What the video shows |
|---|---|---|
| Mukyala Dorcus Nandy | [paste link] | `rfid.c` MFRC522 driver (register access over SPI, WUPA, anticollision, BCC, debounce, two readers on one bus with separate CS); `access_control.c` decision rules, roles and colours, buzzer pattern. Demo: tapping the Wokwi cards on both readers, granted/denied, colours |
| Mariam Wambui | [paste link] | `room_control.c`: PIR polling, the vacancy timer and light control; the room provider adapter and the rooms panel on the dashboard. Demo with PIR 1 and PIR 2 in Wokwi and in the browser |
| Kisomose Arnold Patrick | [paste link] | `lcd_display.c` (PCF8574 bits, 4-bit mode, lcd task and queue); the circuit in `diagram.json` (zones, readers, LCD), pin plan, `wokwi.toml` (port forward), `sdkconfig.defaults`. Demo of the LCD messages and the circuit |
| Katumba Joshua Mugwanya | [paste link] | storage, schedule, auth, management: SD log, opening hours, PIN hashing and lockout; `auth_check_pin` shared lockout, tokens, 401. Demo of `login`, `hours`, `emergency`, web sign-in lockout and `syslog` |
| Nannyombi Shakiran | [paste link] | `main.c` start-up order, `hub.c`, `wifi.c`, `web_dashboard.c` and `dashboard.html`: hub queue, offline detection, alert ring, event listeners. Demo of `rooms`, `roomfail`, `wifi` and the live dashboard |

---

## 4. Individual contributions

Each member owned one part of the system, developed it on their own branch and merged it through a pull request. The parts were then brought together and tested as one firmware on the `integrate` branch.

### Mukyala Dorcus Nandy: entrance and access control

- Designed the entrance as three tasks (console, access, actuator) joined by `scan_queue` and `actuator_queue`.
- Wrote the access decision: registered card, blocked card, anti-passback, opening hours and emergency exit.
- Drove the hardware: entry and exit servos (LEDC PWM), green and red LEDs, buzzer.
- Built the serial console with simulated RFID (`in` / `out`), character echo, PIN masking, and the command table other components register into.
- Kept the “who is inside” record and the entrance log; added card blocking saved in NVS.
- Added the entrance parts to `diagram.json` and wrote the first README.
- **New:** wrote the `rfid` component: MFRC522 register access over SPI3, soft reset and antenna set-up, WUPA request, anticollision with BCC check, debounce, and the `rfid` task that calls `entrance_submit_scan()`.
- **New:** made the driver work with the two official Wokwi `board-mfrc522` readers on one SPI bus (separate chip selects 5 and 23); the version check accepts any answer except `0x00` / `0xFF`.
- **New:** added card roles and colours (`entrance_role_t`, `entrance_role_name()`, `entrance_role_color()`) and matched the card table to the Wokwi presets (Blue Brian, Green Alice, Yellow Grace, Red Dan, orange Key Fob blocked); role/colour/since-time in `inside` and `users`.
- **New:** the buzzer pattern: one short beep when granted, two long beeps when denied.

### Mariam Wambui: lecture-room occupancy and lighting

- Wrote the room task: PIR polling every 100 ms, light on with motion, off after the vacancy delay.
- Added light-on time tracking and the room status record (occupied, light on, light-on seconds).
- Wired and tested both PIR sensors and room LEDs in Wokwi.
- Updated the application start-up (`app_main`) to run the rooms.
- **New:** rooms on the web dashboard: room cards (online/offline, occupied, light, light-on time) fed by the room provider adapter (`web_room()` in `main.c` → `hub_get_room()`).
- **New:** room part of the test plan (sections 3, 4 and the dashboard room checks).

### Kisomose Arnold Patrick: room component, project structure, circuit and entrance LCD

- Made `room_control` one reusable component that runs room 1 and room 2 in the same firmware, each with its own pins, task and mutex.
- Wrote `room_config.h` (pin plan, poll interval, vacancy delay, heartbeat) and the status callback that reports each room to the rest of the system.
- Set up the single-ESP32 project (`cocis_system`) and the shared `components/` layout.
- Maintained the repository: `.gitignore`, removal of the old per-node folders, and `PROJECT_STRUCTURE.md`.
- **New:** wrote the `lcd_display` component: the PCF8574 I2C backpack bits (RS, EN, backlight, D4–D7), the HD44780 4-bit start-up, the welcome / denied / idle screens, and the `lcd` task fed by a queue from an entrance event listener, so the entrance never waits for the slow I2C bus. The system keeps running if no LCD answers.
- **New:** wired the two `board-mfrc522` readers (shared SPI3 bus, CS 5 and 23, RST to 3.3 V) and the LCD (SDA 15, SCL 2) in `diagram.json`, laid the circuit out in zones so no parts overlap, and checked the pin plan has no clashes.
- **New:** `wokwi.toml`: the port forward `localhost:8180` → ESP32 port 80. `sdkconfig.defaults`: large app partition, 4 MB flash, bigger event task stack and HTTP header size.

### Katumba Joshua Mugwanya: storage, schedule, authentication and management

- **storage:** NVS settings, SD card mounting over SPI, the `ACCESS.LOG` file, the RAM log and the RAM-only fallback.
- **schedule:** the single simulated clock, opening hours saved in NVS, and the shared `schedule_log` used by every component.
- **auth:** salted SHA-256 PIN storage, constant-time comparison, 3-try lockout, 120 s idle timeout, first-boot default PIN.
- **management:** the admin commands `login`, `logout`, `setpin`, `hours`, `clock`, `syslog`, `emergency`, `block`, `unblock`.
- **New, web sign-in security:** `auth_check_pin()` shares the lockout counter with the console `login`; random token sessions (10 min idle); `401` for a missing or old token (the page returns to sign-in); `WEB,...` log lines for every web action and `AUTH,web login ...` for sign-ins.

### Nannyombi Shakiran: system integration and hub

- Owned `cocis_system/main/main.c`: the start-up order of all components and error checks on each. Shared services start first (storage → schedule → auth), then the hub, then management commands, then the two rooms, then the entrance last so every command is registered before the console prints help.
- Owned the hub (`hub.c`, `hub.h`): one FreeRTOS queue for room reports and entrance events, 15 s offline detection, and the `rooms` / `roomfail` commands.
- Agreed the message format with the group. Room messages are `room_status_t` (room number, occupied, light, light-on seconds, sequence). Entrance messages are `entrance_event_t` (entry/exit, UID, name, granted, reason, inside count). Both are delivered through callbacks that only enqueue, so a slow hub cannot stall a terminal.
- Made access_control the single owner of building occupancy. The hub announces granted entries and exits; `rooms` reads the count from the entrance instead of keeping a second copy.
- Handled a temporarily silent terminal: after 15 s without a room heartbeat the hub marks that room OFFLINE, logs it, and restores ONLINE on the next report. `roomfail` is the integration test for that path.
- Detected duplicate room reports with the per-room sequence number, and printed a message if the hub queue was full.
- Wrote the integration tests in `docs/TEST_PLAN.md` sections 0 (boot / first hub reports), 3 (rooms to hub) and 4 (offline detection).
- **New, Wi-Fi + web dashboard:** `wifi.c` (station on `Wokwi-GUEST`, retry forever, non-blocking), `web_dashboard.c` (HTTP server, JSON API, alert ring of 30 with a mutex, `wifi` command) and `dashboard.html` (live view, toasts, beeps, admin actions).
- **New:** changed `entrance_set_event_cb()` to allow several listeners (hub + web), and updated the start-up order in `main.c` (web commands before `entrance_start()`, readers and Wi-Fi after). Merged the feature branches.

**Shared work:** all members agreed the pin plan and command names, reviewed each other’s pull requests, and wrote this report together.

---

## 5. Testing and limitations

The firmware builds with no errors on ESP-IDF v6.0.1. It was tested in Wokwi with the test steps in `docs/TEST_PLAN.md`, covering boot, RFID readers, card roles and colours, the entrance LCD, card scans, opening hours, rooms, offline detection, login security, the SD log, Wi-Fi and the web dashboard.

### Integration tests (owner: Kiran)

| Test | What it shows |
|---|---|
| Boot (TEST_PLAN 0.5) | Both rooms report to the hub: `ONLINE`, then `EMPTY, light OFF` |
| Granted entry (1.1) | Hub prints `building occupancy: 1` after the entrance decision |
| PIR click (3.1–3.4) | Room task and hub both print the occupancy / light change; `rooms` shows ONLINE and light-on time |
| `roomfail` without login (4.1) | Privileged integration command is protected |
| `roomfail 2 on` (4.2–4.3) | After 15 s the hub marks room 2 OFFLINE; the room LED still works (only the report path is cut) |
| `roomfail 2 off` (4.4) | Next heartbeat brings the room ONLINE again |
| `rooms` after scans | Building occupancy matches `inside` (one owner) |

### Limitations

- The MFRC522 driver only reads the card UID (WUPA, anticollision level 1). It does not do card select, authentication or reading card memory, and a 7-byte UID is cut to its first 4 bytes.
- The web dashboard uses HTTP, not HTTPS, so the PIN and token are not encrypted on the network. A real system needs HTTPS.
- Only one web session at a time: a new sign-in replaces the old token.
- Registered cards are fixed in the firmware; adding a card means rebuilding.
- A PIR detects movement, not people, so rooms show occupied or empty but not who or how many.
- The “who is inside” record is kept in RAM and is reset by a reboot. Blocked cards and settings are kept in NVS.
- The clock is simulated and starts at 08:00 at every boot (no real-time clock or internet time).
- GPIO 2 and 15 (LCD I2C) are ESP32 strapping pins. This is fine in Wokwi; on a real board other pins would be safer.
- There is one admin role with one PIN.
- Overnight opening hours (close after midnight) are not supported.
- Restarting the Wokwi simulator usually clears flash and the SD card; NVS persistence can only be shown with a reset inside the same simulation.
