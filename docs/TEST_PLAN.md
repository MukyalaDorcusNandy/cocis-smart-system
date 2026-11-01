# Test Plan

Run in Wokwi (`cocis_system`, F1 → **Wokwi: Start Simulator**) and type the commands in the serial monitor. The expected lines are copied from the `printf` strings in the code. Mark each test Pass or Fail.

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

## 1. Entrance: scans

| # | Steps | Expected |
|---|---|---|
| 1.1 | `in A1B2C3D4` | `>> ENTRY A1B2C3D4 (Alice (Staff)) -> GRANTED [ok] \| inside now: 1`, `[HUB] building occupancy: 1`, green LED on and entry servo opens for 3 s |
| 1.2 | `in A1B2C3D4` again | `-> DENIED [already inside (anti-passback)]`, red LED and buzzer |
| 1.3 | `in 12345678` | `>> ENTRY 12345678 (-) -> DENIED [unknown tag]` |
| 1.4 | `in DEADBEEF` | `-> DENIED [tag blocked]` |
| 1.5 | `out B2C3D4E5` (Brian is outside) | `-> DENIED [not inside]` |
| 1.6 | `inside` | `People inside: 1` and Alice's line |
| 1.7 | `out A1B2C3D4` | `>> EXIT A1B2C3D4 (Alice (Staff)) -> GRANTED [ok] \| inside now: 0`, exit servo opens |
| 1.8 | `log` | CSV lines for 1.1–1.7 with the time from the shared clock |

## 2. Opening hours (coursework §2.6)

| # | Steps | Expected |
|---|---|---|
| 2.1 | `in C3D4E5F6` (granted), then `login 1234`, `clock 19:00` | `clock 19:00:00`, `opening hours 08:00 - 18:00, building is CLOSED` |
| 2.2 | `out C3D4E5F6` | `>> EXIT C3D4E5F6 (Carol (Student)) -> DENIED [outside access hours]`: a person inside cannot leave outside hours |
| 2.3 | `in B2C3D4E5` | `-> DENIED [outside access hours]` |
| 2.4 | `emergency on` | `emergency mode ON: exit allowed outside opening hours`; `status` shows `emergency ON` |
| 2.5 | `out C3D4E5F6` | `-> GRANTED [emergency exit]` |
| 2.6 | `emergency off`, `syslog 10` | The log shows `EMERGENCY,on`, `ACCESS,C3D4E5F6,...,GRANTED,emergency exit` and `EMERGENCY,off` |
| 2.7 | `hours 07:00 20:00` | `opening hours 07:00 - 20:00, building is OPEN`; `in B2C3D4E5` is now GRANTED |
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
| 5.11 | `block A1B2C3D4`, `in A1B2C3D4` | `card A1B2C3D4 BLOCKED`, then `-> DENIED [tag blocked]`; `unblock A1B2C3D4` undoes it |

## 6. SD card log

| # | Steps | Expected |
|---|---|---|
| 6.1 | After some tests: `login <pin>`, `syslog 5` | `-- last 5 log lines (RAM) --` and five lines, then `-- last 5 of N lines in /sdcard/ACCESS.LOG --` and the same five lines |
| 6.2 | Check the line format | `seq,HH:MM:SS,EVENT,...`, for example `12,08:03:10,ACCESS,A1B2C3D4,Alice (Staff),ENTRY,GRANTED,ok` |

## Notes

- **Saved settings after reboot (2.9):** flash (NVS) keeps its content only while the simulation keeps running. Restarting the Wokwi simulator usually starts with empty flash and an empty SD card, so test persistence with a reset inside the same simulation if one is available.
- **PIR timing:** the Wokwi PIR keeps its output high for a few seconds after a click. The 10 s vacancy delay starts when the output goes low.
