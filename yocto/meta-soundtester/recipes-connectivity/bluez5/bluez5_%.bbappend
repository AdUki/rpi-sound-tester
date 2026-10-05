# BlueZ for the device's two A2DP roles: a speaker paired here plays the daemon's Bluetooth output,
# and a phone playing to the device arrives as an input. bluez-alsa carries the audio; soundtesterd
# does pairing and device management itself over D-Bus.

# meta-raspberrypi adds pi-bluetooth to every rpi build. That is btuart/hciattach and an
# hciuart.service ordered after dev-serial1.device — the userspace way of attaching the radio. This
# image attaches it with the kernel's own serdev driver instead (dtparam=krnbt=on, see
# rpi-config_%.bbappend), so the UART has no tty at all: hciattach would have nothing to open, and
# the unit would sit waiting 90 s for a device that never appears.
RDEPENDS:${PN}:remove:rpi = "pi-bluetooth"

# Only what an audio device uses. obex brings libical and an obexd nobody would talk to; the
# network, HID and HoG profiles would let a keyboard or a PAN client attach to a bench instrument
# that is meant to be a speaker. readline stays: it builds bluetoothctl and btmgmt.
PACKAGECONFIG:remove = "obex-profiles network-profiles hid-profiles hog-profiles"

# btmgmt is the one tool the image needs (soundtester-bluetooth gives the radio its address with
# it), and upstream leaves it among the uninstalled tools, which bluez5.inc packages all together
# as -noinst-tools. Its own package, listed ahead of that one, so it can be installed alone.
PACKAGES =+ "${PN}-btmgmt"
FILES:${PN}-btmgmt = "${bindir}/btmgmt"

# BlueZ's make install ships no main.conf (conf_DATA is empty in 5.72) and bluez5.inc installs only
# network.conf and input.conf, so bluetoothd runs on compiled-in defaults. Install the upstream file
# with three of its commented-out defaults turned on:
#
#   Class = 0x000414           Audio/Video, loudspeaker. Phones filter their scan by class and
#                              list the tester as a speaker; BlueZ adds the service bits itself.
#   FastConnectable = true     a phone reconnects in about a second rather than several. Costs
#                              power, which a bench instrument on a wall supply has.
#   JustWorksRepairing = confirm
#                              a reflash wipes /data and the pairings with it, but the phone still
#                              holds its key. "never" would refuse it until someone found the
#                              phone's "forget device"; "confirm" asks soundtesterd's agent, which
#                              puts the question in the console.
#
# Each edit is checked: a BlueZ upgrade that rewords a line would otherwise leave the default in
# force with nothing to say so.
do_install:append() {
    install -m 0644 ${S}/src/main.conf ${D}${sysconfdir}/bluetooth/main.conf
    sed -i -e 's|^#Class = 0x000100$|Class = 0x000414|' \
           -e 's|^#FastConnectable = false$|FastConnectable = true|' \
           -e 's|^#JustWorksRepairing = never$|JustWorksRepairing = confirm|' \
           ${D}${sysconfdir}/bluetooth/main.conf
    for l in 'Class = 0x000414' 'FastConnectable = true' 'JustWorksRepairing = confirm'; do
        grep -qx "$l" ${D}${sysconfdir}/bluetooth/main.conf || \
            bbfatal "main.conf: could not set '$l' — the upstream file has changed"
    done
}
