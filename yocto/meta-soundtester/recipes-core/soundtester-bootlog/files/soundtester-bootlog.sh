#!/bin/sh
# Bring-up aid for a board with no serial console. Development image only.
#
#   soundtester-bootlog up         the system finished booting: status LED solid on
#   soundtester-bootlog emergency  it fell into emergency mode: status LED blinks fast
#
# Either way it writes what a serial console would have shown to bootlog.txt on the SD card's FAT
# boot partition, which any machine can read once the card is pulled. The boot partition is not
# mounted at runtime, so it is mounted just for the write and unmounted straight after.

set -u
stage=${1:-up}

# The board's status LED (the VIM3L's white one; the Pi has none by this name, so nothing happens
# there). Until now the kernel's heartbeat trigger has been blinking it, which only says a kernel
# is running.
for led in /sys/class/leds/*:status; do
    [ -e "$led/trigger" ] || continue
    case "$led" in *red*) continue ;; esac
    if [ "$stage" = up ]; then
        echo none > "$led/trigger"
        cat "$led/max_brightness" > "$led/brightness"
    else
        echo timer > "$led/trigger"
        echo 100 > "$led/delay_on" 2>/dev/null
        echo 100 > "$led/delay_off" 2>/dev/null
    fi
done

dev=/dev/disk/by-label/boot
mnt=/run/soundtester-bootpart
[ -e "$dev" ] || { echo "no $dev" >&2; exit 1; }
mkdir -p "$mnt"
mount -t vfat "$dev" "$mnt" || exit 1

have() { command -v "$1" >/dev/null 2>&1; }
section() { echo; echo "--- $1"; }

# --- Ethernet --------------------------------------------------------------------------------
# The wired interface, and its PHY's registers through phytool (MII ioctls). The RTL8211F keeps
# vendor registers in pages selected through register 0x1f; a paged read saves and restores the
# page so the kernel's own polling reads the page it expects.
eth=$(for n in /sys/class/net/e*; do [ -e "$n/device" ] && basename "$n" && break; done)
phyreg()  { phytool read "$eth/0/$1" 2>&1; }
phypage() { # page reg
    old=$(phytool read "$eth/0/0x1f" 2>/dev/null)
    phytool write "$eth/0/0x1f" "$1" && phytool read "$eth/0/$2" 2>&1
    phytool write "$eth/0/0x1f" "${old:-0}"
}
phypage_write() { # page reg value
    old=$(phytool read "$eth/0/0x1f" 2>/dev/null)
    phytool write "$eth/0/0x1f" "$1" && phytool write "$eth/0/$2" "$3"
    phytool write "$eth/0/0x1f" "${old:-0}"
}
carrier() { cat "/sys/class/net/$eth/carrier" 2>/dev/null || echo "?"; }
phy_dump() {
    echo "carrier=$(carrier) operstate=$(cat /sys/class/net/$eth/operstate 2>/dev/null)"
    have phytool || { echo "(no phytool)"; return; }
    for r in 0 1 4 5 6 9 10 15; do echo "  reg $r: $(phyreg $r)"; done
    echo "  0xa43/0x18 PHYCR1 (ALDPS bits 1,2,12): $(phypage 0xa43 0x18)"
    echo "  0xa43/0x19 PHYCR2:                     $(phypage 0xa43 0x19)"
    echo "  0xa43/0x1a PHYSR (link bit 2):         $(phypage 0xa43 0x1a)"
    echo "  0xa42/0x12 INER (interrupt enables):   $(phypage 0xa42 0x12)"
    echo "  0xd40/0x16 INTB/PMEB pin (bit 5=PMEB): $(phypage 0xd40 0x16)"
    echo "  0xd8a/0x10 WOL events:                 $(phypage 0xd8a 0x10)"
    echo "  0xd8a/0x11 WOL reset/RMSQ (bit 15):    $(phypage 0xd8a 0x11)"
    echo "  0xd8a/0x13 RGMII pad isolation (b15):  $(phypage 0xd8a 0x13)"
    echo "  0xd04/0x10 LED config:                 $(phypage 0xd04 0x10)"
}
eth_report() {
    [ -n "$eth" ] || { echo "no wired interface"; return; }
    echo "interface $eth"
    have ethtool && { ethtool "$eth" 2>&1; ethtool -i "$eth" 2>&1; }
    phy_dump
    section "interrupts (PHY / ethernet)"
    grep -iE "eth|gpio|mdio|dwmac" /proc/interrupts
}

# No carrier after the first 30 s: try what could leave this PHY without a link, one step at a
# time, logging the PHY after each, and stop at the first that brings the carrier up (the board is
# then on the network for the rest of this boot). Only on the first run, and only on an RTL8211F:
# the paged registers below are its own, and mean something else on any other PHY.
phy_driver() { basename "$(readlink "/sys/class/net/$eth/phydev/driver" 2>/dev/null)" 2>/dev/null; }
eth_recover() {
    [ -n "$eth" ] && have phytool || return
    case "$(phy_driver)" in
        *RTL8211F*) ;;
        *) echo "PHY driver '$(phy_driver)' is not an RTL8211F: nothing to try"; return ;;
    esac
    [ -e /run/soundtester-bootlog-recovered ] && { echo "(recovery already attempted this boot)"; return; }
    touch /run/soundtester-bootlog-recovered
    [ "$(carrier)" = 1 ] && { echo "carrier is up, nothing to recover"; return; }
    try() { # description, then the commands as the remaining arguments
        what=$1; shift
        echo; echo ">>> $what"
        "$@"
        sleep 8
        phy_dump
        [ "$(carrier)" = 1 ] && { echo "<<< carrier UP after: $what"; return 0; }
        return 1
    }
    step_aneg()  { ethtool -r "$eth"; }
    step_wol()   { # what a vendor kernel's WoL shutdown leaves set (see the kernel patch)
                   phypage_write 0xd8a 0x10 0
                   phypage_write 0xd8a 0x11 $(( $(phypage 0xd8a 0x11 | tail -1) & ~0x8000 ))
                   phypage_write 0xd8a 0x13 $(( $(phypage 0xd8a 0x13 | tail -1) & ~0x8000 ))
                   phypage_write 0xd40 0x16 $(( $(phypage 0xd40 0x16 | tail -1) & ~0x20 ))
                   ethtool -r "$eth"; }
    step_aldps() { phypage_write 0xa43 0x18 $(( $(phypage 0xa43 0x18 | tail -1) & ~0x1006 )); ethtool -r "$eth"; }
    step_bounce(){ ip link set "$eth" down; sleep 1; ip link set "$eth" up; }
    step_100()   { ethtool -s "$eth" speed 100 duplex full autoneg off; }
    try "restart autonegotiation" step_aneg && return
    try "clear WoL events, pad isolation and set the pin to INTB" step_wol && return
    try "turn ALDPS power saving off" step_aldps && return
    try "interface down/up (re-runs the PHY driver's config_init)" step_bounce && return
    try "force 100 Mbit/s full duplex" step_100 && return
    ethtool -s "$eth" autoneg on
    echo "<<< no step brought the carrier up"
}

{
    echo "=== soundtester bootlog ($stage), uptime $(cut -d' ' -f1 /proc/uptime) s, $(date -u)"
    uname -a
    cat /proc/cmdline
    section "ip addr";         ip addr 2>&1
    section "ip route";        ip route 2>&1
    have networkctl && { section "networkctl"; networkctl status --no-pager 2>&1 | head -n 80; }
    section "ethernet";        eth_report 2>&1
    [ "$stage" = up ] && { section "ethernet recovery"; eth_recover 2>&1; }
    section "ip addr after";   ip addr show "$eth" 2>&1
    section "failed units";    systemctl --failed --no-pager 2>&1
    section "mounts";          cat /proc/mounts
    have aplay && { section "aplay -l"; aplay -l 2>&1; section "arecord -l"; arecord -l 2>&1; }
    section "/proc/asound/cards"; cat /proc/asound/cards 2>&1
    section "drm connectors";  for c in /sys/class/drm/card*-*; do [ -e "$c/status" ] && echo "$c: $(cat "$c/status")"; done
    section "dmesg";           dmesg 2>&1
    section "journal";         journalctl -b --no-pager -o short-monotonic 2>&1 | tail -n 2000
} > "$mnt/bootlog.txt" 2>&1
echo "$(cut -d' ' -f1 /proc/uptime) s  $stage" >> "$mnt/bootlog-history.txt"

sync
umount "$mnt"
