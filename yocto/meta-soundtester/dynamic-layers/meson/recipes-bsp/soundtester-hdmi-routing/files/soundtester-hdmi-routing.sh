#!/bin/sh
# Routes the Khadas VIM3L's sound card so its HDMI output plays and its TDM loopback records.
#
# The mainline Amlogic card is a DAPM graph with every path open at boot, so PCM 0 (FRDDR_A) plays
# into nothing and HDMI stays silent until these controls are set:
#
#   FRDDR_A -> TDMOUT_A -> TOHDMITX -> dw-hdmi        (playback, PCM 0)
#   TDM_A loopback -> TDMIN_A (IN 13 on SM1) -> TODDR_A   (capture, PCM 3: what HDMI was sent)
#
# Development image only, for bring-up with alsa-utils: the daemon will set the same controls
# itself through alsa-lib, so the production image needs neither this nor amixer.

set -u

# The board's own card is the first one that is not USB. It is built into the kernel, but give it
# a moment anyway.
card=""
for _ in 1 2 3 4 5 6 7 8 9 10; do
    for c in /proc/asound/card[0-9]*; do
        [ -d "$c" ] || continue
        [ -e "$c/usbid" ] && continue
        card=${c##*/card}
        break
    done
    [ -n "$card" ] && break
    sleep 1
done
if [ -z "$card" ]; then
    echo "no on-board sound card found" >&2
    exit 1
fi

failed=0
set_ctl() {
    if ! amixer -q -c "$card" cset name="$1" "$2"; then
        echo "card $card: could not set '$1' to '$2'" >&2
        failed=1
    fi
}

set_ctl 'FRDDR_A SINK 1 SEL'          'OUT 0'
set_ctl 'FRDDR_A SRC 1 EN Switch'     on
set_ctl 'TDMOUT_A SRC SEL'            'IN 0'
set_ctl 'TOHDMITX I2S SRC'            'I2S A'
set_ctl 'TOHDMITX I2S OUT EN Switch'  on
set_ctl 'TDMIN_A SRC SEL'             'IN 13'
set_ctl 'TODDR_A SRC SEL'             'IN 0'

exit $failed
