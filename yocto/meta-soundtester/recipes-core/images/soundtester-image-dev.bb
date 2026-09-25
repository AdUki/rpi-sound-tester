SUMMARY = "Sound Tester appliance — writable development image"
DESCRIPTION = "Same software as the production image but with a writable rootfs and the \
tools needed to bring the card up: alsa-utils (aplay/arecord/speaker-test/amixer), i2c-tools \
and a package manager. This is the image to use for milestone 0 (does the Octo work on this \
kernel at all?) and for iterating on the daemon. It is never the shipped artifact."
LICENSE = "MIT"

require soundtester-image.bb

# Deliberately writable: debugging on a read-only rootfs with no persistent logs is
# miserable.
IMAGE_FEATURES:remove = "read-only-rootfs"

IMAGE_FEATURES += "ssh-server-openssh package-management debug-tweaks"

IMAGE_INSTALL:append = " \
    alsa-utils \
    alsa-tools \
    i2c-tools \
    usbutils \
    strace \
    htop \
    soundtester-bootlog \
"

# The VIM3L's card plays nothing until its routing is set; for bring-up with alsa-utils, set it at
# boot. The recipe exists only when meta-meson is in the build, hence the machine override.
IMAGE_INSTALL:append:khadas-vim3l = " soundtester-hdmi-routing"

export IMAGE_BASENAME = "soundtester-image-dev"
