# Khadas VIM3L kernel additions, on top of meta-meson's meson64-kmeta (which already builds the
# whole Amlogic sound stack in: the AXG sound card, FRDDR/TODDR, TDM, TOHDMITX, hdmi-codec).

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append:khadas-vim3l = " file://vim3l-soundtester.cfg"

# The HDMI DRM driver is what carries HDMI audio. meta-meson adds its fragment only when x11 or
# wayland is a DISTRO_FEATURE; a distro without them would silently lose HDMI, so ask for it
# ourselves then (not in addition, or the fragment would be applied twice).
KERNEL_FEATURES:append:khadas-vim3l = "${@'' if bb.utils.contains_any('DISTRO_FEATURES', 'x11 wayland', True, False, d) else ' cfg/meson-hdmi.scc'}"
