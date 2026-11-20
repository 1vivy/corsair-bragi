"""Header bytes of a hub command: the flags byte and the address byte.

    sudo python3 tools/header-probe.py /dev/hidrawN

1. Does a command with another third byte execute (open a spare handle with it, read it with 01)?
2. What answers at the addresses in the second byte (gets only)?

Run it on the hub's own hidraw node with the corsair-bragi driver unloaded: the driver sets the reply
bit on every command, which hides what part 1 looks for. Replies to other programs on the node arrive
here too; only a reply whose echo and address match counts.
"""
import os
import select
import sys
import time

fd = os.open(sys.argv[1], os.O_RDWR | os.O_NONBLOCK)
SPARE = 0x30


def xfer(h1, h2, cmd, want=None):
    """Reply to cmd: first report whose echo byte is cmd[0] (or `want`), else None."""
    while select.select([fd], [], [], 0)[0]:
        os.read(fd, 512)
    buf = bytes([0, h1, h2]) + bytes(cmd)
    t = time.monotonic()
    os.write(fd, buf + bytes(513 - len(buf)))
    took = (time.monotonic() - t) * 1000
    end = time.monotonic() + 0.15
    while time.monotonic() < end:
        if select.select([fd], [], [], 0.05)[0]:
            r = os.read(fd, 512)
            if r[2] == (want if want is not None else cmd[0]) and r[0] == (h1 & 0x7F):
                return r, took
    return None, took


def show(r, n=10):
    return r[:n].hex(" ") if r else "no reply"


print("== third byte: open the spare handle on speeds with it, then read it with 01")
for h2 in (0x01, 0x03, 0x00, 0x02, 0x04, 0x81):
    xfer(0, 1, [0x05, 0x01, SPARE])
    o, took = xfer(0, h2, [0x0D, SPARE, 0x17])
    r, _ = xfer(0, 1, [0x08, SPARE])
    c, _ = xfer(0, 1, [0x05, 0x01, SPARE])
    print(f"h2={h2:02x}: open -> {show(o, 4)} ({took:.0f} ms) | framed read -> {show(r, 8)} | close -> {show(c, 4)}")

print("== second byte as an address: get firmware (02 13) and get mode (02 03)")
for addr in (0, 1, 2, 3, 4, 5, 12, 13, 14, 15):
    f, took = xfer(addr, 1, [0x02, 0x13])
    m, _ = xfer(addr, 1, [0x02, 0x03])
    print(f"addr {addr:2d}: firmware -> {show(f, 8)} ({took:.0f} ms) | mode -> {show(m, 8)}")
os.close(fd)
