"""Does a fan with no host duty keep following the stored curve in software mode, and what does the sensor byte select?

    sudo python3 tools/hw-curve-tracking-test.py /dev/hidrawCMD BACKUP_FILE

Run with the corsair-bragi driver unloaded and nothing else on the node. WRITES the stored curve table `65 6d`:
gives channel 1 a steep curve (22 °C → 20 % … 45 °C → 100 %), moves the channel 2 sensor by stopping and then
running the channel 2 fan, tries sensor bytes `02`, `0d` and `00`, then writes the original bytes back and verifies
them. The original table is saved to BACKUP_FILE (hex) before the first write. Leaves the hub in hardware mode.
Results: hid-probe.md.
"""
import os, select, sys, time

fd = os.open(sys.argv[1], os.O_RDWR | os.O_NONBLOCK)
backup = sys.argv[2]
CURVES, CURVES_TYPE = [0x65, 0x6D], bytes([0x05, 0x00])
SOFTWARE, HARDWARE = [0x01, 0x03, 0x00, 0x02], [0x01, 0x03, 0x00, 0x01]
STEEP = ((22.0, 20), (26.0, 30), (30.0, 40), (33.0, 52), (36.0, 64), (40.0, 80), (45.0, 100))
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
    assert o[3] == 0, f"open {bytes(res).hex()} status {o[3]}"
    r = xfer([0x08, 0x01])
    xfer([0x05, 0x01, 0x01])
    assert r[3] == 0, f"read {bytes(res).hex()} status {r[3]}"
    return r


def res_write(res, dtype, data):
    payload = (len(data) + 2).to_bytes(2, "little") + b"\0\0" + dtype + data
    xfer([0x05, 0x01, 0x01])
    o = xfer([0x0D, 0x01, *res])
    assert o[3] == 0, f"open {bytes(res).hex()} status {o[3]}"
    w = xfer([0x06, 0x01, *payload])
    xfer([0x05, 0x01, 0x01])
    return f"write {w[3]:02x}"


def rec(r, i):
    return int.from_bytes(r[8 + 3 * i:10 + 3 * i], "little") if r[7 + 3 * i] == 0 else None


def sample():
    s, t = res_read([0x17]), res_read([0x21])
    temps = " ".join(f"t{i}={rec(t, i) / 10:.1f}" for i in (0, 2, 13, 14))
    return f"ch1={rec(s, 1)} ch2={rec(s, 2)} rpm | {temps}"


def watch(sec, step=3.0):
    for _ in range(int(sec / step)):
        time.sleep(step)
        log("  ", sample())


def read_curves():
    r = res_read(CURVES)
    assert r[4:6] == CURVES_TYPE, r[4:6].hex()
    body, pos, recs = r[6:], 1, []
    for _ in range(body[0]):
        ln = 2 + 4 * body[pos + 1]
        recs.append(bytes(body[pos:pos + ln]))
        pos += ln
    return bytes(body[:pos]), recs


def reload_tables():
    """A hardware-mode entry makes the hub take the stored tables; it also clears host duties."""
    return xfer(HARDWARE)[3:4].hex() + " " + xfer(SOFTWARE)[3:4].hex()


def duty(ch, pct):
    return res_write([0x18], bytes([0x07, 0x00]), bytes([1, ch, 0, pct, 0]))


log("software mode:", xfer(SOFTWARE)[3:4].hex(), "|", sample())
orig, recs = read_curves()
assert orig[0] == 15 and len(recs[1]) == 30, (orig[0], len(recs[1]))
with open(backup, "w") as f:
    f.write(orig.hex())
log(f"original table, {len(orig)} bytes, saved to {backup}")


def table(sensor):
    r1 = bytes([sensor, len(STEEP)]) + b"".join(
        int(t * 10).to_bytes(2, "little") + p.to_bytes(2, "little") for t, p in STEEP)
    out = bytes([orig[0]]) + b"".join(r1 if i == 1 else r for i, r in enumerate(recs))
    assert len(out) == len(orig)
    return out


try:
    log("channel 1: steep curve, sensor 02:", res_write(CURVES, CURVES_TYPE, table(0x02)), "| reload:", reload_tables())
    watch(24)
    log("channel 2 fan stopped (host duty 0 %):", duty(2, 0))
    watch(48)
    log("channel 2 fan at 100 %:", duty(2, 100))
    watch(48)
    for sensor in (0x0D, 0x00, 0x02):
        log(f"channel 1: steep curve, sensor {sensor:02x}:", res_write(CURVES, CURVES_TYPE, table(sensor)),
            "| reload:", reload_tables())
        watch(27)
finally:
    log("restore original table:", res_write(CURVES, CURVES_TYPE, orig))
    back = read_curves()[0]
    log("  read back matches the original:", back == orig)
    if back != orig:
        log("  MISMATCH, table now:", back.hex(" "))
    log("reload:", reload_tables())

watch(15)
log("hardware mode:", xfer(HARDWARE)[3:4].hex())
