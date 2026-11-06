# Video Guide

Each member records one video of about **3–4 minutes**. The brief says: explain the system and show the code and features **you** worked on. The lecturer will ask questions about your part, so read your "Likely questions" section well.

How to use this guide:

1. Start with the shared **System overview** (about 1 minute). Everyone can read the same text.
2. Then do **your** section: intro, code tour, live demo, and be ready for the questions.
3. Speak slowly. Short sentences are fine. Show the code on screen while you talk.

Line numbers below are approximate (`file:line`). Search for the function name if a line moved.

## Recording checklist (do this before you press Record)

- [ ] Open the `cocis_system` folder in VS Code, in an ESP-IDF terminal.
- [ ] Get the latest code: `git fetch`, `git checkout feature/rfid-dashboard`, `git pull`.
- [ ] Build: `rm -f sdkconfig` (only the first time after pulling), then `idf.py build`. It must finish with no errors.
- [ ] F1 → **Wokwi: Start Simulator**. Wait for the boot lines: `[LCD] 16x2 LCD ready at 0x27 (SDA 15, SCL 2)`, `[RFID] ENTRY reader v0x.. OK`, `[RFID] EXIT reader v0x.. OK`, `[WIFI] connected, IP ...`.
- [ ] Open **http://localhost:8180** in the browser. Sign in with PIN **1234** (or the PIN you set).
- [ ] Turn the computer sound on (the buzzer and the dashboard beep).
- [ ] Put the code editor, the Wokwi diagram + terminal, and the browser where you can switch quickly.
- [ ] Practise tapping a card: click a reader, pick a card, press **Tap** (keyboard: `b`/`g`/`y`/`r`/`n`/`k` then `t`).
- [ ] After recording: upload the video and **send the link to Kiran by 23:30** (it goes in `docs/REPORT.md` section 3).

## Shared system overview (about 1 minute, everyone can reuse)

> "Our project is the CoCIS Smart Control and Monitoring System. The lecturer allowed one microcontroller in free Wokwi, so one ESP32 runs the whole system. Inside it, the entrance, lecture room 1 and lecture room 2 are separate FreeRTOS tasks. They talk through queues, and every shared record has one mutex.
>
> At the entrance there are two MFRC522 RFID readers, ENTRY and EXIT, with our own driver. The card colour is the role: blue student, green staff, yellow guest, red admin. A card is granted or denied. Granted opens the door servo, lights the green LED and gives one short beep; the LCD says, for example, WELCOME Brian, Student BLUE card. Denied lights the red LED, gives two long beeps, and the LCD gives the reason.
>
> Each room has a PIR sensor. Motion turns the light on, and it goes off 10 seconds after the last motion. The hub task collects all reports and marks a room OFFLINE if it is silent for 15 seconds.
>
> Opening hours are 08:00 to 18:00. Outside the hours both entry and exit are denied, unless an admin turns on emergency mode. Settings need a PIN login. Everything is logged to the SD card. And over Wi-Fi there is a Security Dashboard in the browser with live alerts."

---

## 1. Mukyala Dorcus Nandy — MFRC522 readers, card roles and the buzzer

**(1) Intro (one sentence).** "I built the entrance: the driver for the two MFRC522 RFID readers, the card roles with their colours, and the buzzer pattern."

**(2) Code tour.**

