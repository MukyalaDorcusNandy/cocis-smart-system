# Demo Guide

A step-by-step demonstration of the whole system, about 10 minutes. Use it for the presentation and for the videos: each member can run the part of the demo that matches their work (see [VIDEO_GUIDE.md](VIDEO_GUIDE.md)).

Do the steps **in order**: each part uses the state the earlier parts left (who is inside, the clock, the login).

## Before you start

1. Open an ESP-IDF terminal (VS Code: F1 → **ESP-IDF: Open ESP-IDF Terminal**; Linux: `. ~/.espressif/tools/activate_idf_v6.0.1.sh`).
2. Build:
   ```sh
   cd cocis_system
   rm -f sdkconfig
   idf.py build
   ```
   `rm -f sdkconfig` is only needed the first time.
3. In VS Code press F1 → **Wokwi: Start Simulator**. Turn the computer sound on.
4. Arrange the screen: the Wokwi circuit on one side, the Wokwi serial terminal under it, and the browser (dashboard) on the other side.

**How to tap a card.** Click a reader (the blue MFRC522 board: the upper one is ENTRY, the lower one is EXIT). A small panel opens. Pick the card first (Blue, Green, Yellow, Red, NFC Tag or Key Fob), **then** press **Tap**. The card stays on the reader for about half a second, and the serial terminal prints `[RFID ENTRY] card ...`.

**If a tap does nothing:** type `rfid` in the terminal. It shows whether each reader was found and how many cards it has read. If a reader shows `NOT FOUND` or the count stays 0, use the typed backup for the rest of the demo: `in <UID>` for the ENTRY reader and `out <UID>` for the EXIT reader. The result is exactly the same, because both go through `entrance_submit_scan()`.

| Wokwi card | UID (typed backup) | Person | Role and colour |
|---|---|---|---|
| Blue Card | `01020304` | Brian | Student, blue |
| Green Card | `11223344` | Alice | Staff, green |
| Yellow Card | `55667788` | Grace | Guest, yellow |
| Red Card | `AABBCCDD` | Dan | Admin, red |
| Key Fob (orange) | `C0FFEE99` | Lost card | blocked |
| NFC Tag (grey) | `04112233` | not registered | unknown |

## Part 1. Start-up (1 minute)

**Say:** "One ESP32 runs the whole system: the main entrance and lecture rooms 1 and 2. Each part is a FreeRTOS task, and they talk through queues."

**Show the boot lines** in the terminal:

| Line | What it proves |
|---|---|
| `[STORAGE] log mode: SD card (/sdcard/ACCESS.LOG) ...` | The SD card log works |
| `[SCHEDULE] open 08:00, close 18:00` | Opening hours are loaded |
| `[HUB] room 1 ONLINE`, `[HUB] room 2 ONLINE` | Both rooms report to the hub |
| `[LCD] 16x2 LCD ready at 0x27 (SDA 15, SCL 2)` | The entrance LCD works; it shows `CoCIS Entrance` / `Tap your card` |
| `[RFID] ENTRY reader v0x.. OK`, `[RFID] EXIT reader v0x.. OK` | Both card readers answered |
| `[WIFI] connected, IP 10.x.x.x` | Wi-Fi is up, the dashboard can be opened |

Type `status`: `Time 08:0x:xx | open 08:00 - 18:00 (OPEN) | emergency off | inside 0`.

## Part 2. Sign in to the dashboard (1 minute)

1. In the browser open **http://localhost:8180**. If it does not load yet, wait for the `[WIFI] connected` line and reload.
2. **Say:** "Only signed-in staff can see who is inside. It uses the same admin PIN and the same 3-tries lockout as the serial console."
3. Type a wrong PIN, for example `0000`: the page shows `Wrong PIN`.
4. Type `1234` and **Sign in**. The dashboard opens: clock, OPEN badge, people inside = 0, the two lecture rooms, Who is inside (empty), Live alerts, Who is out (every card), Admin.

Keep the dashboard visible for the rest of the demo: every scan appears there within 2 seconds.

## Part 3. People arrive (2 minutes)

| Step | Do | You should see |
|---|---|---|
| 3.1 | ENTRY reader: pick **Blue Card**, press **Tap** | Terminal: `[RFID ENTRY] card 01020304`, then `>> ENTRY 01020304 (Brian (Student), blue card) -> GRANTED [ok] \| inside now: 1`. Circuit: green LED, entry door servo opens for 3 s, one short beep. LCD: `WELCOME Brian` / `Student BLUE card`. Dashboard: toast "Entry", a **blue** chip for Brian in Who is inside |
| 3.2 | ENTRY reader: **Green Card**, Tap | Alice (Staff) enters: **green** chip, LCD `Staff GREEN card` |
| 3.3 | ENTRY reader: **Yellow Card**, Tap | Grace (Guest) enters: **yellow** chip |
| 3.4 | Type `inside` | `People inside: 3`, one line each with role, colour and the time they came in |

**Say:** "The card colour is the role: blue student, green staff, yellow guest, red admin. Security can tell at a glance who is in the building."

## Part 4. Access denied: security alerts (2 minutes)

