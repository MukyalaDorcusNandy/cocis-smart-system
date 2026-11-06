# Test Plan

Run in Wokwi (`cocis_system`, F1 → **Wokwi: Start Simulator**) and type the commands in the serial monitor. To tap a card, click a reader, pick a card (Blue = Brian, Green = Alice, Yellow = Grace, Red = Dan, Key Fob = lost card, NFC Tag = unknown) and press **Tap**. Carol (`C3D4E5F6`) has no Wokwi card and is used with typed scans. The expected lines are copied from the `printf` strings in the code. Mark each test Pass or Fail.

The clock starts at **08:00** and the default opening hours are **08:00–18:00**, so the building is open at boot.

## 0. Boot

| # | Steps | Expected serial output |
|---|---|---|
| 0.1 | Start the simulator | `[STORAGE] settings in NVS: OK` |
| 0.2 | (same boot) | `[STORAGE] log mode: SD card (/sdcard/ACCESS.LOG) + RAM ring of 20 lines`. If the card fails: `[STORAGE] log mode: RAM ONLY ...` |
| 0.3 | (same boot) | `[SCHEDULE] open 08:00, close 18:00` |
| 0.4 | First boot only | `[AUTH] first boot: default admin PIN created, change it with setpin` |
| 0.5 | (same boot) | `[HUB] room 1 ONLINE`, `[HUB] room 1 EMPTY, light OFF`, and the same for room 2 |
| 0.6 | (same boot) | `=== CoCIS ENTRANCE (access_control) ready ===`, the command list, then `Time 08:00:0x \| open 08:00 - 18:00 (OPEN) \| emergency off \| inside 0` |
| 0.7 | (same boot) | `[RFID] ENTRY reader v0x.. OK` and `[RFID] EXIT reader v0x.. OK` (any version except `0x00` / `0xFF`). If a reader is missing: `[RFID] ENTRY reader NOT FOUND (got 0x..), use typed in/out` |
| 0.7b | (same boot) | `[LCD] 16x2 LCD ready at 0x27 (SDA 15, SCL 2)`; the LCD shows `CoCIS Entrance` / `Tap your card`, then the idle screen. Without an LCD: `[LCD] no LCD at 0x27, running without it` |
| 0.8 | (same boot) | `[WIFI] connecting to Wokwi-GUEST ...`, `[WEB] dashboard on port 80`, then after a few seconds `[WIFI] connected, IP 10.x.x.x` and `[WEB] open http://localhost:8180 (Wokwi port forward)` |

## 1. Entrance: scans

| # | Steps | Expected |
|---|---|---|
| 1.1 | `in 11223344` | `>> ENTRY 11223344 (Alice (Staff), green card) -> GRANTED [ok] \| inside now: 1`, `[HUB] building occupancy: 1`, green LED on, one short beep, LCD `WELCOME Alice`, entry servo opens for 3 s |
| 1.2 | `in 11223344` again | `-> DENIED [already inside (anti-passback)]`, red LED and two long beeps |
| 1.3 | `in 88041122` | `>> ENTRY 88041122 (-, no card) -> DENIED [unknown tag]` |
| 1.4 | `in C0FFEE99` | `-> DENIED [tag blocked]` |
| 1.5 | `out 01020304` (Brian is outside) | `-> DENIED [not inside]` |
| 1.6 | `inside` | `People inside: 1` and Alice's line with `Staff   (green card) since 08:0x:xx` |
| 1.7 | `out 11223344` | `>> EXIT 11223344 (Alice (Staff), green card) -> GRANTED [ok] \| inside now: 0`, exit servo opens |
| 1.8 | `log` | CSV lines for 1.1–1.7 with the time from the shared clock |

## 2. Opening hours (coursework §2.6)

| # | Steps | Expected |
|---|---|---|
| 2.1 | `in C3D4E5F6` (granted), then `login 1234`, `clock 19:00` | `clock 19:00:00`, `opening hours 08:00 - 18:00, building is CLOSED` |
| 2.2 | `out C3D4E5F6` | `>> EXIT C3D4E5F6 (Carol (Student), blue card) -> DENIED [outside access hours]`: a person inside cannot leave outside hours |
| 2.3 | `in 01020304` | `-> DENIED [outside access hours]` |
| 2.4 | `emergency on` | `emergency mode ON: exit allowed outside opening hours`; `status` shows `emergency ON` |
| 2.5 | `out C3D4E5F6` | `-> GRANTED [emergency exit]` |
| 2.6 | `emergency off`, `syslog 10` | The log shows `EMERGENCY,on`, `ACCESS,C3D4E5F6,...,GRANTED,emergency exit` and `EMERGENCY,off` |
| 2.7 | `hours 07:00 20:00` | `opening hours 07:00 - 20:00, building is OPEN`; `in 01020304` is now GRANTED |
| 2.8 | `hours 18:00 08:00` | `opening time must be before closing time` |
| 2.9 | Reboot without restarting the simulator (if the board offers a reset), then `hours` | `opening hours 07:00 - 20:00`: the setting was saved in NVS. See note below. |

