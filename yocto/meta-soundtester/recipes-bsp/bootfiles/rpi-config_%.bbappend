# On MACHINE=raspberrypi3-64 the rpi-config recipe hard-codes "dtparam=audio=on" in a
# do_deploy:append with no variable to turn it off, and it lands after RPI_EXTRA_CONFIG.
# Append our own line afterwards so the onboard audio block stays out of the way of the
# Octo's I2S. (The 32-bit raspberrypi3 machine emits no dtparam=audio line at all.)
do_deploy:append:raspberrypi3-64() {
    echo "dtparam=audio=off" >> ${DEPLOYDIR}/bcm2835-bootfiles/config.txt
}
