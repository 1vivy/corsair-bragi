# HID probe: what the hub does, measured

Measured on one hub (`1b1c:0c3f`, firmware 3.10.636) on 2026-11-19 with the scripts in [`../tools/`](../tools/), run as root
with nothing else on the hub's hidraw node. Commands and framing: [`hid-protocol.md`](hid-protocol.md). The
questions: how a kernel driver and other programs can use the hub at once, and what the hub does for a fan on its
own. The last section lists what the `corsair-bragi` driver takes from the answers.

## Replies

| Fact | Evidence |
|---|---|
| Every command gets one 512-byte reply. Byte 2 echoes the command byte, byte 3 is the status | all runs |
| Replies come back in request order | 20 of 20 pairs written back to back (`08 01` read, then `06 00` colour) were answered `08`, then `06`; none lost |
| Bytes after the status are valid only for a successful read; other replies carry bytes left from an earlier reply | a colour write answered `06 00 25 00 …` (the speeds data type) |
| Status `03` does not prove hardware mode | in hardware mode: open `03`, read `03`, close `00`. In software mode a read on a closed endpoint is `03` too |
| An invalid command tells the mode without side effects | `01 08 01` and `01 7f 01` answer `03` in hardware mode and `05` in software mode |
| One exchange takes about 2 ms; a colour frame for 138 LEDs about 6 ms | 120 exchanges: 1.95 / 2.04 / 6.01 ms (min / avg / max); 30 colour writes: 1.97 / 5.90 / 7.99 ms |

## Handles

Measured with [`hid-probe.py`](../tools/hid-probe.py) and [`handles-probe.py`](../tools/handles-probe.py).

| Fact | Evidence |
|---|---|
| A colour frame between the open and the read of a speeds read does not disturb it | 40 of 40 reads returned the correct speeds (colours on handle 0, speeds on handle 1) |
| Every handle number works | speeds opened, read and closed with status `00` on each of the 256 numbers. The open implementations use 0, 1 and 2 |
| The same resource can be open on two handles at once | speeds open on handles 1 and 2 (and on 0 and 1): both read, a second read on the first still worked, and closing one left the other readable |
| Two resources on two handles do not mix | speeds on 1 and temperatures on 2, read in the other order: each returned its own data type |
| A close takes a count and a list, of at most 6 or 7 handles | `05 02 01 02` closed handles 1 and 2 (reads on both then `03`); lists of 2 to 6 closed every handle; lists of 8 and 12 answered `01` and closed none (7 not tested) |
| A read, write or continue on a closed handle answers `03` | handle 1 closed: `06 01`, `08 01`, `07 01` each `03` |
| Other handles can be used between the reports of a long write | 600 colour bytes in two reports (`06 00`, `07 00`), with speeds opened, read and closed on handle 2 in between: every status `00` |
| A reply starts `00 00`, then the echo and the status | `00 00 01 00 …` for the software-mode command |
| Interface 1 takes no commands | a command written to its hidraw node fails with `EPIPE`; the mode did not change |

## Header bytes

Measured with [`header-probe.py`](../tools/header-probe.py) through the client node of `corsair-bragi` 0.2.0, which passed
these reports on unchanged. The driver now sets the reply bit on every command, so to repeat the rows about a
clear bit, unload it and use the hub's own node. A command is `00 <address> <flags>` and then the opcode; the open
implementations send `00 00 01` (OpenLinkHub also `00 19 01` for a LINK PSU and `00 <channel> 01` for a device's
model).

| Fact | Evidence |
|---|---|
| Bit 0 of the third byte asks for a reply; the other bits change nothing seen | get firmware and an open with `01`, `03`, `0d`, `81`, `ff`: answered alike, in 2 ms |
| With bit 0 clear the command runs and no reply comes | an open of handle `30` on speeds sent with `00`, `02` and `04`: no reply in 500 ms; a read of handle `30` sent with `01` then answered `00` with the speeds data type |
| The second byte is an address: `00` is the hub, `N` is the LINK device on channel N | get firmware at 1 and 3 (LX120): `1.3.66`; at 2, 13 and 14 (QX140): `1.0.91`; at 4, 5, 12, 15 (empty): status `07` |
| A reply repeats the address in its first byte, with bit 7 dropped | address `01` → `01 00 02 00 …`; address `ff` → `7f 00 02 07 …` |
| A LINK device does not have the hub's mode property | get `03` at every address but 0: status `01` |

Not tested: any set, open or write at a device address.

## Software mode