| Open | Say |
|---|---|
| `components/rfid/rfid.c` top comment and pins (lines 1–40) | "The two readers are Wokwi's official MFRC522 part. Both are on SPI3: SCK 16, MOSI 17, MISO 35. Chip select 5 is ENTRY, 23 is EXIT. RST is wired to 3.3 V, so the readers always run." |
| `rfid.c` `reg_write()` / `reg_read()` (~92, ~102) | "We use no library. One register access is two bytes over SPI: the address byte is `(reg << 1) & 0x7E`, plus `0x80` for a read, then the data. This is from the MFRC522 datasheet." |
| `rfid.c` `mfrc_init()` (~117) | "Soft reset, timer about 25 ms, antenna on." |
| `rfid.c` `mfrc_request()` (~168) and `mfrc_anticoll()` (~176) | "WUPA asks: is a card there? The card answers with ATQA. Then anticollision reads 4 UID bytes and the BCC. If BCC is not the XOR of the four bytes, we ignore the read." |
| `rfid.c` `read_card()` (~191) | "The UID becomes 8 hex characters, like `01020304` for the Blue Card. The grey NFC Tag has a 7-byte UID, so here we get `88` plus its first 3 bytes: `88041122`. It is not registered, so it is denied." |
| `rfid.c` `poll_reader()` (~207) | "Debounce: a card is reported once. It is reported again only after 2 empty polls, so holding a card does not open the door again and again." |
| `rfid.c` `rfid_task()` (~224) | "At start it reads `VersionReg`. Any value except `0x00` or `0xFF` means a reader answered. Then every 150 ms it polls both readers and calls `entrance_submit_scan()`. The two readers share one bus; the ESP-IDF SPI driver gives each one its own device handle and CS pin and runs one transaction at a time." |
| `components/access_control/access_control.c` `entrance_submit_scan()` (~237) | "This puts the scan on `scan_queue`. The typed `in`/`out` command and the web use the same function, so the decision code is the same." |
| `access_control.c` `users[]` (~108), `role_colors[]` (~155) | "Every card has a role, and the cards match the Wokwi presets: Blue Card is Brian, a student; Green is Alice, staff; Yellow is Grace, a guest; Red is Dan, admin. The orange Key Fob is a lost card, blocked." |
| `access_control.c` `process_scan()` (~405) | "The rules in order: unknown, blocked, anti-passback, opening hours, emergency exit." |
| `access_control.c` `actuator_task()` (~506) and `beep()` (~393) | "Granted: green LED, door open 3 s, one short beep. Denied: red LED and two long beeps, so you can hear the difference without looking." |

**(3) Live demo.**

1. Show the boot lines `[RFID] ENTRY reader v0x.. OK` and `[RFID] EXIT reader v0x.. OK`.
2. Click the ENTRY reader, pick the **Blue Card** and press **Tap**. Show `[RFID ENTRY] card 01020304` and `>> ENTRY 01020304 (Brian (Student), blue card) -> GRANTED`. Point at the servo, the green LED and the LCD; listen for the short beep.
3. Press **Hold** on a card for 3 s: only one line (debounce).
4. Tap the **Yellow Card** (Grace, guest) and the **Red Card** (Dan, admin): the colour of the card is the role colour.
5. Tap the Blue Card on ENTRY again: `DENIED [already inside (anti-passback)]`, red LED, two long beeps.
6. Tap the **NFC Tag**: `card 88041122`, `DENIED [unknown tag]`. Tap the **Key Fob**: `DENIED [tag blocked]`.
7. EXIT reader, Tap the Blue Card: Brian leaves. Type `inside` to show role, colour and since-time.
8. Type `in C3D4E5F6` (Carol) to show the typed backup still works.

**(4) Likely questions.**

- *Why write your own driver?* — We wanted to understand the reader, not hide it in a library. The driver talks to the MFRC522 registers directly, so the same code would work with a real reader.
- *What is BCC?* — Block Check Character: the XOR of the 4 UID bytes. The card sends it after the UID. If it does not match, the read was corrupted and we ignore it.
- *How do two readers share one bus?* — Same SCK, MOSI, MISO; each has its own chip-select pin. The ESP-IDF SPI driver does one transaction at a time, and only the reader whose CS is low answers.
- *What if a reader is missing?* — `VersionReg` reads `0x00` or `0xFF`, so `rfid_task` prints `NOT FOUND ... use typed in/out` and skips that reader. The rest of the system keeps working.

---

## 2. Mariam Wambui — Lecture rooms and the rooms panel

**(1) Intro.** "I built the lecture-room part: PIR occupancy and automatic lights, and the rooms panel on the web dashboard."

**(2) Code tour.**

| Open | Say |
|---|---|
| `components/room_control/include/room_config.h` | "Room 1: PIR 32, LED 21. Room 2: PIR 33, LED 22. Poll every 100 ms, vacancy delay 10 s, heartbeat 5 s." |
| `components/room_control/room_control.c` `room_task()` (~49) | "Each room has its own task. It reads the PIR. Motion turns the light on at once. 10 s after the last motion the light goes off. It also adds up the light-on seconds." |
| `room_control.c` `publish()` (~27) | "The room sends its status when it changes and every 5 s as a heartbeat. The status has a sequence number." |
| `room_control.c` `room_control_get()` (~136) | "Other parts can read the status safely; each room has its own mutex." |
| `cocis_system/main/main.c` `web_room()` (~18) | "The web component cannot call the hub directly, because `main/` is not a component. So main.c gives it this small adapter. It calls `hub_get_room()` and copies online, occupied, light and light-on time." |
| `components/web_dashboard/web_dashboard.c` `h_state()` (~242), the `"rooms"` part | "The dashboard asks for each room 1, 2, ... until the adapter says there is no such room, and puts it in the JSON." |
| `components/web_dashboard/dashboard.html`, "Lecture rooms" section | "The page draws one card per room: ONLINE or OFFLINE, occupied or empty, light on or off, and light-on time." |

