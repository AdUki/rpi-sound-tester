SUMMARY = "Sound Tester appliance — read-only production image"
LICENSE = "MIT"

inherit core-image

IMAGE_FEATURES += "read-only-rootfs"
IMAGE_FEATURES += "${@bb.utils.contains('SOUNDTESTER_ENABLE_SSH', '1', 'ssh-server-openssh', '', d)}"

IMAGE_INSTALL = " \
    packagegroup-core-boot \
    kernel-modules \
    soundtesterd \
    soundtester-base \
    alsa-lib \
    avahi-daemon \
    ${SOUNDTESTER_WIFI_FIRMWARE} \
    ${CORE_IMAGE_EXTRA_INSTALL} \
"

# kernel-modules is not optional here. The Octo's driver
# (snd-soc-audioinjector-octo-soundcard, plus the cs42xx8 codec and bcm2835-i2s) is built as
# a module, and packagegroup-core-boot installs none. The machine config *recommends*
# kernel-modules through MACHINE_EXTRA_RRECOMMENDS, but only packagegroup-base honours that,
# and this image does not install it — without this line the image builds cleanly with an
# empty /lib/modules and neither the sound card nor the Wi-Fi driver appears.

# The Pi machine configs list the right Broadcom firmware for their board in
# MACHINE_EXTRA_RRECOMMENDS, but that variable is only honoured by packagegroup-base, which
# this minimal image does not install — the firmware goes missing with no build-time error
# and Wi-Fi fails at runtime. Take the machine's own list (correct per board: bcm43430/43455
# on a Pi 3, 43455/43456 on a Pi 4) and install it outright, but only when Wi-Fi was
# actually configured.
SOUNDTESTER_WIFI_FIRMWARE = "${@' '.join(p for p in (d.getVar('MACHINE_EXTRA_RRECOMMENDS') or '').split() if p.startswith('linux-firmware')) if d.getVar('SOUNDTESTER_WIFI_SSID') else ''}"

# Under read-only-rootfs every pkg_postinst must run at rootfs time; the build fails
# otherwise.
IMAGE_LINGUAS = ""

# The root password is baked in at build time and cannot be changed on the running device:
# /etc lives in the read-only rootfs.
#
# NOT extrausers/"usermod -P": in current shadow, -P means --prefix, so OE's old plaintext
# spelling now fails with "usermod: prefix must be an absolute path". chpasswd takes the
# plaintext directly and hashes it itself, which also keeps us out of the business of
# hashing on the host (Python's crypt module is gone in 3.13).
set_root_password() {
    echo "root:${SOUNDTESTER_ROOT_PASSWORD}" | chpasswd -R ${IMAGE_ROOTFS}
}
ROOTFS_POSTPROCESS_COMMAND += "set_root_password;"
PACKAGE_INSTALL:append = " base-passwd shadow"

# oe-core's read_only_rootfs_hook sees no host keys in /etc/ssh and therefore points sshd at
# sshd_config_readonly, whose keys live in /var/run/ssh — a tmpfs, so they would be
# regenerated on every boot and every login would warn that the host key changed. We keep the
# keys on the writable /data partition instead (see soundtester-base), so send sshd back to
# the normal config, which honours our sshd_config.d drop-in.
#
# systemd's EnvironmentFile takes the LAST definition of a variable, so these lines must be
# appended after the hook's. IMAGE_PREPROCESS_COMMAND (start of do_image) is used rather than
# ROOTFS_POSTPROCESS_COMMAND because the hook lands late in the postprocess list and would
# otherwise overwrite us — measured: ours ran 5th, the hook 18th.
soundtester_ssh_persistent_keys() {
    if [ -f ${IMAGE_ROOTFS}${sysconfdir}/default/ssh ]; then
        echo "SYSCONFDIR=/etc/ssh" >> ${IMAGE_ROOTFS}${sysconfdir}/default/ssh
        echo "SSHD_OPTS=" >> ${IMAGE_ROOTFS}${sysconfdir}/default/ssh
    fi
}
IMAGE_PREPROCESS_COMMAND += "${@bb.utils.contains('SOUNDTESTER_ENABLE_SSH', '1', 'soundtester_ssh_persistent_keys;', '', d)}"

WKS_FILE = "soundtester.wks"

IMAGE_FSTYPES = "wic.bz2 wic.bmap"

# Do NOT let wic rewrite /etc/fstab. Without this, wic appends a line per mounted partition
# to the fstab inside the image: it derives the device name from --ondisk verbatim, and it
# appends a second /data entry with plain "defaults" (no nofail) that overrides base-files'
# LABEL=data line, because systemd's fstab-generator lets the last entry for a mountpoint
# win. Two mounts on wrong or unprotected devices → local-fs.target fails →
# OnFailure=emergency.target cancels the whole boot past sysinit — while systemd-networkd
# (DefaultDependencies=no) keeps answering ping and DHCP. The per-partition
# --no-fstab-update flag in the .wks does NOT prevent this: scarthgap's update_fstab() never
# consults it (it only controls which partitions receive a copy of the updated fstab). Only
# this imager-level switch turns the rewrite off. base-files owns the one and only /data
# entry; /boot is deliberately not mounted at runtime — the GPU firmware reads it before
# Linux starts, nothing else needs it.
WIC_CREATE_EXTRA_ARGS += "--no-fstab-update"

# A bench instrument does not need a GPU stack.
DISABLE_VC4GRAPHICS = "1"

export IMAGE_BASENAME = "soundtester-image"
