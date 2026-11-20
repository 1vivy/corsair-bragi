#!/usr/bin/env bash
# Install corsair-bragi with DKMS, load it and bind it to every iCUE LINK
# System Hub. Idempotent: with an unchanged source it only reloads the module,
# which is also how the driver reads the LINK devices again.
# Run as a user with sudo. Needs dkms, rsync and the headers of each kernel.
# The fans run the hub's stored curves while the module is out. Programs that
# had the hub's hwmon chip or hidraw node open must be restarted afterwards.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)

step() { printf '\n== %s\n' "$*"; }

ver=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$here/dkms.conf")

step "DKMS source /usr/src/corsair-bragi-$ver"
sudo mkdir -p "/usr/src/corsair-bragi-$ver"
# The itemised list is empty when the installed source equals this checkout.
changed=$(sudo rsync -ai "$here/corsair-bragi.c" "$here/Makefile" "$here/dkms.conf" "/usr/src/corsair-bragi-$ver/")
if [ -z "$(dkms status "corsair-bragi/$ver")" ]; then
    sudo dkms add "corsair-bragi/$ver"
fi
# Other versions of this module: DKMS would keep installing them at kernel updates.
for old in $(dkms status corsair-bragi | sed -n 's|^corsair-bragi/\([^,]*\),.*|\1|p' | sort -u); do
    [ "$old" = "$ver" ] && continue
    sudo dkms remove "corsair-bragi/$old" --all
    sudo rm -rf "/usr/src/corsair-bragi-$old"
done

step "Build and install for every installed kernel"
for dir in /usr/lib/modules/*; do
    # A tree with headers and no modules.dep is a headers package without its kernel.
    [ -e "$dir/build/Makefile" ] && [ -e "$dir/modules.dep" ] || continue
    k=$(basename "$dir")
    if [ -z "$changed" ] && dkms status "corsair-bragi/$ver" -k "$k" | grep -q ': installed'; then
        continue
    fi
    sudo dkms install --force "corsair-bragi/$ver" -k "$k"
done
dkms status corsair-bragi

step "Load"
sudo modprobe -r corsair_bragi 2>/dev/null || true
sudo modprobe corsair_bragi

step "Bind every hub"
# hid-core moves a device from hid-generic to a new driver only once, so after
# a module reload the hub stays on hid-generic until it is rebound by hand.
# The driver then adds one more HID device per hub: the client, whose hidraw
# node programs use.
for dev in /sys/bus/hid/devices/0003:1B1C:0C3F.*; do
    [ -e "$dev" ] || continue
    id=$(basename "$dev")
    cur=$(basename "$(readlink "$dev/driver" 2>/dev/null)" 2>/dev/null || true)
    [ "$cur" = corsair-bragi ] && continue
    if [ -n "$cur" ]; then
        echo "$id" | sudo tee "/sys/bus/hid/drivers/$cur/unbind" >/dev/null
    fi
    echo "$id" | sudo tee /sys/bus/hid/drivers/corsair-bragi/bind >/dev/null
done
sleep 1
for dev in /sys/bus/hid/devices/0003:1B1C:0C3F.*; do
    [ -e "$dev" ] || continue
    printf '%s -> %s %s\n' "$(basename "$dev")" "$(basename "$(readlink "$dev/driver")")" \
        "$(find "$dev/hidraw" -mindepth 1 -maxdepth 1 -printf '%f ' 2>/dev/null)"
done