**(3) Live demo.**

1. Show the dashboard **Lecture rooms** cards: both online, empty, light off.
2. Click PIR 1 in Wokwi. Show `[ROOM 1] OCCUPIED, light ON` and `[HUB] room 1 OCCUPIED, light ON`, the room 1 LED, and the room 1 card on the dashboard.
3. Wait until the PIR stops plus 10 s: `[ROOM 1] EMPTY, light OFF`. Show the light-on time on the card and with `rooms`.
4. Do the same with PIR 2.
5. `login 1234`, `roomfail 2 on`. After 15 s the room 2 card shows OFFLINE. `roomfail 2 off`: online again.

**(4) Likely questions.**

- *Why 10 seconds?* — It is a demo value so the video is short. In a real building it would be about 5 minutes, because a PIR sees movement, not a person sitting still.
- *Can the room tell how many people are inside?* — No. A PIR only says motion or no motion. The building count comes from the RFID readers.
- *Why does the web use an adapter?* — The hub is in `main/`, which is not a component, so `web_dashboard` cannot include `hub.h`. `main.c` passes a function pointer instead.
- *What if the hub queue is full?* — The room does not wait. The drop is printed, and the next heartbeat (within 5 s) sends the status again.

---

## 3. Kisomose Arnold Patrick — Entrance LCD, project structure and the circuit

**(1) Intro.** "I did the entrance LCD, the project structure and the circuit: the one-ESP32 layout, the wiring and the zones in the diagram, the pin plan, and the Wokwi and build settings."

**(2) Code tour.**

| Open | Say |
|---|---|
| `components/lcd_display/lcd_display.c` top comment (lines 1–12) | "The LCD is a 16x2 `wokwi-lcd1602` in I2C mode at address 0x27, on SDA 15 and SCL 2. The I2C backpack is a PCF8574: one I2C byte sets 8 pins. Bit 0 is RS, bit 2 is EN, bit 3 is the backlight, and bits 4 to 7 are the data lines D4 to D7." |
| `lcd_display.c` `lcd_send()` (~44) | "Only 4 data lines, so each byte goes as two halves: high 4 bits, then low 4 bits. For each half we set EN high, then low; the LCD reads the data on the falling edge. That is 4-bit mode." |
| `lcd_display.c` `lcd_init()` (~68) | "The standard start-up: three times 8-bit mode, then switch to 4-bit mode. Then 2 lines, display on, clear." |
| `lcd_display.c` `show_event()` (~108) and `short_reason()` (~98) | "After a scan: `WELCOME Brian` and `Student BLUE card`, or `BYE` on exit. When denied, a short reason that fits 16 characters, like `Building closed` or `Card blocked`." |
| `lcd_display.c` `on_entrance_event()` (~144) and `lcd_task()` (~149) | "The entrance calls our listener after each decision. The listener only copies the event into a queue, so the entrance never waits for the slow I2C bus. Only the `lcd` task, priority 3, talks to the LCD. It waits up to 1 s for an event; if none comes it shows the clock, people inside and OPEN or CLOSED." |
| `lcd_display.c` `lcd_display_start()` (~169) | "If no LCD answers at 0x27 we print `no LCD at 0x27, running without it`, and the system keeps working." |
| `cocis_system/main/main.c` (~50) | "`lcd_display_start()` is called before `entrance_start()`, because it adds an entrance listener." |
| `cocis_system/diagram.json`, parts `lcd`, `rfid_entry`, `rfid_exit` (type `board-mfrc522`) | "The LCD has `pins: i2c` and address 0x27. The two readers are Wokwi's official MFRC522 part." |
| `diagram.json` connections | "Both readers use SCK 16, MOSI 17, MISO 35. ENTRY SDA goes to 5, EXIT SDA to 23. RST goes to 3.3 V. LCD SDA to 15, SCL to 2." |
| `PROJECT_STRUCTURE.md`, pin plan | "Every pin is used once. The SD card is on SPI2 (14/13/34/4) and the readers on SPI3, so the two buses never clash." |
| `cocis_system/wokwi.toml` | "`[[net.forward]]` sends `localhost:8180` to ESP32 port 80, so the browser can open the dashboard." |
| `cocis_system/sdkconfig.defaults` | "Wi-Fi and the web server make the app bigger than 1 MB, so we use the large app partition and 4 MB flash. We also give the event task a bigger stack and allow longer HTTP headers for the token." |