| Fact | Evidence |
|---|---|
| A fan with no speed set follows its stored curve (see "Stored curves run in software mode"); here that is 20 %, about 405 RPM, while the curve's sensor is below 44 °C | first read after 35 s in hardware mode: 404 / 402 / 404 / 403 / 402 RPM; unchanged over the next 30 s with no duty set |
| 40 % is about 907 RPM on the LX120 and 806 RPM on the QX140 | steady after about 20 s |
| Repeating the software-mode command changes nothing | speeds unchanged 8 s after a second `01 03 00 02` at 40 % |
| The hub returns to hardware mode after 20–30 s without traffic | still software mode after 4, 6, 8, 10, 12, 15 and 20 s of silence; hardware mode after 30 s |
| Fan reads alone keep software mode | one speeds read every 7.5 s for 75 s |
| Colour writes alone keep software mode | one colour frame every 7.5 s for 75 s |
| Nothing can be read in hardware mode | see "Hardware mode" below |
| Interface 1 sends nothing unprompted | no report in 75 s |

Not tested: whether a firmware query alone keeps software mode (that run was cut off); the timeout between 20 and
30 s; other firmware versions. The ~10 s figure from the OpenLinkHub author
([liquidctl #693](https://github.com/liquidctl/liquidctl/issues/693)) does not match this firmware.

## Hardware-mode fan tables

Read on 2026-11-19 with [`hw-tables-probe.py`](../tools/hw-tables-probe.py) (close, open, read, close on handle 1; nothing
written), in software mode, beside the `corsair-bragi` driver. Two runs gave the same bytes. Resource ids are two
bytes; the tables use the reply data types that liquidctl's Commander Core driver uses for its `60 6d`–`62 6d`, at
other ids.

| Resource | Reply type | Content on this hub |
|---|---|---|
| `63 6d` | `03 00` | count `0f`, then one mode byte per record 0–14: `02` for records 1, 2, 3, 13, 14 (the fans), `05` for the rest. On the Commander Core `00` is fixed percent and `02` is curve; `05` is "no device" [inference] |
| `64 6d` | `04 00` | count `0f`, then LE16 percent per record: record 1 = 60, record 3 = 25, the rest 0. Not in effect while the mode is `02` [inference] |
| `65 6d` | `05 00` | count `0f`, then per record: sensor byte, point count, points as LE16 temperature (0.1 °C) + LE16 percent. Records without a fan: `00 00`. All five fans hold the same curve, sensor `02`, 7 points: 44.0→20, 52.0→30, 57.0→38, 60.0→45, 63.0→54, 66.0→66, 70.0→70. 171 bytes, one report |
| `66 6d` | `11 00` | `00 00 00 00 63 6d 64 6d 65 6d`: an index naming the three tables |
| `60 6d`, `61 6d`, `62 6d` | `4c b9`, `69 6c`, `35 00` | not fan tables here. `61 6d` names `60 6d`; `62 6d` lists channels 1, 2, 3, 13, 14 as LE32 and has OpenLinkHub's Time Warp data type |
| `67 6d`–`6f 6d`, `1a` | — | open answers status `01`: no such resource [inference] |

The temperature resource `21` reports 52 records, not 26; records 26–51 were all "not available".

Not tested: whether a table write persists across power loss, the largest point count, writes to `63 6d` and
`64 6d`, and other mode values.

## Stored curves run in software mode

Measured on 2026-11-19 with [`hw-table-write-test.py`](../tools/hw-table-write-test.py) and
[`hw-curve-tracking-test.py`](../tools/hw-curve-tracking-test.py), driver unloaded. Both wrote channel 1's record in `65 6d`
and restored the original 171 bytes (read back identical). LX120 reference: 20 / 30 / 40 / 50 % = 405 / 659 / 907 /
1158 RPM, about 25 RPM per percent.

| Fact | Evidence |
|---|---|
| A write to `65 6d` is accepted and reads back | status `00` on five writes; each read-back matched |
| The hub applies a written table only at a mode change | channel 1 flattened to 40 %: 405 RPM for 25 s in software mode, 908 RPM after 12 s in hardware mode. Original restored: still 907 RPM 20 s later. Hardware mode then software mode back to back (two commands) applies a table at once |
| A fan with no host duty follows its stored curve in software mode, continuously, and its speed stays readable | curve 22→20 %, 26→30, 30→40, 33→52, 36→64, 40→80, 45→100 on channel 1, sensor byte `02`: 1224 RPM at 33.1 °C on temperature record 2 (curve: 52.4 %), 1178–1190 at 32.7 °C (50.8 %), 1288 at 33.8 °C (55.2 %), 1130 and falling at 32.1 °C (48.4 %). Linear between points |
| The sensor byte is the temperature record index | sensor `0d` with the same curve: 591 RPM, steady, with record 13 at 25.0 °C (curve: 27.5 % ≈ 595 RPM) |
| Sensor byte `00` is not record 0 | same curve, record 0 at 34.5 °C (58 % ≈ 1360 RPM): 1971 RPM after 27 s and still rising. What it selects is unknown; full speed for a missing sensor would fit [inference] |
| A host duty holds until the next mode change; a mode change returns every fan to its curve | channels 1 and 3 at 50 % (1158 RPM); hardware then software mode back to back, duty sent again for channel 3 only: channel 3 stayed at 1157–1159 RPM, channel 1 fell 1099 → 665 RPM in 20 s towards its curve |
| The hub limits the rate of change | LX120: about +50 RPM/s (405 → 1145 in 15 s) and −25 RPM/s (1099 → 665 in 18 s) |

So the hub's own control is available per fan inside software mode: leave a fan without a host duty and it runs its
stored curve; the way to hand a fan back is a mode change followed by the duties of the fans that stay host-driven.
Not tested: a non-zero second byte in a set-speed record (every implementation sends `00`).

## Hardware mode

Measured on 2026-11-19 with [`hw-mode-probe.py`](../tools/hw-mode-probe.py), driver unloaded, hub in hardware mode for 35 s.

| Fact | Evidence |
|---|---|
| Every open is refused, on every handle | handles 0, 1 and 2, resources `17`, `21`, `36`, `63 6d`, `65 6d`: status `03` each. In software mode the same 15 opens and reads succeed, so the handles are interchangeable |
| No report arrives unprompted | 3 s idle: 0 reports on either interface, in both modes |
| Property gets work, and none carries a speed or a temperature | `02 xx` for xx = `00`–`ff`: status `00` for `03` (mode: `01` hardware, `02` software), `11` (`1b1c`), `12` (`0c3f`), `13`, `14` (versions), `36` (`0e 60` = bits 1, 2, 3, 13, 14: the connected channels), `3d`, `3e`, `65`, `af` (rose by 36 in 36 s: uptime in seconds [inference]), `cb`, `cc`, `ce`, `d5`. The same set answers in software mode; only `03` and `af` differ |

None of the open implementations reads speeds or temperatures in hardware mode or by another route (survey of
OpenLinkHub, FanControl.CorsairLink, liquidctl, SignalRGB, ckb-next and the iCUE 5.54 binaries, 2026-11-19). iCUE
calls hardware mode "Device Memory Mode": "all your iCUE LINK Devices will play pre-saved configurations".

## Duty floor

Through the driver, 20 s per step, 2026-11-19:

| Duty | LX120 (ch 1, 3) | QX140 (ch 2, 13, 14) |
|---|---|---|
| 20 % | 405, 403 RPM | 404, 401, 403 RPM |
| 10 % | 0, 0 | 402, 403, 402 |
| 0 % | 0, 0 | 0, 0, 0 |

## Reply attribution by echo byte alone is not enough

A driver that shared the hub's hidraw node and matched replies by echo byte (`corsair-bragi` 0.1.0), beside an
OpenRGB that opened the colour handle before every frame, logged one `hub left software mode, re-entering` in
18 minutes with nothing sending hardware mode (18:51:07). An open of the already open colour handle answers
`0d 03`; a driver waiting for the reply to its own `0d 01 …` takes that reply as its own and reads `03` as hardware
mode [inference: the timing fits, about 0.4 % per keepalive; no trace was captured].

## What the driver takes from this

- Replies name no sender, so exact attribution needs every command to pass through one place, one exchange at a
  time. The driver hides the hub's hidraw node and gives programs its own.
- Handles are independent, also for the same resource and between the reports of a long write. So the driver locks
  per exchange, not per sequence, uses a handle number no program uses (`fd`), and lets programs read speeds,
  temperatures and tables themselves.
- The reply to a close still carries the bytes of the read before it, but nothing says it must. The driver parses
  a read from the reply to the read.
- A command runs with any flags byte, with or without a reply. The driver applies its rules by address and opcode,
  not by matching the three header bytes, and sends every command with the reply bit so that it sees the outcome.
- The hub refuses a long close list and closes nothing then. The driver stops following a handle only when the
  close was answered `00`.
- A mode command resets every fan and closes every handle. Programs' mode commands are answered without being
  sent. When the driver changes mode itself, it sends the host duties again and reopens the handles programs had
  open.
- A fan without a host duty runs its stored curve inside software mode. That is the automatic setting per fan
  (`pwmN_enable` = 2), and the state the driver starts every fan in.
- A 1 s poll is far inside the 20–30 s timeout. On unbind the driver sends hardware mode.
