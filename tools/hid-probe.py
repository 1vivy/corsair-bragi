#!/usr/bin/env python3
"""iCUE LINK hub probe: what a kernel fan driver that shares the command channel has to know.

Run as root: sudo python3 tools/hid-probe.py
It stops openlinkhub.service, talks to the hub directly, returns the hub to hardware mode and starts the
service again. It takes about 6 min on firmware 3.10.636 (the mode timeout is 20-30 s, and three tests each run
2.5 times that long); the case fans run on the hub's hardware curve or at the probe's 40 % meanwhile.
Protocol: hid-protocol.md. Results of the 2026-11-19 runs: hid-probe.md.
"""
import fcntl, glob, os, select, signal, struct, subprocess, sys, time

# A killed run must still restore hardware mode and restart OpenLinkHub (the `finally` below).
signal.signal(signal.SIGTERM, lambda *_: sys.exit("terminated"))
signal.signal(signal.SIGHUP, lambda *_: sys.exit("terminated"))

FANS = (1, 2, 3, 13, 14)
LEDS = 138
CLOSE, OPEN, OPEN_COLOR = b"\x05\x01\x01", b"\x0d\x01", b"\x0d\x00"
READ, WRITE, WRITE_COLOR = b"\x08\x01", b"\x06\x01", b"\x06\x00"
SOFTWARE, HARDWARE, FIRMWARE = b"\x01\x03\x00\x02", b"\x01\x03\x00\x01", b"\x02\x13"
GET_MODE, BOGUS = b"\x01\x08\x01", b"\x01\x7f\x01"
M_SPEEDS, M_SET_SPEED, M_COLOR = b"\x17", b"\x18", b"\x22"
T_SPEEDS, T_SET_SPEED, T_COLOR = b"\x25\x00", b"\x07\x00", b"\x12\x00"
WRONG_MODE = 0x03


def command_node() -> str:
    """hidraw node of the hub's interface 0 (1b1c:0c3f); the number changes between boots."""
    for node in glob.glob("/sys/class/hidraw/hidraw*"):
        iface = os.path.realpath(f"{node}/device/..")
        try:
            with open(f"{iface}/../idVendor") as v, open(f"{iface}/../idProduct") as p, open(f"{iface}/bInterfaceNumber") as n:
                if (v.read().strip(), p.read().strip(), n.read().strip()) == ("1b1c", "0c3f", "00"):
                    return "/dev/" + os.path.basename(node)
        except FileNotFoundError:
            continue
    raise SystemExit("hub interface 0 not found")


T0 = time.monotonic()
latency: list[float] = []


def log(*a):
    print(f"[{time.monotonic() - T0:6.1f}]", *a, flush=True)


def xfer(payload: bytes) -> bytes:
    buf = bytearray(513)  # byte 0 = report id 0
    buf[2] = 0x01
    buf[3 : 3 + len(payload)] = payload
    start = time.monotonic()
    os.write(fd, bytes(buf))
    if not select.select([fd], [], [], 1.0)[0]:
        raise TimeoutError(f"no reply to {payload[:6].hex()}")
    reply = os.read(fd, 512)
    latency.append(time.monotonic() - start)
    return reply


def framed(dtype: bytes, data: bytes) -> bytes:
    return struct.pack("<H", len(data) + 2) + b"\x00\x00" + dtype + data


def read_speeds():
    """(status bytes of close/open/read/close, {channel: rpm} when the read succeeded)."""
    replies = [xfer(CLOSE + M_SPEEDS), xfer(OPEN + M_SPEEDS), xfer(READ + M_SPEEDS), xfer(CLOSE + M_SPEEDS)]
    r = replies[2]
    rpm = {}
    if r[3] == 0 and r[4:6] == T_SPEEDS:
        rpm = {i: struct.unpack_from("<h", r, 8 + 3 * i)[0] for i in range(r[6]) if r[7 + 3 * i] == 0 and i in FANS}
    return bytes(x[3] for x in replies).hex(" "), rpm


