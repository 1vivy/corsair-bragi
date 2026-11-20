"""Handle numbers, the same resource on two handles, chunked colour writes interleaved with reads, interface 1.

    sudo python3 tools/handles-probe.py /dev/hidrawCMD /dev/hidrawOTHER

Run with the corsair-bragi driver unloaded and nothing else on the nodes (the hub is then in hardware mode).
Reads, opens and closes only, plus two all-black colour frames and the mode commands. Leaves the hub in hardware
mode. Results: hid-probe.md.
"""
import os, select, sys, time

fd = os.open(sys.argv[1], os.O_RDWR | os.O_NONBLOCK)
fd2 = os.open(sys.argv[2], os.O_RDWR | os.O_NONBLOCK)
SOFTWARE, HARDWARE = [0x01, 0x03, 0x00, 0x02], [0x01, 0x03, 0x00, 0x01]


def xfer(cmd, timeout=0.5):
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
    return None


def st(r):
    return "--" if r is None else f"{r[3]:02x}"


def mode():
    r = xfer([0x02, 0x03])
    return r[4] if r else None


print("== interface 1: software-mode command written to the other node, hub in hardware mode")
print("   mode before:", mode())
try:
    buf = bytes([0, 0, 1]) + bytes(SOFTWARE)
    n = os.write(fd2, buf + bytes(513 - len(buf)))
    print("   write returned", n)
except OSError as e:
    print("   write failed:", e)
time.sleep(0.2)
print("   mode after:", mode(), "| reports on the other node:", bool(select.select([fd2], [], [], 0.3)[0]))

r = xfer(SOFTWARE)
print("\n== software mode:", st(r), "| reply bytes 0-7:", r[:8].hex(" "))

print("\n== open of speeds (17) on every handle number")
by = {}
for h in range(256):
    xfer([0x05, 0x01, h], 0.1)
    o = xfer([0x0D, h, 0x17], 0.1)
    rd = xfer([0x08, h], 0.1) if o is not None and o[3] == 0 else None
    c = xfer([0x05, 0x01, h], 0.1)
    by.setdefault((st(o), st(rd), st(c)), []).append(h)
for k, hs in by.items():
    shown = " ".join(f"{h:02x}" for h in hs[:12]) + (" …" if len(hs) > 12 else "")
    print(f"   open/read/close {k}: {len(hs)} handles: {shown}")

print("\n== the same resource on two handles at once")
for a, b in ((1, 2), (0, 1)):
    xfer([0x05, 0x01, a]); xfer([0x05, 0x01, b])
    oa, ob = xfer([0x0D, a, 0x17]), xfer([0x0D, b, 0x17])
    ra, rb = xfer([0x08, a]), xfer([0x08, b])
    ra2 = xfer([0x08, a])
    print(f"   17 on {a} and {b}: open {st(oa)} {st(ob)} | read {st(ra)} {st(rb)} | second read on {a}: {st(ra2)}"
          f" | types {ra[4:6].hex() if ra else ''} {rb[4:6].hex() if rb else ''}")
    ca = xfer([0x05, 0x01, a])
    rb2 = xfer([0x08, b])
    print(f"      close {a}: {st(ca)} | read on {b} after that: {st(rb2)}")
    xfer([0x05, 0x01, b])

print("\n== two resources on two handles at once")
xfer([0x05, 0x01, 1]); xfer([0x05, 0x01, 2])
o1, o2 = xfer([0x0D, 1, 0x17]), xfer([0x0D, 2, 0x21])
r2, r1 = xfer([0x08, 2]), xfer([0x08, 1])
print(f"   17 on 1, 21 on 2: open {st(o1)} {st(o2)} | read types {r1[4:6].hex()} {r2[4:6].hex()}")
xfer([0x05, 0x01, 1]); xfer([0x05, 0x01, 2])

print("\n== close with a count of two handles")
xfer([0x0D, 1, 0x17]); xfer([0x0D, 2, 0x21])
c = xfer([0x05, 0x02, 0x01, 0x02])
print(f"   05 02 01 02: {st(c)} | read on 1: {st(xfer([0x08, 1]))} | read on 2: {st(xfer([0x08, 2]))}")
xfer([0x05, 0x01, 1]); xfer([0x05, 0x01, 2])

print("\n== operations on a closed handle")
print("   write 06 01 (set-speed payload, handle closed):", st(xfer([0x06, 0x01, 0x03, 0x00, 0, 0, 0x07, 0x00, 0x00])))
print("   read 08 01:", st(xfer([0x08, 0x01])), "| continue 07 01:", st(xfer([0x07, 0x01, 0x00])))

print("\n== colour frame in two reports (600 colour bytes, all black), reads on handle 2 in between")
total = 600
payload = (total + 2).to_bytes(2, "little") + b"\0\0" + bytes([0x12, 0x00]) + bytes(total)
print("   open colour on 0:", st(xfer([0x0D, 0x00, 0x22])))
w1 = xfer([0x06, 0x00, *payload[:508]])
xfer([0x05, 0x01, 2])
o = xfer([0x0D, 2, 0x17]); rd = xfer([0x08, 2]); c = xfer([0x05, 0x01, 2])
w2 = xfer([0x07, 0x00, *payload[508:]])
print(f"   first report {st(w1)} | speeds on 2: open {st(o)} read {st(rd)} type {rd[4:6].hex() if rd else ''} close {st(c)}"
      f" | continue {st(w2)}")
short = (416).to_bytes(2, "little") + b"\0\0" + bytes([0x12, 0x00]) + bytes(414)
print("   one-report frame after that:", st(xfer([0x06, 0x00, *short])))

print("\n== timing, 200 exchanges (get mode)")
ts = []
for _ in range(200):
    t = time.perf_counter(); xfer([0x02, 0x03]); ts.append((time.perf_counter() - t) * 1000)
ts.sort()
print(f"   ms min/median/max: {ts[0]:.2f} / {ts[100]:.2f} / {ts[-1]:.2f}")
ts = []
for _ in range(100):
    t = time.perf_counter(); xfer([0x06, 0x00, *short]); ts.append((time.perf_counter() - t) * 1000)
ts.sort()
print(f"   one-report colour frame ms min/median/max: {ts[0]:.2f} / {ts[50]:.2f} / {ts[-1]:.2f}")

print("\n== back to hardware mode:", st(xfer(HARDWARE)))
