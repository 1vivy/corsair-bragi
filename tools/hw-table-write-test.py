"""Does the hub follow its stored curve table in software mode, and does a write to it take effect?

    sudo python3 tools/hw-table-write-test.py /dev/hidrawCMD BACKUP_FILE

Run with the corsair-bragi driver unloaded and nothing else on the node. WRITES the stored curve table `65 6d`:
flattens channel 1's curve to 40 %, observes, then writes the original bytes back and verifies them. The original
table is saved to BACKUP_FILE (hex) before the first write. Leaves the hub in hardware mode. Results: hid-probe.md.
"""
import os, select, sys, time

fd = os.open(sys.argv[1], os.O_RDWR | os.O_NONBLOCK)
backup = sys.argv[2]
FANS = (1, 2, 3, 13, 14)
CURVES, CURVES_TYPE = [0x65, 0x6D], bytes([0x05, 0x00])
SOFTWARE, HARDWARE = [0x01, 0x03, 0x00, 0x02], [0x01, 0x03, 0x00, 0x01]
t0 = time.monotonic()


def log(*a):
    print(f"[{time.monotonic() - t0:6.1f}]", *a, flush=True)


def xfer(cmd, timeout=2.0):
    while select.select([fd], [], [], 0)[0]:
        os.read(fd, 512)
    buf = bytes([0, 0, 1]) + bytes(cmd)
    os.write(fd, buf + bytes(513 - len(buf)))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if select.select([fd], [], [], 0.05)[0]:
            d = os.read(fd, 512)
            if d[2] == cmd[0]:
                return d
    raise SystemExit(f"no reply to {bytes(cmd[:4]).hex(' ')}")


def res_read(res):
    xfer([0x05, 0x01, 0x01])
    o = xfer([0x0D, 0x01, *res])
    if o[3]:
        return o[3], None
    r = xfer([0x08, 0x01])
    xfer([0x05, 0x01, 0x01])
    return r[3], r


def res_write(res, dtype, data):
    payload = (len(data) + 2).to_bytes(2, "little") + b"\0\0" + dtype + data
    xfer([0x05, 0x01, 0x01])
    o = xfer([0x0D, 0x01, *res])
    if o[3]:
        return f"open {o[3]:02x}"
    w = xfer([0x06, 0x01, *payload])
    xfer([0x05, 0x01, 0x01])
    return f"write {w[3]:02x}"


def speeds():
    st, r = res_read([0x17])
    if st != 0:
        return f"status {st}"
    return " ".join(f"ch{c}={int.from_bytes(r[8 + 3 * c:10 + 3 * c], 'little')}" for c in FANS)


def watch(sec, step=2.5):
    for _ in range(int(sec / step)):
        time.sleep(step)
        log("  speeds:", speeds())


def read_curves():
    st, r = res_read(CURVES)
    assert st == 0 and r[4:6] == CURVES_TYPE, (st, r and r[4:6].hex())
    body, pos, recs = r[6:], 1, []
    for _ in range(body[0]):
        ln = 2 + 4 * body[pos + 1]
        recs.append(bytes(body[pos:pos + ln]))
        pos += ln
    return bytes(body[:pos]), recs


log("software mode:", xfer(SOFTWARE)[3:4].hex(), "| first speeds:", speeds())
orig, recs = read_curves()
assert orig[0] == 15 and recs[1][1] == 7, (orig[0], recs[1][:2].hex())
with open(backup, "w") as f:
    f.write(orig.hex())
log(f"original table, {len(orig)} bytes, saved to {backup}")
log("  channel 1 record:", recs[1].hex(" "))

flat = bytearray(recs[1])
for p in range(flat[1]):
    flat[2 + 4 * p + 2:2 + 4 * p + 4] = (40).to_bytes(2, "little")
mod = bytes([orig[0]]) + b"".join(bytes(flat) if i == 1 else r for i, r in enumerate(recs))
assert len(mod) == len(orig)

try:
    log("write channel 1 flat 40 %:", res_write(CURVES, CURVES_TYPE, mod))
    log("  read back matches the written table:", read_curves()[0] == mod)
    log("software mode, no duty set:")
    watch(25)
    log("hardware mode for 12 s:", xfer(HARDWARE)[3:4].hex())
    time.sleep(12)
    log("software mode:", xfer(SOFTWARE)[3:4].hex(), "| first speeds:", speeds())
    watch(10)
finally:
    log("restore original table:", res_write(CURVES, CURVES_TYPE, orig))
    back = read_curves()[0]
    log("  read back matches the original:", back == orig)
    if back != orig:
        log("  MISMATCH, table now:", back.hex(" "))

log("after restore, software mode, no duty set:")
watch(20)

# Can one fan be handed back to the hub while the others keep a host duty?
log("direct duty: ch3 = 50 %, ch1 = 50 %:", res_write([0x18], bytes([0x07, 0x00]), bytes([2, 1, 0, 50, 0, 3, 0, 50, 0])))
watch(15)
log("hardware then software mode, back to back:", xfer(HARDWARE)[3:4].hex(), xfer(SOFTWARE)[3:4].hex())
log("direct duty again for ch3 only:", res_write([0x18], bytes([0x07, 0x00]), bytes([1, 3, 0, 50, 0])))
watch(20)
log("hardware mode:", xfer(HARDWARE)[3:4].hex())