**(3) Live demo.**

1. Show the Wokwi diagram and the zones: rooms (PIRs + LEDs) on the left, the ESP32 in the middle, the LCD on top, the readers and door servos on the right, green/red LEDs and buzzer at the bottom right, the SD card at the bottom.
2. Hover or click a wire to show it goes to the pin in the table.
3. Start the simulator. Show `[LCD] 16x2 LCD ready at 0x27 (SDA 15, SCL 2)` and `[RFID] ENTRY reader v0x.. OK` (the wiring works). The LCD shows `CoCIS Entrance` / `Tap your card`, then the clock and `In:0   OPEN`.
4. Tap the Blue Card on ENTRY: `WELCOME Brian` / `Student BLUE card` for 3 s, then the idle screen with `In:1`.
5. Tap the Blue Card again: `DENIED Brian` / `Already inside`. Tap the Key Fob: `Card blocked`.
6. Open http://localhost:8180 to show the port forward works.

**(4) Likely questions.**

- *Why one ESP32, not three?* — The lecturer said free Wokwi supports one microcontroller. Each terminal is still separate: its own tasks, pins, data and mutex.
- *Why a queue for the LCD?* — I2C is slow. The entrance task must decide the next scan quickly, so it only drops the event in the queue and the `lcd` task does the slow writing.
- *What is 4-bit mode?* — The backpack has only 4 data lines to the LCD, so every byte is sent as two halves, each with an EN pulse.
- *Why a second SPI bus for the readers?* — The SD card uses SPI2. Putting the readers on SPI3 keeps the pins separate and lets the SD log and the readers work without waiting for each other.
- *Why `rm -f sdkconfig`?* — `sdkconfig.defaults` is only used when `sdkconfig` is created. Deleting the old one once makes the new partition and flash settings apply.
- *Are GPIO 2 and 15 safe?* — They are strapping pins. In Wokwi it is fine. On a real board we would move the LCD to other pins.

---

## 4. Katumba Joshua Mugwanya — Web sign-in security

**(1) Intro.** "I built storage, schedule, login and admin commands, and now the security of the web sign-in: one shared lockout, token sessions and logging."

**(2) Code tour.**

| Open | Say |
|---|---|
| `components/auth/auth.c` `hash_pin()` (~53), `hash_equal()` (~64) | "The PIN is never stored. We store a random salt and SHA-256 of salt + PIN, and compare in constant time." |
| `auth.c` `check_pin()` (~140) | "One function for both console and web. After 3 wrong PINs, login is locked for 30 s. The counter `s_failures` is shared, so you cannot get extra guesses by switching between console and web." |
| `auth.c` `auth_login()` (~176) and `auth_check_pin()` (~181) | "The console opens a console session. The web only checks the PIN; it keeps its own token. The log says `login` or `web login`." |
| `components/web_dashboard/web_dashboard.c` `h_login()` (~203) | "If locked, the web answers `Locked: try again in N s`. A wrong PIN gives `Wrong PIN`, or `Too many wrong PINs: locked for 30 s`. A right PIN gives a token." |
| `web_dashboard.c` `new_token()` (~88) and `token_ok()` (~102) | "The token is 16 random hex characters from `esp_random()`. Every API call must send it in the `X-Token` header. After 10 minutes idle the token is cleared and we log `WEB,session timeout`. Only one web session at a time." |
| `web_dashboard.c` `send_401()` (~134) | "No token or an old token gives 401. In `dashboard.html` the page sees 401 and goes back to the sign-in screen." |
| `web_dashboard.c` `h_hours()`, `h_emergency()`, `h_block()`, `h_scan()` | "Every web action is logged: `WEB,hours ...`, `WEB,emergency ON`, `WEB,block ...`, `WEB,test scan ...`." |

**(3) Live demo.**