## 3. Rooms (test room 1, then repeat for room 2)

| # | Steps | Expected |
|---|---|---|
| 3.1 | Click PIR 1 (motion) | `[ROOM 1] OCCUPIED, light ON`, `[HUB] room 1 OCCUPIED, light ON`, room 1 LED on |
| 3.2 | Wait until the PIR output drops, then 10 s more | `[ROOM 1] EMPTY, light OFF`, `[HUB] room 1 EMPTY, light OFF`, LED off |
| 3.3 | `rooms` | `Room 1: ONLINE  empty    light off light-on N s, last report M s ago` with N > 0; `Building: X people inside` |
| 3.4 | Repeat 3.1–3.3 with PIR 2 | Same with `ROOM 2` / `room 2`, room 2 LED |

## 4. Offline detection

| # | Steps | Expected |
|---|---|---|
| 4.1 | `roomfail 2 on` without login | `login required: login <pin>` |
| 4.2 | `login <pin>`, `roomfail 2 on` | `room 2 stopped reporting (OFFLINE after 15 s)` |
| 4.3 | Wait up to 15 s | `[HUB] room 2 OFFLINE (no report for 15 s)`; `rooms` shows `Room 2: OFFLINE`; clicking PIR 2 still switches its LED |
| 4.4 | `roomfail 2 off` | Within 5 s: `[HUB] room 2 ONLINE` |

## 5. Login and security

| # | Steps | Expected |
|---|---|---|
| 5.1 | Type `login 1234` | The screen shows `login ****`; then `logged in as admin (...)` and `WARNING: default PIN in use, run: setpin <new 4-8 digits>` |
| 5.2 | `setpin 12` | `PIN must be 4-8 digits` |
| 5.3 | `setpin 5678` | Screen shows `setpin ****`, then `PIN changed` |
| 5.4 | `logout`, `hours 09:00 17:00` | `logged out`, then `login required: login <pin>` |
| 5.5 | `login 0000` twice | `wrong PIN` twice |
| 5.6 | `login 0000` a third time | `login locked, try again in 30 s` |
| 5.7 | `login 5678` at once | `login locked, try again in N s` (N < 30) |
| 5.8 | Wait 30 s, `login 5678` | `logged in as admin ...`, no default-PIN warning |
| 5.9 | Wait 2 minutes, `syslog` | `session ended after 120 s without activity`, then `login required: login <pin>` |
| 5.10 | `login 5678`, `syslog 20` | Contains `AUTH,login failed`, `AUTH,login failed,locked for 30 s`, `AUTH,login refused (locked)`, `AUTH,login OK`, `AUTH,PIN changed`, `AUTH,session timeout`. **No PIN appears anywhere.** |
| 5.11 | `block 11223344`, `in 11223344` | `card 11223344 BLOCKED`, then `-> DENIED [tag blocked]`; `unblock 11223344` undoes it |

## 6. SD card log

| # | Steps | Expected |
|---|---|---|
| 6.1 | After some tests: `login <pin>`, `syslog 5` | `-- last 5 log lines (RAM) --` and five lines, then `-- last 5 of N lines in /sdcard/ACCESS.LOG --` and the same five lines |
| 6.2 | Check the line format | `seq,HH:MM:SS,EVENT,...`, for example `12,08:03:10,ACCESS,11223344,Alice (Staff),ENTRY,GRANTED,ok` |

## 7. RFID readers (Wokwi MFRC522)

Typed `in`/`out` is not used here. To tap a card: click a reader, pick a card and press **Tap** (the card stays about 0.5 s). Keyboard, after clicking the card panel: `b` / `g` / `y` / `r` / `n` / `k` picks Blue / Green / Yellow / Red / NFC Tag / Key Fob, `t` taps. **Hold** keeps the card on the reader.

