# BOARD=vim3l — Khadas VIM3L (Amlogic S905D3), no add-on card: HDMI out and USB audio.
# Included by the Makefile.
#
# meta-meson's khadas-vim3l, not khadas-vim3l-sdboot: the former is mainline U-Boot plus the
# signed Amlogic FIP and linux-yocto; the latter boots through the vendor U-Boot and an
# end-of-life 6.5 kernel.
MACHINE := khadas-vim3l

# The daemon's board profile, written into the build's auto.conf as SOUNDTESTER_BOARD.
PROFILE := vim3l

# The BSP layer: cloned into yocto/layers/ by the Makefile and added to bblayers.conf.
BSP_LAYERS := $(LAYERS)/meta-meson