| Step | Do | You should see |
|---|---|---|
| 4.1 | ENTRY reader: **Blue Card** again (Brian is already inside) | `-> DENIED [already inside (anti-passback)]`. Red LED, two long beeps. LCD `DENIED Brian` / `Already inside`. Dashboard: red **SECURITY ALERT** toast with an alarm sound |
| 4.2 | ENTRY reader: **NFC Tag** | `>> ENTRY 04112233 (-, no card) -> DENIED [unknown tag]`. LCD `Unknown card`. Red alert "unknown card" |
| 4.3 | ENTRY reader: **Key Fob** (a lost card) | `-> DENIED [tag blocked]`. LCD `Card blocked`. Red alert "blocked card" |
| 4.4 | EXIT reader: **Red Card** (Dan never came in) | `>> EXIT AABBCCDD (Dan (Admin), red card) -> DENIED [not inside]` |

**Say:** "Every decision, granted or denied, is written to the SD card and shown live on the dashboard. The red alerts are the ones security must look at."

## Part 5. Lecture rooms and lights (1.5 minutes)

| Step | Do | You should see |
|---|---|---|
| 5.1 | Click the **PIR sensor of room 1** (top left) | `[ROOM 1] OCCUPIED, light ON` and `[HUB] room 1 OCCUPIED, light ON`. The room 1 yellow LED is on. Dashboard: room 1 bulb lit |
| 5.2 | Wait. About 10 s after the PIR stops sensing motion | `[ROOM 1] EMPTY, light OFF`: the light switches itself off in an empty room (saves electricity) |
| 5.3 | Type `rooms` | Each room: ONLINE, occupied or empty, light on or off, and how many seconds the light has been on |
| 5.4 | (Optional) `login 1234`, `roomfail 2 on`, wait 15 s | `[HUB] room 2 OFFLINE (no report for 15 s)`; the dashboard shows room 2 OFFLINE. `roomfail 2 off` brings it back |

**Say:** "A PIR sees movement, so the light stays on for a while after the last movement. In real use that delay would be about 5 minutes; 10 seconds is for the demo."

## Part 6. Opening hours and the emergency exit (2 minutes)

| Step | Do | You should see |
|---|---|---|
| 6.1 | Type `hours 07:00 20:00` without logging in | `login required: login <pin>`: settings are protected |
| 6.2 | `login 1234` | `logged in as admin ...` and a warning to change the default PIN |
| 6.3 | `clock 21:00` | `clock 21:00:00`, `... building is CLOSED`. LCD idle screen shows `CLOSED`, dashboard badge shows CLOSED |
| 6.4 | EXIT reader: **Green Card** (Alice is inside) | `-> DENIED [outside access hours]`. LCD `Building closed`. **Say:** "The brief says people must not leave outside the permitted hours." |
| 6.5 | Dashboard → Admin → **Emergency ON** (or type `emergency on`) | The emergency badge blinks red |
| 6.6 | EXIT reader: **Green Card** again | `-> GRANTED [emergency exit]`, LCD `BYE Alice`. **Say:** "Only an admin can open the emergency exit, and it is logged." |
| 6.7 | **Emergency off**, then `clock 09:00` | Building OPEN again |

## Part 7. Admin from the dashboard (1 minute)

| Step | Do | You should see |
|---|---|---|
| 7.1 | Admin → Cards table → **Block** next to Brian | `users` shows Brian `[BLOCKED]` |
| 7.2 | EXIT reader: **Blue Card** | `-> DENIED [tag blocked]`, red alert |
| 7.3 | **Unblock** Brian, then EXIT reader: **Blue Card** | `-> GRANTED`, Brian moves to Who is out |
| 7.4 | Admin → opening hours `07:00` to `20:00` → Save | Toast "Saved"; `hours` in the terminal shows `07:00 - 20:00` |

## Part 8. The log on the SD card (30 seconds)

Type `syslog 10` (you are still logged in). It prints the last 10 events from RAM and then the same lines read back from `/sdcard/ACCESS.LOG`, for example:

```text
12,09:01:10,ACCESS,01020304,Brian (Student),EXIT,GRANTED,ok
13,09:01:30,WEB,hours 07:00-20:00
```

**Say:** "Every event has a sequence number and a time, so management can see who came in or out and when. The PIN never appears in the log."

## Closing (30 seconds)

**Say:** "To sum up: two MFRC522 readers at the entrance, colour-coded cards, an LCD, doors, LEDs and a buzzer; two lecture rooms with automatic lights; an SD card log; opening hours that only a signed-in admin can change; and a Wi-Fi dashboard with live security alerts, all on one ESP32 with FreeRTOS tasks, queues and mutexes."

## Quick fixes during the demo

| Problem | Fix |
|---|---|
| A tap does nothing | Pick the card first, then press Tap. Type `rfid` to check the readers. Use `in <UID>` / `out <UID>` as backup |
| The same card is not read twice | The reader reports a card once while it is on the reader. Tap again (Tap removes it after 0.5 s) |
| Dashboard does not load | Wait for `[WIFI] connected`, then reload http://localhost:8180 |
| Dashboard went back to sign-in | Someone signed in on another tab (one session only). Sign in again |
| `login locked, try again in N s` | 3 wrong PINs: wait 30 s |
| Everything is CLOSED | The clock was moved: `login 1234`, then `clock 09:00` |