1. On the web sign-in, type a wrong PIN: `Wrong PIN`.
2. In the console: `login 0000`: `wrong PIN`.
3. On the web, wrong again: `Too many wrong PINs: locked for 30 s`.
4. Console: `login 1234` at once: `login locked, try again in N s`. Explain: the web failures also locked the console.
5. Wait 30 s, sign in on the web. Change the opening hours in Admin.
6. `login 1234`, `syslog 15`: show `AUTH,web login failed`, `AUTH,login failed`, `AUTH,web login failed,locked for 30 s`, `AUTH,web login OK`, `WEB,hours ...`. Point out: no PIN in the log.
7. Open a second browser tab and sign in: the first tab goes back to sign-in (only one session).

**(4) Likely questions.**

- *Why share the lockout?* — With two counters, an attacker could try 2 PINs on the console and 2 on the web again and again. One counter means 3 tries in total.
- *Why a token and not send the PIN every time?* — The PIN is sent once. After that only a random token is sent, and it expires after 10 minutes idle or when someone signs in again.
- *Is it secure on the network?* — It is HTTP, not HTTPS, so on a real network the token could be read. A real system needs HTTPS. That is in our limitations.
- *Why salt the hash?* — The same PIN on two devices gives different hashes, and a ready-made table of PIN hashes does not work.

---

## 5. Nannyombi Shakiran (Kiran) — Wi-Fi, web dashboard and integration

**(1) Intro.** "I did the integration of the whole firmware, and the Wi-Fi with the web Security Dashboard: the server, the page and the live alerts."

**(2) Code tour.**

| Open | Say |
|---|---|
| `cocis_system/main/main.c` `app_main()` (~30) | "The start-up order matters: storage, schedule, auth, hub, admin commands, web commands, LCD, rooms, entrance, readers, Wi-Fi. The web and LCD listeners are added before `entrance_start()`, and the readers start after it, because they send scans to the entrance." |
| `components/access_control/access_control.c` `entrance_set_event_cb()` (~147) | "Before, there was one listener (the hub). I changed it to up to 4 listeners, so the hub, the web and the LCD all get every decision." |
| `components/web_dashboard/wifi.c` `on_wifi_event()` (~25), `wifi_net_start()` (~47) | "Station mode on `Wokwi-GUEST`. It does not wait: if Wi-Fi drops, it retries forever, and the building keeps working." |
| `web_dashboard.c` `on_entrance_event()` (~63) | "This runs in the entrance task, so it must not block. It only copies the event into a ring of 30 alerts, under the `s_lock` mutex." |
| `web_dashboard.c` `web_dashboard_start()` (~422) | "HTTP server on port 80 and 8 handlers: the page and the JSON API." |
| `web_dashboard.c` `h_state()` (~242) | "One JSON answer with time, hours, emergency, users with role and colour, rooms and alerts. It reads copies through `entrance_get_users()`, so it never holds the entrance lock for long." |
| `components/web_dashboard/dashboard.html` `refresh()` (~265) and `toast()` / `beep()` (~349, ~363) | "The page polls every 1.5 s. A new alert id means a new event: a toast and a beep. Denied is a red SECURITY ALERT with three tones." |
| `cocis_system/main/hub.c` `check_offline()` (~104) | "My hub still marks a room OFFLINE after 15 s with no report." |

**(3) Live demo.**

1. In the console type `wifi`: `Wi-Fi: connected, IP ..., web server running on port 80`.
2. Open http://localhost:8180 and sign in.
3. ENTRY reader, Tap the Blue Card (Brian): toast "Entry", blue chip in **Who is inside**, Brian leaves **Who is out**.
4. Tap the NFC Tag: red **SECURITY ALERT: DENIED - unknown card** with the alarm sound.
5. Admin: Emergency ON; show the badge and `status` in the console. Emergency off.
6. Admin: Test scan `C3D4E5F6`, Scan IN: Carol appears with a blue chip.
7. `login 1234`, `roomfail 2 on`: after 15 s `[HUB] room 2 OFFLINE (no report for 15 s)` and the dashboard shows it. `roomfail 2 off`.

**(4) Likely questions.**

- *How do the hub, the web and the LCD all get the scans?* — `entrance_set_event_cb()` keeps a list of up to 4 listeners and calls each one after every decision.
- *How is the alert ring protected?* — The entrance task writes and the HTTP task reads, so both take the `s_lock` mutex. The writer only copies, so it is very quick.
- *What if Wi-Fi is down?* — `web_dashboard_start()` does not wait for Wi-Fi. The readers, rooms, console and SD log keep working. Wi-Fi retries in the background.
- *Why polling and not WebSocket?* — Polling every 1.5 s is simple and works with the ESP-IDF HTTP server. Each alert has an id, so the page never misses one (up to 30 kept).