def set_speed(pct: int) -> int:
    data = bytes([len(FANS)]) + b"".join(bytes([ch, 0, pct, 0]) for ch in FANS)
    xfer(CLOSE + M_SET_SPEED)
    xfer(OPEN + M_SET_SPEED)
    status = xfer(WRITE + framed(T_SET_SPEED, data))[3]
    xfer(CLOSE + M_SET_SPEED)
    return status


COLOR_FRAME = WRITE_COLOR + framed(T_COLOR, bytes([0, 8, 16]) * LEDS)


def enter() -> None:
    """Software mode, fans at 40 %, colour endpoint open."""
    xfer(SOFTWARE)
    time.sleep(0.5)
    set_speed(40)
    xfer(CLOSE + M_COLOR)
    xfer(OPEN_COLOR + M_COLOR)


def lapsed() -> bool:
    # 01 08 01 is not a valid command: the hub answers 0x05 in software mode and 0x03 in hardware mode.
    # A read on a closed endpoint is no test: it answers 0x03 in software mode too.
    return xfer(GET_MODE)[3] == WRONG_MODE

def stats(xs):
    return f"n={len(xs)} min={min(xs) * 1e3:.2f} avg={sum(xs) / len(xs) * 1e3:.2f} max={max(xs) * 1e3:.2f} ms"


subprocess.run(["systemctl", "stop", "openlinkhub"], check=True)
time.sleep(1.5)
fd = os.open(command_node(), os.O_RDWR)
fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
try:
    xfer(HARDWARE)
    time.sleep(0.5)
    log("1. hardware mode: statuses of close/open/read/close =", read_speeds()[0],
        "| 01 08 01 ->", xfer(GET_MODE)[3], "| bogus 01 7f 01 ->", xfer(BOGUS)[3])
    enter()
    log("   software mode: statuses of close/open/read/close =", read_speeds()[0],
        "| 01 08 01 ->", xfer(GET_MODE)[3], "| bogus 01 7f 01 ->", xfer(BOGUS)[3])

    latency.clear()
    for _ in range(30):
        read_speeds()
    seq = stats(latency)
    latency.clear()
    for _ in range(30):
        xfer(COLOR_FRAME)
    log("2. exchange latency: fan read sequence", seq, "| colour write", stats(latency))

    time.sleep(8)
    before = read_speeds()[1]
    xfer(SOFTWARE)  # again, while already in software mode
    time.sleep(8)
    log("3. software-mode command repeated at 40 %: before", before, "after 8 s", read_speeds()[1])

    limit = None
    for t in (4, 6, 8, 10, 12, 15, 20, 30):
        read_speeds()  # real traffic right before the silence, whether or not the mode query counts as traffic
        time.sleep(t)
        gone = lapsed()
        log(f"4. {t:2d} s of silence ->", "hardware mode" if gone else "still software mode")
        if gone:
            limit = t
            break
    if limit is None:
        raise SystemExit("no lapse within 30 s")

    period, duration = max(1.0, limit / 4), 2.5 * limit
    kinds = {
        "fan reads only": lambda: read_speeds()[0],
        "colour writes only": lambda: xfer(COLOR_FRAME)[3],
        "firmware query only": lambda: xfer(FIRMWARE)[3],
    }
    for name, action in kinds.items():
        enter()
        end = time.monotonic() + duration
        seen = set()
        while time.monotonic() < end:
            seen.add(str(action()))
            time.sleep(period)
        log(f"5. {name} every {period:.1f} s for {duration:.0f} s ->",
            "hardware mode" if lapsed() else "still software mode", "| statuses seen:", sorted(seen))
finally:
    try:
        xfer(HARDWARE)
        log("hardware mode restored")
    except (OSError, TimeoutError) as e:
        log("FAILED to restore hardware mode:", e)
    os.close(fd)
    subprocess.run(["systemctl", "start", "openlinkhub"], check=False)
    log("openlinkhub started")
