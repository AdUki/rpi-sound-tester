# Pinned Raspberry Pi 5.15 kernel.
#
# WHY THIS EXISTS: the Audio Injector Octo does not work on any 6.x Raspberry Pi kernel.
# The card probes and enumerates, but playback is a distorted rhythmic pulsing noise and
# capture is noise (Audio-Injector/Octo#64, raspberrypi/firmware#1884 — still open;
# raspberrypi/linux#6909 was closed "not planned", the vendor is inactive). 5.15.92 is the
# last kernel the community has verified working with this card.
#
# meta-raspberrypi/scarthgap ships only 6.1/6.6/6.12, so the 5.15 recipe is carried here.
# It is a copy of meta-raspberrypi's kirkstone linux-raspberrypi_5.15.bb, minus the
# kirkstone-only Wi-Fi certificate patches (they are not in scarthgap's files/ directory).
#
# UNVERIFIED: building a 5.15 tree with scarthgap's newer toolchain is the riskiest step
# in this whole image. Milestone 0 must prove it. If it fights the toolchain, build the
# whole stack on the kirkstone branch set instead (see docs/octo-known-issues.md).

LINUX_VERSION ?= "5.15.92"
LINUX_RPI_BRANCH ?= "rpi-5.15.y"
LINUX_RPI_KMETA_BRANCH ?= "yocto-5.15"

SRCREV_machine = "14b35093ca68bf2c81bbc90aace5007142b40b40"
SRCREV_meta = "509f4b9d68337f103633d48b621c1c9aa0dc975d"

KMETA = "kernel-meta"

# linux-raspberrypi.inc adds file://default-cpu-governor.cfg (and, behind MACHINE_FEATURES,
# vc4graphics.cfg / wm8960.cfg / initramfs-image-bundle.cfg) to SRC_URI. Those fragments live
# in meta-raspberrypi's own files/ directory, which is not on this recipe's FILESPATH because
# the recipe lives in meta-soundtester. They are copied into files/ here so the recipe is
# self-contained; without them bitbake fails at parse time with "file could not be found".
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI = " \
    git://github.com/raspberrypi/linux.git;name=machine;branch=${LINUX_RPI_BRANCH};protocol=https \
    git://git.yoctoproject.org/yocto-kernel-cache;type=kmeta;name=meta;branch=${LINUX_RPI_KMETA_BRANCH};destsuffix=${KMETA} \
    file://octo.cfg \
    file://gcc13-compat.cfg \
    file://0001-ASoC-audioinjector-octo-set-the-card-owner.patch \
    file://0002-ASoC-bcm2835-i2s-only-report-SYNC-error-when-clock-master.patch \
"

# Two kernel fixes, both to keep false alarms out of the kernel log.
#
# 0001: the Octo machine driver forgets .owner on its snd_soc_card, so every boot trips
#       WARN_ON(!module) in snd_card_new() and taints the kernel with a backtrace through the
#       audio probe path.
#
# 0002: bcm2835-i2s verifies its FIFO clear by polling the CS_A SYNC bit — a check its own
#       source has always annotated "FIXME: This does not seem to work for slave mode!". The
#       Octo's FPGA is the BCLK/LRCLK master, so the Pi *is* the slave, the poll always times
#       out, and "I2S SYNC error!" is printed on every stream setup. The FIFO clear is fine (it
#       needs 2 bit-clocks; the timeout spin takes far longer), only the verification is
#       impossible — so the error is reported only when we own the clock. "I2S SYNC error!" is
#       also the symptom of the card's real TDM slot-rotation fault, which this daemon raises a
#       UI banner on; a false one on every boot teaches the operator to ignore the real one.

require recipes-kernel/linux/linux-raspberrypi.inc

KERNEL_DTC_FLAGS += "-@ -H epapr"

COMPATIBLE_MACHINE = "^rpi$"
