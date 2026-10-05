#!/bin/sh
# Get the Bluetooth radio ready for bluetoothd, which starts right after this. Two jobs:
#
# 1. Bring back the pairings. BlueZ keeps its link keys under /var/lib/bluetooth, and on this
#    read-only image /var/lib is an overlay on a tmpfs: every boot would forget every phone and
#    speaker. soundtesterd copies that directory to /data/bluetooth whenever a pairing changes;
#    this puts it back before bluetoothd looks.
#
# 2. Give the controller an address of its own. A Pi 3B's BCM43430A1 comes up with the chip's
#    default 43:43:A1:12:1F:AC (a 3B+'s BCM4345C0 with 43:45:C0:00:1F:AC). This kernel's btbcm does
#    not treat those as invalid, and the 5.15 device tree has no `bluetooth` alias for the firmware
#    to write a real one through — so every board would share one address, and BlueZ files its
#    pairings under the adapter's address. The address is derived from the board's serial number
#    exactly as RPi-Distro's bthelper does it, so a board gets the same one it would under
#    Raspberry Pi OS. The kernel only takes a new address while the controller is powered off, which
#    it is until bluetoothd starts.
#
# Never fails the boot: Bluetooth is one feature of the device, not a reason to lose the others.
# Every problem is logged and the script exits 0.

STATE=/var/lib/bluetooth
SAVED=/data/bluetooth
HCI=hci0

log() { echo "soundtester-bluetooth: $*"; }

# --- 1. pairings ---------------------------------------------------------------------------
mkdir -p "$STATE"
if [ -d "$SAVED" ]; then
    if cp -a "$SAVED/." "$STATE/"; then
        log "restored pairings from $SAVED"
    else
        log "could not restore pairings from $SAVED"
    fi
fi
# After the copy: cp -a gives the directory the mode of the one it came from. Link keys are secrets.
chmod 700 "$STATE"

# --- 2. address ----------------------------------------------------------------------------

# hci0's address, or nothing — and nothing while it reads all zeros, which is what it reports until
# the kernel has loaded the chip's firmware and asked it. hciconfig asks the kernel with one ioctl and exits. Not btmgmt: in
# non-interactive mode its `info` never exits without a terminal, and what it printed is still in
# its buffer when it is killed.
read_addr() {
    hciconfig "$HCI" 2>/dev/null | sed -n 's/.*BD Address: \([0-9A-Fa-f:]\{17\}\).*/\1/p' |
        grep -v '^00:00:00:00:00:00$' | head -n 1
}

# Runs a command with a bound, since busybox has no timeout(1) here: btmgmt talks to the kernel
# directly and should always answer, but a tool that hangs here would hold bluetoothd back with it.
bounded() {
    "$@" > /dev/null 2>&1 &
    pid=$!
    i=0
    while kill -0 "$pid" 2>/dev/null; do
        i=$((i + 1))
        if [ "$i" -gt 5 ]; then
            kill "$pid" 2>/dev/null
            break
        fi
        sleep 1
    done
    wait "$pid" 2>/dev/null
}

# hci0 is registered before the kernel has loaded the chip's firmware and read its address — so
# wait for an address, not just for the device.
addr=""
i=0
while [ "$i" -lt 15 ]; do
    if [ -e /sys/class/bluetooth/$HCI ]; then
        addr=$(read_addr)
        [ -n "$addr" ] && break
    fi
    i=$((i + 1))
    sleep 1
done
if [ -z "$addr" ]; then
    log "no Bluetooth controller answered within 15 s — leaving it alone"
    exit 0
fi

case "$addr" in
    43:4[35]:*) ;;
    *)
        log "$HCI has address $addr"
        exit 0
        ;;
esac

serial=$(tr -d '\000' < /proc/device-tree/serial-number 2>/dev/null)
tail6=${serial#"${serial%??????}"}
case "$tail6" in
    [0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]) ;;
    *)
        log "$HCI has the chip's default address $addr and no serial number to derive one from"
        exit 0
        ;;
esac
b1=$(echo "$tail6" | cut -c1-2)
b2=$(echo "$tail6" | cut -c3-4)
b3=$(echo "$tail6" | cut -c5-6)
new=$(printf 'B8:27:EB:%02X:%02X:%02X' $((0x$b1 ^ 0xaa)) $((0x$b2 ^ 0xaa)) $((0x$b3 ^ 0xaa)))

# Its exit status is not worth much (it may be killed for lingering): the address read back below
# is what says whether it worked.
bounded btmgmt --index 0 public-addr "$new"

# Setting it makes the kernel run the controller's setup again, during which it drops out of the
# management interface. Wait for it to come back with the new address before bluetoothd starts.
i=0
while [ "$i" -lt 10 ]; do
    now=$(read_addr)
    if [ "$now" = "$new" ]; then
        log "$HCI address set to $new (was the chip's default $addr)"
        exit 0
    fi
    i=$((i + 1))
    sleep 1
done
log "$HCI did not come back with $new within 10 s (reads ${now:-nothing})"
exit 0
