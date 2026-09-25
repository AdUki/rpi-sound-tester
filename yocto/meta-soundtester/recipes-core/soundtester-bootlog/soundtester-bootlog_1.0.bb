SUMMARY = "Bring-up aid: boot log on the SD card's boot partition, and a status LED that says so"
DESCRIPTION = "For boards without a serial console. After boot (and from emergency mode) it \
writes dmesg, the journal, the network state and the sound cards to bootlog.txt on the FAT boot \
partition, and turns the status LED solid (booted) or fast-blinking (emergency mode). \
Development image only."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://soundtester-bootlog.sh \
    file://soundtester-bootlog.service \
    file://soundtester-bootlog.timer \
    file://soundtester-bootlog-emergency.service \
"

S = "${WORKDIR}"

inherit systemd allarch

# ethtool and phytool for the Ethernet PHY report and recovery steps.
RDEPENDS:${PN} = "ethtool phytool"

SYSTEMD_SERVICE:${PN} = "soundtester-bootlog.timer soundtester-bootlog-emergency.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${bindir} ${D}${systemd_system_unitdir}
    install -m 0755 ${WORKDIR}/soundtester-bootlog.sh ${D}${bindir}/soundtester-bootlog
    install -m 0644 ${WORKDIR}/soundtester-bootlog.service ${WORKDIR}/soundtester-bootlog.timer \
        ${WORKDIR}/soundtester-bootlog-emergency.service ${D}${systemd_system_unitdir}/
}

FILES:${PN} += "${systemd_system_unitdir}"
