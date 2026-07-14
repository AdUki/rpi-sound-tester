SUMMARY = "Sound Tester appliance base configuration: network, hostname, ssh, storage"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://20-wired.network \
    file://25-wireless.network \
    file://wpa_supplicant-wlan0.conf \
    file://journald-volatile.conf \
    file://10-soundtester.conf \
    file://soundtester-sshkeys.sh \
    file://soundtester-sshkeys.service \
    file://sshd-early.conf \
    file://sshd-at-early.conf \
    file://sshdgenkeys-early.conf \
"

S = "${WORKDIR}"

inherit systemd allarch

RDEPENDS:${PN} = "avahi-daemon"
RDEPENDS:${PN} += "${@bb.utils.contains('SOUNDTESTER_ENABLE_SSH', '1', 'openssh-sshd openssh-scp openssh-sftp-server', '', d)}"

# Only wpa-supplicant is needed here: the Pi machine configs already pull the Broadcom
# firmware in through MACHINE_EXTRA_RRECOMMENDS. That firmware sits behind
# LICENSE_FLAGS = "synaptics-killswitch", and because a RRECOMMENDS is soft, a build that
# has not accepted the flag installs no firmware and says nothing — Wi-Fi then fails at
# runtime with no clue why. local.conf.sample accepts the flag; see the note there.
RDEPENDS:${PN} += "${@'wpa-supplicant' if d.getVar('SOUNDTESTER_WIFI_SSID') else ''}"

SYSTEMD_SERVICE:${PN} = "${@bb.utils.contains('SOUNDTESTER_ENABLE_SSH', '1', 'soundtester-sshkeys.service', '', d)}"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    # --- wired: always DHCP -------------------------------------------------
    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${WORKDIR}/20-wired.network ${D}${sysconfdir}/systemd/network/

    # --- wireless: only when an SSID was configured --------------------------
    if [ -n "${SOUNDTESTER_WIFI_SSID}" ]; then
        install -m 0644 ${WORKDIR}/25-wireless.network ${D}${sysconfdir}/systemd/network/
        install -d ${D}${sysconfdir}/wpa_supplicant
        sed -e 's|@SSID@|${SOUNDTESTER_WIFI_SSID}|' \
            -e 's|@PSK@|${SOUNDTESTER_WIFI_PSK}|' \
            -e 's|@COUNTRY@|${SOUNDTESTER_WIFI_COUNTRY}|' \
            ${WORKDIR}/wpa_supplicant-wlan0.conf \
            > ${D}${sysconfdir}/wpa_supplicant/wpa_supplicant-wlan0.conf
        chmod 0600 ${D}${sysconfdir}/wpa_supplicant/wpa_supplicant-wlan0.conf

        install -d ${D}${sysconfdir}/systemd/system/multi-user.target.wants
        ln -sf ${systemd_system_unitdir}/wpa_supplicant@.service \
            ${D}${sysconfdir}/systemd/system/multi-user.target.wants/wpa_supplicant@wlan0.service
    fi

    # The hostname is NOT written here: base-files already owns /etc/hostname, and two
    # packages shipping the same file makes opkg refuse to build the rootfs
    # ("check_data_file_clashes"). It is set through base-files' own `hostname` variable
    # instead — see recipes-core/base-files/base-files_%.bbappend.

    # --- logs stay in RAM: the rootfs is read-only ---------------------------
    install -d ${D}${sysconfdir}/systemd/journald.conf.d
    install -m 0644 ${WORKDIR}/journald-volatile.conf ${D}${sysconfdir}/systemd/journald.conf.d/

    # --- ssh ------------------------------------------------------------------
    # A drop-in, not a replacement sshd_config: openssh-sshd already owns that path and two
    # packages shipping the same file makes the rootfs build fail.
    if [ "${SOUNDTESTER_ENABLE_SSH}" = "1" ]; then
        install -d ${D}${sysconfdir}/ssh/sshd_config.d
        install -m 0644 ${WORKDIR}/10-soundtester.conf ${D}${sysconfdir}/ssh/sshd_config.d/
        install -d ${D}${bindir}
        install -m 0755 ${WORKDIR}/soundtester-sshkeys.sh ${D}${bindir}/soundtester-sshkeys
        install -d ${D}${systemd_system_unitdir}
        install -m 0644 ${WORKDIR}/soundtester-sshkeys.service ${D}${systemd_system_unitdir}/

        # Drop-ins that pull ssh in front of sysinit.target, so a broken boot stays
        # reachable instead of dying silently in emergency.target.
        install -d ${D}${systemd_system_unitdir}/sshd.socket.d
        install -m 0644 ${WORKDIR}/sshd-early.conf ${D}${systemd_system_unitdir}/sshd.socket.d/10-early.conf
        install -d ${D}${systemd_system_unitdir}/sshd@.service.d
        install -m 0644 ${WORKDIR}/sshd-at-early.conf ${D}${systemd_system_unitdir}/sshd@.service.d/10-early.conf
        install -d ${D}${systemd_system_unitdir}/sshdgenkeys.service.d
        install -m 0644 ${WORKDIR}/sshdgenkeys-early.conf ${D}${systemd_system_unitdir}/sshdgenkeys.service.d/10-early.conf
    fi
}

FILES:${PN} += " \
    ${sysconfdir}/systemd/network \
    ${sysconfdir}/systemd/journald.conf.d \
    ${sysconfdir}/systemd/system \
    ${sysconfdir}/wpa_supplicant \
    ${sysconfdir}/ssh \
    ${systemd_system_unitdir} \
"

# The hostname and Wi-Fi credentials are baked in, so the package must be rebuilt when
# they change.
do_install[vardeps] += "SOUNDTESTER_HOSTNAME SOUNDTESTER_WIFI_SSID SOUNDTESTER_WIFI_PSK \
                        SOUNDTESTER_WIFI_COUNTRY SOUNDTESTER_ENABLE_SSH"