| # | Steps | Expected |
|---|---|---|
| 7.1 | ENTRY reader: Tap the Green Card | `[RFID ENTRY] card 11223344`, then `>> ENTRY 11223344 (Alice (Staff), green card) -> GRANTED [ok] \| inside now: 1`; LCD `WELCOME Alice` / `Staff GREEN card`, entry servo opens, one short beep |
| 7.2 | ENTRY reader: Hold the Green Card for 5 s | Only one `[RFID ENTRY] card 11223344` line (the card is reported once while it stays on the reader) |
| 7.3 | Take the card away, then Tap the Green Card on ENTRY again | `[RFID ENTRY] card 11223344` again, then `-> DENIED [already inside (anti-passback)]`; LCD `DENIED Alice` / `Already inside`, red LED, two long beeps |
| 7.4 | EXIT reader: Tap the Green Card | `[RFID EXIT] card 11223344`, `>> EXIT 11223344 (Alice (Staff), green card) -> GRANTED [ok] \| inside now: 0`; LCD `BYE Alice`, exit servo opens |
| 7.5 | ENTRY reader: Tap the NFC Tag | `[RFID ENTRY] card 88041122`, `>> ENTRY 88041122 (-, no card) -> DENIED [unknown tag]`; LCD `DENIED` / `Unknown card` |
| 7.6 | ENTRY reader: Tap the Key Fob | `[RFID ENTRY] card C0FFEE99`, `-> DENIED [tag blocked]`; LCD `DENIED Lost` / `Card blocked` |
| 7.7 | Both readers at the same time: Hold the Blue Card on ENTRY and the Yellow Card on EXIT | Both `[RFID ENTRY] card 01020304` and `[RFID EXIT] card 55667788` lines appear; the shared SPI bus gives no mixed or wrong UIDs |
| 7.8 | Typed backup: `in 55667788` | Works the same as a reader: `>> ENTRY 55667788 (Grace (Guest), yellow card) -> GRANTED [ok] ...` |

## 8. Card roles, colours and the LCD

Tap each card on the ENTRY reader. The colour of the Wokwi card is the role colour.

| # | Steps | Expected |
|---|---|---|
| 8.1 | Blue Card (Brian) | `(Brian (Student), blue card) -> GRANTED`; LCD `WELCOME Brian` / `Student BLUE card` |
| 8.2 | Green Card (Alice) | `(Alice (Staff), green card)`; LCD `Staff GREEN card` |
| 8.3 | Yellow Card (Grace) | `(Grace (Guest), yellow card)`; LCD `Guest YELLOW card` |
| 8.4 | Red Card (Dan) | `(Dan (Admin), red card)`; LCD `Admin RED card` |
| 8.5 | A denied scan (NFC Tag) | `(-, no card) -> DENIED`; LCD `DENIED` / `Unknown card`; red LED and two long beeps |
| 8.6 | Wait 3 s after a scan | LCD goes back to the idle screen: `CoCIS  HH:MM:SS` / `In:N   OPEN` (N = people inside) |
| 8.7 | `login <pin>`, `clock 21:00`, wait 2 s | Idle screen shows `CLOSED`; a tap shows `DENIED ...` / `Building closed`. Set `clock 09:00` again |
| 8.8 | `inside` | One line per person: UID, name, role, `(<colour> card) since HH:MM:SS` |
| 8.9 | `users` | Every card with role and colour, `[inside]` / `[outside]`, and `[BLOCKED]` for `C0FFEE99` |

## 9. Wi-Fi and web dashboard

