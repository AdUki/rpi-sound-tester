# BOARD=rpi3 — Raspberry Pi 2/3 with the Audio Injector Octo. Included by the Makefile.
#
# MACHINE=raspberrypi3 (32-bit) on purpose:
#   - its defconfig (bcm2709_defconfig) already carries CONFIG_SND_AUDIOINJECTOR_OCTO_SOUNDCARD=m
#   - unlike raspberrypi3-64, it does not hard-code dtparam=audio into config.txt, so the
#     RPI_EXTRA_CONFIG in yocto/conf/boards/rpi3.conf is the only thing that decides it
# A Pi 4 works too (MACHINE=raspberrypi4). The Octo has no working configuration on a Pi 5.
MACHINE := raspberrypi3

# The daemon's board profile, written into the build's auto.conf as SOUNDTESTER_BOARD.
PROFILE := rpi3-octo

# The BSP layer: cloned into yocto/layers/ by the Makefile and added to bblayers.conf.
BSP_LAYERS := $(LAYERS)/meta-raspberrypi
