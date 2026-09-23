SUMMARY = "Khadas VIM3L: route the sound card to HDMI and its loopback at boot (bring-up)"
DESCRIPTION = "Sets the Amlogic sound card's DAPM routing with amixer so HDMI plays and the TDM \
loopback records. For the development image only: the daemon applies the same routing itself."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://soundtester-hdmi-routing.sh \
    file://soundtester-hdmi-routing.service \
"

S = "${WORKDIR}"

inherit systemd allarch

RDEPENDS:${PN} = "alsa-utils-amixer"

SYSTEMD_SERVICE:${PN} = "soundtester-hdmi-routing.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${bindir} ${D}${systemd_system_unitdir}
    install -m 0755 ${WORKDIR}/soundtester-hdmi-routing.sh ${D}${bindir}/soundtester-hdmi-routing
    install -m 0644 ${WORKDIR}/soundtester-hdmi-routing.service ${D}${systemd_system_unitdir}/
}

FILES:${PN} += "${systemd_system_unitdir}"