| # | Steps | Expected |
|---|---|---|
| 9.1 | After boot, type `wifi` | `Wi-Fi: connected, IP 10.x.x.x, web server running on port 80` and `Dashboard: open http://localhost:8180 (Wokwi port forward)` |
| 9.2 | Open http://localhost:8180 in the browser | Sign-in page: "CoCIS Smart Building", "Security Dashboard — admin sign-in" |
| 9.3 | Sign in with the PIN (`1234` on first boot) | The dashboard opens: clock, hours, OPEN badge, emergency badge, People inside, Lecture rooms, Who is inside, Live alerts, Who is out, Admin |
| 9.4 | Tap the Blue Card on ENTRY | Within 2 s: a toast "Entry" with Brian (Student), a blue chip in **Who is inside** with the since-time, Brian leaves **Who is out**, count goes up |
| 9.5 | Click PIR 1 | Room 1 card shows occupied and light on; later light-on time grows |
| 9.6 | `login <pin>`, `roomfail 2 on`, wait 15 s | Room 2 card shows OFFLINE; `roomfail 2 off` brings it back online |
| 9.7 | Admin: set hours 07:00 to 20:00, Save | Toast "Saved — Opening hours updated"; console `hours` shows `07:00 - 20:00`; `syslog` has `WEB,hours 07:00-20:00` |
| 9.8 | Admin: Emergency ON, then Emergency off | Emergency badge changes; `status` shows it; `syslog` has `WEB,emergency ON` and `WEB,emergency off` |
| 9.9 | Admin: block `11223344` in the Cards table, then unblock | `users` shows `[BLOCKED]`, then not; `syslog` has `WEB,block 11223344` and `WEB,unblock 11223344` |
| 9.10 | Admin: Test scan `C3D4E5F6`, Scan IN | Console prints `>> ENTRY C3D4E5F6 (Carol (Student), blue card) -> ...`; `syslog` has `WEB,test scan in C3D4E5F6` |
| 9.11 | Sign out | Back to sign-in; `syslog` has `WEB,logout` |

## 10. Live security alerts

Signed in on the dashboard. Turn on the computer sound.

| # | Steps | Expected |
|---|---|---|
| 10.1 | Tap the NFC Tag on ENTRY (unknown) | Red toast **SECURITY ALERT: DENIED - unknown card**, three-tone alarm beep, red line in Live alerts |
| 10.2 | Tap the Key Fob on ENTRY (blocked) | Red toast **SECURITY ALERT: DENIED - blocked card** |
| 10.3 | Tap a card of someone already inside | Red toast **SECURITY ALERT: DENIED entry** (anti-passback) |
| 10.4 | `clock 19:00` (logged in), tap any card | Red toast **SECURITY ALERT: DENIED - after hours** |
| 10.5 | A granted scan | Normal toast "Entry" or "Exit" in the card colour, one short beep |
| 10.6 | Reload the page (F5) | Still signed in (the token is kept for the tab). Old alerts show in the feed, but no toasts for them: only new alerts pop up |

## 11. Sign-in lockout shared by console and web

| # | Steps | Expected |
|---|---|---|
| 11.1 | Web: sign in with a wrong PIN | Message `Wrong PIN` |
| 11.2 | Console: `login 0000` | `wrong PIN` (this is the 2nd failure in the same counter) |
| 11.3 | Web: wrong PIN again (3rd failure) | `Too many wrong PINs: locked for 30 s` |
| 11.4 | Console: `login <right pin>` at once | `login locked, try again in N s`: the web failures also lock the console |
| 11.5 | Web: right PIN at once | `Locked: try again in N s` |
| 11.6 | Wait 30 s, web: right PIN | Dashboard opens |
| 11.7 | `syslog 20` (after console login) | `AUTH,web login failed`, `AUTH,login failed`, `AUTH,web login failed,locked for 30 s`, `AUTH,login refused (locked)`, `AUTH,web login OK`. No PIN appears anywhere |
| 11.8 | Sign in on a second browser tab | The first tab goes back to sign-in on its next refresh ("Session ended, please sign in again"): only one web session (401) |
| 11.9 | (Optional, slow) Close the dashboard tab, wait more than 10 minutes, then reopen it (Ctrl+Shift+T) | 401 → sign-in screen "Session ended, please sign in again"; `syslog` has `WEB,session timeout`. An open tab never times out, because it polls every 1.5 s |

## Notes

- **Saved settings after reboot (2.9):** flash (NVS) keeps its content only while the simulation keeps running. Restarting the Wokwi simulator usually starts with empty flash and an empty SD card, so test persistence with a reset inside the same simulation if one is available.
- **PIR timing:** the Wokwi PIR keeps its output high for a few seconds after a click. The 10 s vacancy delay starts when the output goes low.
- **Dashboard:** the browser asks for `/api/state` every 1.5 s, so a change can take up to 2 s to appear. If the page does not open, check `wifi` first; Wi-Fi can take a few seconds after boot.
- **Tapping cards:** a reader reports a card once. **Tap** puts the card on the reader for about 0.5 s, so the next Tap is a new scan. A card on **Hold** is reported only once until it is taken away.
- **LCD:** a scan result stays on the LCD for 3 s, then the idle screen comes back. If two scans come quickly, the second is shown after the first.
