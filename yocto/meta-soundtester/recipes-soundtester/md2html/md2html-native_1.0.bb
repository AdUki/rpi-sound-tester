SUMMARY = "Markdown -> HTML converter (build-time tool for the soundtester API reference)"
DESCRIPTION = "A pure-stdlib Python 3 script that renders docs/api.md to a static www/api.html. \
The soundtesterd build calls it so GET /api serves a pre-rendered page — no runtime renderer, and \
only the HTML ships to the read-only image."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Native-only: it runs on the build host, never on the device. python3-native is the interpreter
# its shebang needs; pulling md2html-native therefore also puts python3 on the consumer's PATH.
inherit native
RDEPENDS:${PN} += "python3-native"

# tools/ lives at the repo root — four levels up from this recipe, the same reach the daemon
# recipe uses for file://app.
FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../:"
SRC_URI = "file://tools"
S = "${WORKDIR}/tools"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/md2html ${D}${bindir}/md2html
}
