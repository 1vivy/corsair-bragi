"""Read-only probe of hub resources on the data handle (close, open, read, close; nothing is written).

    sudo python3 tools/hw-tables-probe.py /dev/hidrawN 636d 646d 656d 666d

Each argument is a resource id in hex, one or two bytes as sent after `0d 01`. Runs beside the corsair-bragi
driver: it waits for the end of the driver's 1 s poll and probes in the gap. Results: hid-probe.md.
"""
import os, select, sys, time

fd = os.open(sys.argv[1], os.O_RDWR | os.O_NONBLOCK)


def wait_quiet():
    """Return right after the driver's poll burst ends (traffic, then 60 ms of silence)."""
    seen = False
    end = time.monotonic() + 3
    while time.monotonic() < end:
        if select.select([fd], [], [], 0.06)[0]:
            if os.read(fd, 512)[2] in (0x08, 0x05):
                seen = True
        elif seen:
            return
    raise SystemExit("no driver poll seen")


def xfer(cmd):
    buf = bytes([0, 0, 1]) + bytes(cmd)
    os.write(fd, buf + bytes(513 - len(buf)))
    end = time.monotonic() + 0.3
    while time.monotonic() < end:
        if select.select([fd], [], [], 0.05)[0]:
            d = os.read(fd, 512)
            if d[2] == cmd[0]:
                return d
    return None


def probe(res):
    xfer([0x05, 0x01, 0x01])
    o = xfer([0x0D, 0x01, *res])
    out = f"{bytes(res).hex():>4} open={o[3]:02x}" if o else f"{bytes(res).hex():>4} open=timeout"
    if o and o[3] == 0:
        r = xfer([0x08, 0x01])
        if r:
            body = r[6:].rstrip(b"\0")
            out += f" read={r[3]:02x} type={r[4:6].hex()} len={len(body)} data={body.hex(' ')}"
        xfer([0x05, 0x01, 0x01])
    return out


targets = [[int(x, 16) for x in (a[:2], a[2:]) if x] for a in sys.argv[2:]]
for i in range(0, len(targets), 12):
    wait_quiet()
    for res in targets[i:i + 12]:
        print(probe(res))
