# USB HID protocol: what the open implementations agree on

Consolidated from the host implementations below, at the commits given. Where the implementations disagree, both
readings are given. Line numbers refer to those commits. Measurements on a hub (reply order, status codes, mode
timeout, handle independence): [`hid-probe.md`](hid-probe.md).

| Short | Repository, commit | Path |
|---|---|---|
| OLH | [OpenLinkHub](https://github.com/jurkovic-nikola/OpenLinkHub) `4b59c5a2532e` | `src/devices/lsh/lsh.go` |
| OLDB | OpenLinkHub | `database/external/lsh.json` |
| FCL | [FanControl.CorsairLink](https://github.com/EvanMulawski/FanControl.CorsairLink) `4bf8cbb5bed2` | `src/devices/icue_link/` |
| ORL | [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) `b2a410db131b` | `Controllers/CorsairICueLinkController/` |
| ORCC | OpenRGB | `Controllers/CorsairCommanderCoreController/` |
| LC | [liquidctl](https://github.com/liquidctl/liquidctl) `292dc561361c` | `liquidctl/driver/commander_core.py` |
| CCD | liquidctl | `docs/developer/protocol/commander_core.md` |

## Framing

| Fact | Source |
|---|---|
| OUT report 513 B (report ID + 512), IN 512 B. OUT `[0]=0x00` report ID, `[1]=0x00`, `[2]=0x01`, command from `[3]`. `[1]` is an address: `00` the hub, N the LINK device on channel N (measured, [`hid-probe.md`](hid-probe.md) "Header bytes"). OLH sends `[1]=0x19` for a PSU and `[1]=<channel>` for a v2 device's model; FCL always sends `00` | OLH:263-302,6039-6078; FCL/LinkHubDataWriter.cs:50-73; ORL/CorsairICueLinkProtocol.h:61-64 |
| Write payload: LE16 length (`data+2`), `00 00`, 2-byte data type, data. Chunk limit 508 B; longer writes continue with `07 00` | OLH:298,5769-5776,5825-5842; FCL/LinkHubDataWriter.cs:33-46; ORL/CorsairICueLinkController.cpp:231-251 |
| Reply: `[4:6]` data type, `[6]` record count, records from `[7]` (offsets without report ID) | OLH:5231-5243,6084-6087; FCL/LinkHubDataReader.cs:8-30 |
| Disagreement: FCL keeps the report ID (status at raw `[4]`, type at raw `[5:7]`); OLH and ORL compare type at `[4]`, ORL only one byte | FCL/ICueLinkHubDevice.cs:363-406; OLH:6084-6087; ORL/CorsairICueLinkController.cpp:207-218 |
| Status byte: `00` OK, `03` wrong mode (FCL re-enters software mode). Measured: `03` also answers a read on a closed endpoint in software mode, and an invalid command answers `05` in software mode | FCL/ICueLinkHubDevice.cs:37-40,371-376; [`hid-probe.md`](hid-probe.md) |

## Commands

| Command | Bytes | Source |
|---|---|---|
| Software mode | `01 03 00 02` | OLH:268; FCL/ICueLinkHubDevice.cs:11; ORL/CorsairICueLinkProtocol.h:72 |
| Hardware mode | `01 03 00 01` | OLH:269; FCL/ICueLinkHubDevice.cs:12 |
| Get device mode (declared, unused) | `01 08 01` | ORL/CorsairICueLinkProtocol.h:78 |
| Firmware version | `02 13`; reply `[4].[5].LE16[6:8]` | OLH:267,5339-5352; FCL/LinkHubDataReader.cs:92-100 |
| Open data / color endpoint | `0d 01 <mode>` / `0d 00 <mode>` | OLH:263-264; ORL/CorsairICueLinkProtocol.h:68-69 |
| Close data / color endpoint | `05 01 01 <mode>` / `05 01` (ORL uses `05 01 01` for both) | OLH:265-266; ORL/CorsairICueLinkController.cpp:247,306 |
| Read / read color | `08 01` / `08 00` | OLH:272-273,5355-5414 |
| Write / write color / continue | `06 01` / `06 00` / `07 00` (FCL has no color or continue) | OLH:270-271,5837-5842; ORL/CorsairICueLinkProtocol.h:74-76 |

## Endpoints (modes) and reply data types

| Data | Mode | Reply type | Source |
|---|---|---|---|
| Subdevices | `36` | `21 00` | OLH:282,294; FCL/ICueLinkHubDevice.cs:25,33 |
| Temperatures | `21` | `10 00` | OLH:283,292 |
| Speeds | `17` | `25 00` | OLH:284,293 |
| Set fixed speed | `18` | `07 00` | OLH:285,295,3595-3624 |
| Colors | `22` | `12 00` | OLH:286,296 |
| LED channel counts (`02` connected, `03` not, LE16 count) | `20` | — | OLH:281,5231-5243 |
| Adapter command codes / LED counts | `1e` / `1d` | `0d 00` / `0c 00` | OLH:275-276,1830-1842 |
| PSU volts / amps, at address `19`: LE16 rail count at `[6]` (at most 3), 4-byte records from `[8]`, LE16 at record `[2]` ÷ 1000 V or ÷ 100 A; watts are computed. Not verified on a PSU | `28` / `29` | `23 00` / `24 00` (one owner report, OLH issue 553) | OLH:287-288,3774-3854 |
| Device models (v2 channel select) | `40` | — | OLH:289,4156 |
| Timewarp | `37` | `35 00` | OLH:277,298 |

The LCD cap is a separate HID device with feature reports (`03 19 00 01`, `03 0b 00 01`, `03 0c <rot> 01`, …):
OLH:278-280,2098-2124.

## PSUs behind a hub

Looked up 2026-11-19; nothing here is measured, this hub has no PSU on it.

| Fact | Source |
|---|---|
| The HXi SHIFT supplies (HX1000i, HX1200i, HX1500i SHIFT) contain an iCUE LINK System Hub with two LINK ports | [Corsair HXi SHIFT manual](https://www.corsair.com/us/en/explorer/diy-builder/power-supply-units/corsair-hxi-shift-series/) |
| An HX1000i SHIFT shows up as hub `1b1c:0c3f` with the PSU as LINK channel 25 (`0x19`), type `0b` | one owner report, [OLH issue 553](https://github.com/jurkovic-nikola/OpenLinkHub/issues/553) |
| Type `0b` models 0 / 1 / 2 are HX1000i / HX1200i / HX1500i. iCUE's manifests number them 11, 267, 523 (`0x0b`, `0x10b`, `0x20b`) | OLDB:381-415; [bragi-re](https://github.com/ToastKiste21/bragi-re) `manifests/dev-link-hx1{000,200,500}i.json` |
| Temperature, fan speed and fan duty of the PSU are the hub's own records for its channel (`21`, `17`, `18`). OLH and FCL send it a duty; whether a PSU obeys is shown nowhere | OLH:3695-3756,3595-3629; FCL `KnownLinkDevice.cs:25-51` |
| iCUE: the manifests declare current, voltage and computed power sensors and `HidProto: Bragi_link_ecosystem_subdevice`, with no command, resource or address. The decompiled `BragiProtocols.dll` has no PSU handler | bragi-re `manifests/dev-link-hx1000i.json:7-108`, `decompiled/` |
| Mainline `corsair-psu` is another protocol (64-byte reports, PMBus commands, LINEAR11) for the supplies with their own USB id (`1c03`–`1c0d`, `1c1e`, `1c1f`, `1c23`, `1c27`). No SHIFT supply is in its table. Its `pwm1` is read only | `drivers/hwmon/corsair-psu.c` |
| Not established: whether a SHIFT supply also has a `1cxx` USB interface, and the USB id of the HX1200i and HX1500i SHIFT hubs | - |

## Records

| Fact | Source |
|---|---|
| Subdevice record: 8 B (type `[2]`, model `[3]`, ID length `[7]`) + ASCII ID; channels start at 1 | OLH:3958-3975; FCL/LinkHubDataReader.cs:8-64 |
| More than 14 devices: a second `08 01` read is appended (FCL only on firmware ≥ 2.5) | OLH:5361-5374; FCL/ICueLinkHubDevice.cs:130-134 |
| Speed/temperature: count, then `[status, LE16]`; status `00` available; temperature in 0.1 °C | OLH:3699-3755; FCL/LinkHubDataReader.cs:101-137 |

Child device types (type → device, LEDs): `01` QX RGB 34, `02` LX RGB 18, `03` RX RGB MAX 8, `04` RX MAX 0,
`0f` RX RGB 8, `13` RX 0, `05` LINK adapter, `06` pump LCD 24, `07` H-series AIO 20, `11` TITAN 20, `09` XC7 24,
`0c` XD5 22, `0e` XD5 LCD (duplicate, skipped), `19` XD6 22, `0d` XG7 16, `0a` XG3 0, `10` VRM cap, `0b` HXi PSU,
`1b` Commander Duo (LEDs read from mode `20`), `1a` v2 devices (by model). Sources: OLDB:3-445;
ORL/CorsairICueLinkProtocol.h:26-57; FCL/KnownLinkDevice.cs:26-39. No implementation covers every type.

## Timing

OLH refreshes devices every 1 s and temperatures every 3 s and waits 500 ms after entering software mode
(OLH:307-309,5229-5234). ORL sends a keepalive every 5 s (ORL/RGBController_CorsairICueLink.cpp:147-155). The ~10 s
fall-back to hardware mode is stated by the OpenLinkHub author (liquidctl #693); no implementation encodes the value.
Measured on firmware 3.10.636: 20–30 s ([`hid-probe.md`](hid-probe.md)).

## Firmware update

None of these implementations flashes firmware or has a bootloader command; all read the version with `02 13`.
The related Commander Core protocol (CCD, LC, ORCC) uses the same opcodes behind a different header (`[1]=0x08`,
96 B or 1024 B reports).
