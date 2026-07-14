# base-files owns /etc/hostname (it writes it from its own `hostname` variable), so the
# hostname is set here rather than shipped as a second copy of the file from
# soundtester-base — opkg refuses to build a rootfs where two packages claim one path.
hostname = "${SOUNDTESTER_HOSTNAME}"

# The writable partition holding saved settings and the ssh host keys. It is mounted
# read-only; the daemon flips it to rw only for the moment it writes.
#
# nofail + a short device timeout + NO fsck pass are load-bearing. Without them, a /data
# that is missing, late or unfsckable fails local-fs.target, which fails sysinit.target,
# which drops the machine into emergency.target. On a headless appliance with no console
# that is indistinguishable from a hang, and nothing looks wrong from the outside:
# systemd-networkd carries DefaultDependencies=no and therefore starts *before* sysinit, so
# the box keeps answering DHCP and ping while sshd.socket, avahi and soundtesterd (all
# ordered after sysinit) never start at all.
#
# This must be the ONLY /data entry in the final image. wic's fstab rewrite is disabled
# image-wide (WIC_CREATE_EXTRA_ARGS --no-fstab-update in soundtester-image.bb): wic would
# otherwise append its own /data line with plain "defaults", and systemd's fstab-generator
# lets the last entry for a mountpoint win — silently discarding every option above.
do_install:append() {
    echo "LABEL=data  /data  ext4  ro,noatime,nofail,x-systemd.device-timeout=10s  0  0" \
        >> ${D}${sysconfdir}/fstab
    install -d ${D}/data
}

do_install[vardeps] += "SOUNDTESTER_HOSTNAME"
