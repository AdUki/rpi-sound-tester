# On MACHINE=raspberrypi3-64 the rpi-config recipe hard-codes "dtparam=audio=on" in a
# do_deploy:append with no variable to turn it off, and it lands after RPI_EXTRA_CONFIG.
#
# That line used to be countered with "dtparam=audio=off" here, to keep the onboard audio out of
# the way. It is now wanted: the Pi's HDMI audio is the daemon's second output, and local.conf
# turns it on for every machine through RPI_EXTRA_CONFIG. The firmware audio block is not the
# I2S the Octo uses and shares no pins with it, so there is nothing left to append; this file
# stays as the record of why the 64-bit machine's forced line is harmless.
