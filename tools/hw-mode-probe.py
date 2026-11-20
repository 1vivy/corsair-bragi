"""What the hub answers in hardware mode, then in software mode with no duty set. Reads and property gets only,
plus the two mode commands.

    sudo python3 tools/hw-mode-probe.py /dev/hidrawCMD /dev/hidrawOTHER

Run with the corsair-bragi driver unloaded (the hub is then in hardware mode) and nothing else on the nodes.
Leaves the hub in hardware mode. Results: hid-probe.md.
"""
import os, select, sys, time

fd = os.open(sys.argv[1], os.O_RDWR | os.O_NONBLOCK)
fd2 = os.open(sys.argv[2], os.O_RDONLY | os.O_NONBLOCK)
FANS = (1, 2, 3, 13, 14)


def listen(sec):
    n = {fd: 0, fd2: 0}
    end = time.monotonic() + sec
    while time.monotonic() < end:
        for f in select.select([fd, fd2], [], [], 0.1)[0]:
            d = os.read(f, 512)
            n[f] += 1
            if n[f] <= 2:
                print("   unsolicited", "cmd" if f == fd else "other", d[:16].hex(" "))
    print(f"   {sec} s idle: {n[fd]} reports on the command node, {n[fd2]} on the other")


def xfer(cmd):
    while select.select([fd], [], [], 0)[0]:
        os.read(fd, 512)
    buf = bytes([0, 0, 1]) + bytes(cmd)
    os.write(fd, buf + bytes(513 - len(buf)))
    end = time.monotonic() + 0.3
    while time.monotonic() < end:
        if select.select([fd], [], [], 0.05)[0]:
            d = os.read(fd, 512)
            if d[2] == cmd[0]:
                return d
    return None


def res_read(handle, res):
    xfer([0x05, 0x01, handle])
    o = xfer([0x0D, handle, *res])
    if not o or o[3]:
        return o[3] if o else None, None
    r = xfer([0x08, handle])
    xfer([0x05, 0x01, handle])
    return (r[3], r) if r else (None, None)


def speeds():
    st, r = res_read(1, [0x17])
    if st != 0:
        return f"status {st}"
    return " ".join(f"ch{c}={int.from_bytes(r[8 + 3 * c:10 + 3 * c], 'little')}" for c in FANS)


def get_scan():
    out = {}
    for p in range(256):
        r = xfer([0x02, p])
        out[p] = (r[3], r[4:12].hex(" ")) if r else (None, "")
    return out


def phase(name):
    print(f"\n== {name}")
    r = xfer([0x02, 0x03])
    print("   mode property (02 03):", r[3:8].hex(" ") if r else "timeout")
    listen(3)
    for handle in (1, 0, 2):
        for res in ([0x17], [0x21], [0x36], [0x63, 0x6D], [0x65, 0x6D]):
            st, r = res_read(handle, res)
            tail = f" type={r[4:6].hex()} data={r[6:22].hex(' ')}" if r is not None and st == 0 else ""
            print(f"   handle {handle} res {bytes(res).hex()}: status {st}{tail}")
    return get_scan()


t0 = time.monotonic()
hw = phase("hardware mode")
print("   gets with status 00:", {f"{p:02x}": v[1] for p, v in hw.items() if v[0] == 0})

time.sleep(max(0, 35 - (time.monotonic() - t0)))
print("\n== enter software mode:", xfer([0x01, 0x03, 0x00, 0x02])[3:4].hex())
print("   first speeds:", speeds())
sw = phase("software mode, no duty set")
print("   gets with status 00:", {f"{p:02x}": v[1] for p, v in sw.items() if v[0] == 0})
print("   gets that differ:", {f"{p:02x}": (hw[p], sw[p]) for p in hw if hw[p] != sw[p]})
for _ in range(10):
    time.sleep(3)
    print(f"   +{time.monotonic() - t0:5.1f} s speeds:", speeds())
print("\n== back to hardware mode:", xfer([0x01, 0x03, 0x00, 0x01])[3:4].hex())
