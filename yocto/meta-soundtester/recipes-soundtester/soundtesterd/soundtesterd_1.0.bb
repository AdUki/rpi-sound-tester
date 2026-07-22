SUMMARY = "Multichannel audio test appliance daemon (Audio Injector Octo)"
DESCRIPTION = "Full-duplex 6-in/8-out audio engine with a web admin console: routing, \
signal generators, spectrum/THD+N analysis and sample-accurate multiroom delay measurement."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# libopus (meta-oe; pulls ne10 transitively on armv7) + libogg (oe-core) for the encoded listen
# stream. RDEPENDS puts the shared libraries into the read-only image, mirroring alsa-lib.
# md2html-native is the build-time tool that renders docs/api.md to www/api.html for GET /api; it
# is native-only, so it appears in DEPENDS but not RDEPENDS (nothing of it ships to the device).
DEPENDS = "alsa-lib libopus libogg md2html-native"
RDEPENDS:${PN} = "alsa-lib libopus libogg"

# The daemon is built from app/ in this repository: the layer sits at yocto/meta-soundtester,
# so four levels up from this recipe is the repo root. Swap this for a git:// SRC_URI if the
# code ever moves to its own repository.
#
# app/third_party/* are git submodules, and file://app copies whatever is checked out there —
# so an uninitialised clone fails in do_configure, where CMake says which command to run.
#
# file://docs is staged too (as ${WORKDIR}/docs, i.e. ${S}/../docs) but never installed: the build
# renders docs/api.md to www/api.html (with md2html-native) so GET /api can serve it, and only that
# HTML ships. Without it, do_configure fails — see app/CMakeLists.txt.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:${THISDIR}/../../../../:"

SRC_URI = " \
    file://app \
    file://docs \
    file://soundtesterd.service \
"

S = "${WORKDIR}/app"

inherit cmake systemd pkgconfig

SYSTEMD_SERVICE:${PN} = "soundtesterd.service"
SYSTEMD_AUTO_ENABLE = "enable"

# `file://app` copies the working tree verbatim, which includes whatever `make build` left
# in app/build — a CMake cache full of host paths. Drop it before configuring so a local
# desktop build can never leak into the image.
do_configure:prepend() {
    rm -rf ${S}/build ${S}/build-vec
}

# Release only; the NEON float-vectorization flag for 32-bit ARM is auto-detected in
# app/CMakeLists.txt (ST_UNSAFE_MATH defaults on for arm32).
EXTRA_OECMAKE = "-DCMAKE_BUILD_TYPE=Release"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    sed -e 's|@PORT@|${SOUNDTESTER_HTTP_PORT}|' \
        ${WORKDIR}/soundtesterd.service \
        > ${D}${systemd_system_unitdir}/soundtesterd.service
    chmod 0644 ${D}${systemd_system_unitdir}/soundtesterd.service

    # The engine opens the card with whatever rate/period config.json says, so patch the
    # shipped config to match the image settings.
    sed -i -e 's|"rate": *[0-9]*|"rate": ${SOUNDTESTER_RATE}|' \
           -e 's|"period": *[0-9]*|"period": ${SOUNDTESTER_PERIOD}|' \
           ${D}${sysconfdir}/soundtester/config.json
}

FILES:${PN} += " \
    ${datadir}/soundtester \
    ${sysconfdir}/soundtester \
    ${systemd_system_unitdir} \
"

do_install[vardeps] += "SOUNDTESTER_HTTP_PORT SOUNDTESTER_RATE SOUNDTESTER_PERIOD"
